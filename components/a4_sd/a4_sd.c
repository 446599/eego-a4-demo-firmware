#include "a4_sd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "fatfs_status.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#if FF_FS_EXFAT != 1
#error "SD storage requires the exFAT-enabled FatFs component"
#endif

enum {
    SD_PIN_SCK = 39,
    SD_PIN_MOSI = 38,
    SD_PIN_MISO = 40,
    SD_PIN_CS = 47,
};

#define SD_MOUNT_POINT "/sd"

static const char *TAG = "a4_sd";
static sdmmc_card_t *s_card;
static bool s_bus_ready;
static StaticSemaphore_t s_sd_lock_storage;
static SemaphoreHandle_t s_sd_lock;
static portMUX_TYPE s_sd_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t sd_lock(void)
{
    if (s_sd_lock == NULL) {
        portENTER_CRITICAL(&s_sd_lock_init_mux);
        if (s_sd_lock == NULL) {
            s_sd_lock = xSemaphoreCreateMutexStatic(&s_sd_lock_storage);
        }
        portEXIT_CRITICAL(&s_sd_lock_init_mux);
    }
    return s_sd_lock;
}

static void a4_sd_unmount_locked(void)
{
    if (s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

bool a4_sd_is_mounted(void)
{
    return s_card != NULL;
}

void a4_sd_unmount(void)
{
    SemaphoreHandle_t lock = sd_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    a4_sd_unmount_locked();
    xSemaphoreGive(lock);
}

bool a4_sd_mount(void)
{
    SemaphoreHandle_t lock = sd_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    bool result = false;
    if (s_card != NULL) {
        result = true;
        goto done;
    }

    if (!s_bus_ready) {
        const spi_bus_config_t bus_config = {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        const esp_err_t err =
            spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SPI bus init: %s", esp_err_to_name(err));
            goto done;
        }
        s_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SPI2_HOST;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        /* Directory probes and future demo-side file operations. */
        .max_files = 16,
        .allocation_unit_size = 0,
    };

    const esp_err_t err = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        goto done;
    }
    const uint8_t filesystem_type =
        fatfs_sdcard_filesystem_type(s_card);
    ESP_LOGI(TAG, "SD mounted: %llu MB",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) /
                 (1024 * 1024));
    ESP_LOGI(TAG, "filesystem=%s exfat=enabled",
             fatfs_filesystem_name(filesystem_type));
    result = true;
done:
    xSemaphoreGive(lock);
    return result;
}

static bool entry_is_hidden(const char *name)
{
    return name[0] == '.' || strcasecmp(name, "System Volume Information") == 0;
}

int a4_sd_list(const char *path, int offset, a4_sd_entry_t *out, int max_out,
               int *total_entries)
{
    if (total_entries != NULL) {
        *total_entries = 0;
    }
    if (!a4_sd_mount()) {
        return -1;
    }

    char full[A4_SD_PATH_MAX];
    int path_length;
    if (path == NULL || path[0] == '\0') {
        path_length = snprintf(full, sizeof(full), SD_MOUNT_POINT);
    } else {
        path_length =
            snprintf(full, sizeof(full), SD_MOUNT_POINT "/%s", path);
    }
    if (path_length <= 0 || (size_t)path_length >= sizeof(full)) {
        ESP_LOGW(TAG, "directory path too long: %s", path == NULL ? "" : path);
        return -1;
    }

    /* Two passes keep memory flat: count first, then fill the page window
     * with directories sorted ahead of files (stable disk order within
     * each group -- fine for a first browser). */
    DIR *dir = opendir(full);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir %s failed", full);
        return -1;
    }

    int total = 0;
    int dir_total = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (entry_is_hidden(ent->d_name)) {
            continue;
        }
        total++;
        if (ent->d_type == DT_DIR) {
            dir_total++;
        }
    }
    if (total_entries != NULL) {
        *total_entries = total;
    }

    int written = 0;
    /* group 0 = directories, group 1 = files */
    for (int group = 0; group < 2 && written < max_out; group++) {
        const int group_base = group == 0 ? 0 : dir_total;
        rewinddir(dir);
        int index_in_group = 0;
        while ((ent = readdir(dir)) != NULL && written < max_out) {
            if (entry_is_hidden(ent->d_name)) {
                continue;
            }
            const bool is_dir = ent->d_type == DT_DIR;
            if ((group == 0) != is_dir) {
                continue;
            }
            const int global_index = group_base + index_in_group;
            index_in_group++;
            if (global_index < offset) {
                continue;
            }
            a4_sd_entry_t *e = &out[written++];
            strlcpy(e->name, ent->d_name, sizeof(e->name));
            e->is_dir = is_dir;
            e->size_bytes = 0;
        }
    }
    closedir(dir);
    return written;
}
