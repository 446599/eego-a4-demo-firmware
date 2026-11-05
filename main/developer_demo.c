/*
 * Standalone EegoRead A4 developer demo boot wiring.
 *
 * This entry initializes only the board, display and shared I2C devices. The
 * app loop and separate demo pages own touch, keys, power, storage and radio
 * demonstrations. Reader, OTA, cloud and transfer services are not started.
 */

#include <stdio.h>

#include "a4_board.h"
#include "devdemo_app_loop.h"
#include "devdemo_app_registry.h"
#include "devdemo_ui.h"
#include "display_manager.h"
#include "epd_uc8279c.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "frame_rotation.h"
#include "a4_display_geometry.h"

static const char *TAG = "developer_demo";

static void draw_splash(uint8_t *fb)
{
    char version[64];
    const esp_app_desc_t *const app = esp_app_get_description();
    (void)snprintf(version, sizeof(version), "%s / loading demo pages",
                   app != NULL ? app->version : "unknown");
    devdemo_ui_clear(fb);
    devdemo_ui_header(fb, "EEGO A4 DEVELOPER DEMO", version);
    devdemo_ui_text(fb, 28, 270, 38, "Standalone hardware demonstration",
                    true);
    devdemo_ui_text(fb, 28, 320, 28,
                    "Touch, keys, display, frontlight, power, RTC, SD, radios",
                    true);
    devdemo_ui_text(fb, 28, 372, 28,
                    "No reader, OTA, cloud, account or transfer services",
                    true);
    devdemo_ui_text(fb, 28, 432, 28,
                    "Initializing display, touch controller and board...",
                    true);
}

void app_main(void)
{
    const esp_app_desc_t *const app = esp_app_get_description();
    ESP_LOGI(TAG, "EegoRead firmware boot build=%s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "firmware built %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "standalone developer demo version=%s idf=%s",
             app != NULL ? app->version : "unknown", esp_get_idf_version());

    ESP_ERROR_CHECK(a4_board_init());
    ESP_ERROR_CHECK(display_manager_init());

    uint8_t *const ui_fb =
        heap_caps_calloc(1, UI_FRAMEBUFFER_SIZE_BYTES,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *const panel_fb =
        heap_caps_calloc(1, EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ui_fb == NULL || panel_fb == NULL) {
        heap_caps_free(ui_fb);
        heap_caps_free(panel_fb);
        ESP_LOGE(TAG, "PSRAM framebuffer allocation failed");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    const epd_uc8279c_config_t display_config = {
        .host = SPI3_HOST,
        .pin_sclk = 42,
        .pin_mosi = 45,
        .pin_cs = 21,
        .pin_dc = 14,
        .pin_reset = 13,
        .pin_busy = 41,
        .clock_hz = 20 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(epd_uc8279c_init(&display_config));
    ESP_ERROR_CHECK(epd_uc8279c_set_power_hold(true));

    draw_splash(ui_fb);
    a4_rotate_to_panel(ui_fb, panel_fb, false);
    ESP_ERROR_CHECK(epd_uc8279c_show_frame(
        panel_fb, EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES));

    ESP_ERROR_CHECK(a4_board_init_i2c_devices());
    if (a4_board_has_frontlight()) {
        (void)a4_board_set_frontlight(0, 0);
    }
    devdemo_app_loop_run(devdemo_overview_page(), ui_fb, panel_fb);
}
