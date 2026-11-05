#include "a4_board.h"
#include "a4_battery_curve.h"
#include "a4_charging_logic.h"
#include "a4_frontlight_logic.h"

#include "gsl1680_fw.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/* 存入 NVS 的值保持稳定；仅在 SD 首次访问前识别并绑定主板。 */
typedef enum {
    A4_BOARD_POWER_UNKNOWN = 0,
    A4_BOARD_POWER_LEGACY = 1,
    A4_BOARD_POWER_CW2015 = 2,
} a4_board_power_profile_t;

enum {
    A4_GPIO_I2C_SCL = 1,
    A4_GPIO_I2C_SDA = 2,
    A4_GPIO_TOUCH_RESET = 3,
    A4_GPIO_MCU_POWER_ENABLE = 4,
    A4_GPIO_BUTTON_UP = 5,
    /* 新主板为低有效 SD 电源；已验证的旧主板需保持高电平以启用 ADC 通路。 */
    A4_GPIO_SD_POWER_ENABLE = 6,
    A4_GPIO_BUTTON_DOWN = 7,
    A4_GPIO_BUTTON_POWER = 8,
    A4_GPIO_TOUCH_INT = 9,
    A4_GPIO_BATTERY_ADC = 10,
    A4_GPIO_CHARGING = 11,
    A4_GPIO_FRONTLIGHT_ENABLE = 12,
    A4_GPIO_EPD_CS = 21,

    A4_RTC_ADDRESS = 0x51,
    A4_FRONTLIGHT_ADDRESS = 0x36,
    /* CW2015 电量计的 I2C 从地址是固定的 0x62。 */
    A4_FUEL_GAUGE_ADDRESS = 0x62,
    A4_CW2015_REG_VERSION = 0x00,
    A4_CW2015_REG_VCELL = 0x02,
    A4_CW2015_REG_SOC = 0x04,
    A4_CW2015_REG_CONFIG = 0x08,
    A4_CW2015_REG_MODE = 0x0a,
    A4_CW2015_REG_BATINFO = 0x10,
    A4_CW2015_BATINFO_SIZE = 64,
    A4_CW2015_CONFIG_UPDATE_FLAG = 0x02,
    A4_CW2015_MODE_RESTART_MASK = 0x0f,
    A4_CW2015_MODE_SLEEP_MASK = 0xc0,
    A4_CW2015_MODE_NORMAL = 0x00,
    A4_I2C_TIMEOUT_MS = 100,
    A4_BOARD_PROBE_ATTEMPTS = 5,
    A4_BOARD_ABSENT_CONFIRMATIONS = 3,
    A4_BOARD_PROBE_DELAY_MS = 20,
    /* PCF8563 与触摸控制器共用 I2C0。实测这颗 RTC 的读写在 400 kHz 下会偶发
     * ESP_ERR_TIMEOUT（总线只启用芯片内部上拉，400 kHz 的边沿裕量本来就紧），
     * 所以给 RTC 单独降到 100 kHz 并放宽事务超时；时钟芯片的数据量极小，
     * 降速不影响功能。 */
    A4_RTC_IO_TIMEOUT_MS = 300,
    A4_I2C_SPEED_HZ = 400000,
    A4_RTC_I2C_SPEED_HZ = 100000,
    A4_RTC_WRITE_ATTEMPTS = 3,
    A4_RTC_WRITE_RETRY_DELAY_MS = 10,
    A4_RTC_RESTART_RETRIES = 3,
    A4_RTC_RETRY_DELAY_MS = 2,
};

static const char *TAG = "a4_board";
static const uint8_t s_cw2015_batinfo_profile[A4_CW2015_BATINFO_SIZE] = {
    /* 3.7 V Li-ion/Li-polymer model used by the CW2015 reference driver.
     * The installed cell is 2000 mAh nominal; capacity is represented by
     * this model and the gauge's accumulated SOC, not by a BATINFO byte. */
    0x15, 0x7e, 0x7c, 0x5c, 0x64, 0x6a, 0x65, 0x5c,
    0x55, 0x53, 0x56, 0x61, 0x6f, 0x66, 0x50, 0x48,
    0x43, 0x42, 0x40, 0x43, 0x4b, 0x5f, 0x75, 0x7d,
    0x52, 0x44, 0x07, 0xae, 0x11, 0x22, 0x40, 0x56,
    0x6c, 0x7c, 0x85, 0x86, 0x3d, 0x19, 0x8d, 0x1b,
    0x06, 0x34, 0x46, 0x79, 0x8d, 0x90, 0x90, 0x46,
    0x67, 0x80, 0x97, 0xaf, 0x80, 0x9f, 0xae, 0xcb,
    0x2f, 0x00, 0x64, 0xa5, 0xb5, 0x11, 0xd0, 0x11,
};
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_rtc;
static i2c_master_dev_handle_t s_touch;
static i2c_master_dev_handle_t s_fuel_gauge;
static bool s_fuel_gauge_available;
static bool s_battery_discovery_complete;
static bool s_i2c_devices_initialized;
static a4_board_power_profile_t s_board_power_profile;
static adc_oneshot_unit_handle_t s_battery_adc;
static adc_cali_handle_t s_battery_adc_cali;
static int64_t s_battery_adc_retry_after_us;
static bool s_battery_present;
static int s_battery_source = -1;
static bool s_battery_percent_cache_valid;
static uint8_t s_battery_percent_cache;
static int64_t s_battery_percent_cache_time_us;
static a4_battery_percent_filter_t s_battery_percent_filter;
static a4_charging_debounce_t s_charging_debounce;
static bool s_frontlight_available;
static bool s_frontlight_rail_on;
static bool s_sd_power_initialized;
static bool s_sd_power_enabled;
static a4_board_reader_key_isr_t s_reader_key_isr_callback;
static void *s_reader_key_isr_context;
static bool s_reader_key_isr_registered;
static bool s_power_restart_handler_registered;
static a4_board_touch_int_isr_t s_touch_int_isr_callback;
static void *s_touch_int_isr_context;
static bool s_touch_int_isr_registered;
static a4_board_charging_isr_t s_charging_isr_callback;
static void *s_charging_isr_context;
static bool s_charging_isr_registered;
static int s_charging_sleep_level;
static bool s_gpio_isr_service_ready;

static bool gsl_init_firmware(void);
static bool gsl_verify_running(void);

static esp_err_t ensure_gpio_isr_service(void)
{
    if (s_gpio_isr_service_ready) {
        return ESP_OK;
    }
    const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_gpio_isr_service_ready = true;
    return ESP_OK;
}

static void IRAM_ATTR reader_key_gpio_isr(void *argument)
{
    const a4_board_reader_key_isr_t callback = s_reader_key_isr_callback;
    if (callback != NULL) {
        callback((a4_button_t)(uintptr_t)argument, s_reader_key_isr_context);
    }
}

static void IRAM_ATTR touch_int_gpio_isr(void *argument)
{
    (void)argument;
    const a4_board_touch_int_isr_t callback = s_touch_int_isr_callback;
    if (callback != NULL) {
        callback(s_touch_int_isr_context);
    }
}

static void IRAM_ATTR charging_gpio_isr(void *argument)
{
    (void)argument;
    const a4_board_charging_isr_t callback = s_charging_isr_callback;
    if (callback != NULL) {
        callback(s_charging_isr_context);
    }
}

static uint8_t bcd_to_binary(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0fU));
}

static uint8_t binary_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static bool datetime_is_valid(const a4_datetime_t *datetime)
{
    if (datetime == NULL || datetime->year < 2000 || datetime->year > 2099 ||
        datetime->month < 1 || datetime->month > 12 || datetime->day < 1 ||
        datetime->day > 31 || datetime->hour > 23 || datetime->minute > 59 ||
        datetime->second > 59) {
        return false;
    }

    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    uint8_t days = days_per_month[datetime->month - 1];
    const bool leap = (datetime->year % 4U == 0U) &&
                      ((datetime->year % 100U != 0U) ||
                       (datetime->year % 400U == 0U));
    if (datetime->month == 2 && leap) {
        days = 29;
    }
    return datetime->day <= days;
}

/* PCF8563 weekday encoding is 0=Sunday .. 6=Saturday. */
static uint8_t datetime_weekday(const a4_datetime_t *datetime)
{
    int year = datetime->year;
    int month = datetime->month;
    if (month < 3) {
        month += 12;
        year--;
    }
    const int weekday =
        (datetime->day + (13 * (month + 1)) / 5 + year + year / 4 -
         year / 100 + year / 400 + 6) % 7;
    return (uint8_t)weekday;
}

static bool datetime_minute_matches(const a4_datetime_t *actual,
                                    const a4_datetime_t *expected)
{
    return actual->year == expected->year &&
           actual->month == expected->month && actual->day == expected->day &&
           actual->hour == expected->hour && actual->minute == expected->minute;
}

static esp_err_t init_power_and_buttons(void)
{
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        /* EXT0 将电源键切到 RTC 功能；唤醒后恢复普通 GPIO 输入。 */
        ESP_RETURN_ON_ERROR(rtc_gpio_deinit(A4_GPIO_BUTTON_POWER), TAG,
                            "restore POWER digital input");
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_MCU_POWER_ENABLE, 1), TAG,
                        "preload MCU power high");
    ESP_RETURN_ON_ERROR(gpio_set_direction(A4_GPIO_MCU_POWER_ENABLE,
                                          GPIO_MODE_OUTPUT), TAG,
                        "MCU power output");
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_MCU_POWER_ENABLE, 1), TAG,
                        "MCU power high");
    /* Keep the latch asserted for the entire application lifetime. On a
     * software reset this hold survives long enough to keep POWER_EN alive;
     * releasing it during early init would briefly expose GPIO4's reset state
     * and switch the board off before the output can be restored. */
    ESP_RETURN_ON_ERROR(gpio_hold_en(A4_GPIO_MCU_POWER_ENABLE), TAG,
                        "hold MCU power high");


    /* 未识别前预装高电平：新板 SD 尚未挂载，旧板 ADC 通路保持启用。
     * 必须先装载输出锁存值，再切换输出方向，避免旧板出现短暂低脉冲。 */
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_SD_POWER_ENABLE, 1), TAG,
                        "preload board GPIO6 high");
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << A4_GPIO_SD_POWER_ENABLE) |
                        (1ULL << A4_GPIO_FRONTLIGHT_ENABLE) |
                        (1ULL << A4_GPIO_TOUCH_RESET) |
                        (1ULL << A4_GPIO_EPD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "board outputs");
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_EPD_CS, 1), TAG, "EPD CS idle");
    /* Release the GSL touch reset; pulled low only for sleep power-down. */
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_TOUCH_RESET, 1), TAG, "touch reset high");
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 0), TAG, "frontlight off");
    /* 先预装正常运行电平和方向，再释放运输态保持，避免 GPIO6 低脉冲。 */
    static const gpio_num_t transport_outputs[] = {
        A4_GPIO_SD_POWER_ENABLE, A4_GPIO_TOUCH_RESET,
        A4_GPIO_FRONTLIGHT_ENABLE, A4_GPIO_EPD_CS,
    };
    for (size_t i = 0; i < sizeof(transport_outputs) / sizeof(transport_outputs[0]); ++i) {
        ESP_RETURN_ON_ERROR(gpio_hold_dis(transport_outputs[i]), TAG,
                            "release transport GPIO%d", transport_outputs[i]);
    }
    gpio_deep_sleep_hold_dis();

    /* sdkconfig enables ESP_SLEEP_GPIO_RESET_WORKAROUND, whose default sleep
     * state isolates every GPIO. Keep the board latch and powered rails on,
     * preserve the current GSL reset level (application sleep leaves it high),
     * and keep the frontlight physically off while the CPU is in light sleep. */
    static const gpio_num_t light_sleep_outputs[] = {
        A4_GPIO_TOUCH_RESET,
        A4_GPIO_MCU_POWER_ENABLE,
        A4_GPIO_SD_POWER_ENABLE,
        A4_GPIO_FRONTLIGHT_ENABLE,
        A4_GPIO_EPD_CS,
    };
    for (size_t i = 0;
         i < sizeof(light_sleep_outputs) / sizeof(light_sleep_outputs[0]);
         ++i) {
        ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(light_sleep_outputs[i]), TAG,
                            "preserve GPIO%d in light sleep",
                            light_sleep_outputs[i]);
    }

    const gpio_config_t active_low_buttons = {
        .pin_bit_mask = (1ULL << A4_GPIO_BUTTON_UP) | (1ULL << A4_GPIO_BUTTON_DOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&active_low_buttons), TAG, "UP/DOWN buttons");

    const gpio_config_t power_button = {
        .pin_bit_mask = 1ULL << A4_GPIO_BUTTON_POWER,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_button), TAG, "POWER button");

    const gpio_config_t touch_int = {
        .pin_bit_mask = 1ULL << A4_GPIO_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&touch_int), TAG, "GSL INT");

    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t init_i2c_bus(void)
{
    if (s_i2c_bus != NULL) return ESP_OK;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = A4_GPIO_I2C_SDA,
        .scl_io_num = A4_GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

static esp_err_t probe_board_fuel_gauge(void)
{
    unsigned consecutive_absent = 0;
    esp_err_t last_error = ESP_ERR_INVALID_STATE;
    for (unsigned attempt = 0; attempt < A4_BOARD_PROBE_ATTEMPTS; ++attempt) {
        last_error = i2c_master_probe(s_i2c_bus, A4_FUEL_GAUGE_ADDRESS,
                                      A4_I2C_TIMEOUT_MS);
        if (last_error == ESP_OK) return ESP_OK;
        consecutive_absent = last_error == ESP_ERR_NOT_FOUND ?
                                 consecutive_absent + 1U : 0U;
        if (attempt + 1U < A4_BOARD_PROBE_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(A4_BOARD_PROBE_DELAY_MS));
        }
    }
    if (consecutive_absent < A4_BOARD_ABSENT_CONFIRMATIONS) {
        return last_error == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_STATE :
                                                last_error;
    }
    /* 不能把整条总线没响应当作旧板。RTC 或触摸 ACK 证明总线可工作；
     * 此处只探测地址，不上传触摸固件，也不写电量计模型。 */
    if (i2c_master_probe(s_i2c_bus, A4_RTC_ADDRESS, A4_I2C_TIMEOUT_MS) != ESP_OK &&
        i2c_master_probe(s_i2c_bus, 0x40, A4_I2C_TIMEOUT_MS) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t bind_board_power_profile(void)
{
    /* 保留工厂身份和授权信息，NVS 异常时绝不自动擦除分区。 */
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "board binding NVS init");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("a4_hardware", NVS_READWRITE, &handle), TAG,
                        "board binding NVS open");
    uint8_t stored = A4_BOARD_POWER_UNKNOWN;
    esp_err_t err = nvs_get_u8(handle, "power_profile", &stored);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }
    if (stored != A4_BOARD_POWER_UNKNOWN && stored != A4_BOARD_POWER_LEGACY &&
        stored != A4_BOARD_POWER_CW2015) {
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t probe = probe_board_fuel_gauge();
    a4_board_power_profile_t profile = (a4_board_power_profile_t)stored;
    if (probe == ESP_OK) {
        profile = A4_BOARD_POWER_CW2015;
    } else if (profile == A4_BOARD_POWER_CW2015) {
        /* 新板绑定不可因电量计掉线、超时或读取失败而降级成旧板。 */
        ESP_LOGW(TAG, "CW2015 probe=%s; retaining bound new-board profile",
                 esp_err_to_name(probe));
    } else if (probe == ESP_ERR_NOT_FOUND) {
        profile = A4_BOARD_POWER_LEGACY;
    } else if (profile == A4_BOARD_POWER_UNKNOWN) {
        ESP_LOGE(TAG, "board detection unresolved: %s; SD power not enabled",
                 esp_err_to_name(probe));
        nvs_close(handle);
        return probe;
    } else {
        ESP_LOGW(TAG, "I2C probe=%s; retaining bound legacy-board profile",
                 esp_err_to_name(probe));
    }

    if ((uint8_t)profile != stored) {
        err = nvs_set_u8(handle, "power_profile", (uint8_t)profile);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }
    nvs_close(handle);

    const bool modern = profile == A4_BOARD_POWER_CW2015;
    const int enabled_level = modern ? 0 : 1;
    ESP_RETURN_ON_ERROR(gpio_set_level(A4_GPIO_SD_POWER_ENABLE, enabled_level),
                        TAG, "apply detected board GPIO6 power");
    vTaskDelay(pdMS_TO_TICKS(100));
    s_board_power_profile = profile;
    s_sd_power_initialized = true;
    s_sd_power_enabled = true;
    ESP_LOGI(TAG, "power profile=%s GPIO%d=%d SD-switchable=%d binding=NVS",
             modern ? "cw2015-sd-active-low" : "legacy-adc-high",
             A4_GPIO_SD_POWER_ENABLE, enabled_level, modern);
    return ESP_OK;
}

static esp_err_t add_i2c_device_speed(uint16_t address, uint32_t scl_hz,
                                      i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_hz,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &config, device);
}

static esp_err_t add_i2c_device(uint16_t address, i2c_master_dev_handle_t *device)
{
    return add_i2c_device_speed(address, A4_I2C_SPEED_HZ, device);
}

static bool cw2015_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (s_fuel_gauge == NULL || data == NULL || len == 0) {
        return false;
    }
    return i2c_master_transmit_receive(s_fuel_gauge, &reg, 1, data, len,
                                       A4_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool cw2015_write(uint8_t reg, uint8_t value)
{
    if (s_fuel_gauge == NULL) {
        return false;
    }
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_fuel_gauge, data, sizeof(data),
                               A4_I2C_TIMEOUT_MS) == ESP_OK;
}

static void init_cw2015(void)
{
    if (s_board_power_profile != A4_BOARD_POWER_CW2015) {
        s_fuel_gauge = NULL;
        s_fuel_gauge_available = false;
        ESP_LOGI(TAG, "bound legacy board: CW2015 absent; calibrated ADC selected");
        return;
    }

    /* available 表示已选择的硬件来源；即使创建句柄或读数失败也不转 ADC。 */
    s_fuel_gauge_available = true;
    if (add_i2c_device(A4_FUEL_GAUGE_ADDRESS, &s_fuel_gauge) != ESP_OK) {
        s_fuel_gauge = NULL;
        ESP_LOGW(TAG, "bound new board: CW2015 handle unavailable");
        return;
    }
    uint8_t mode = 0;
    if (cw2015_read(A4_CW2015_REG_MODE, &mode, 1) &&
        (mode & A4_CW2015_MODE_SLEEP_MASK) == A4_CW2015_MODE_SLEEP_MASK) {
        if (cw2015_write(A4_CW2015_REG_MODE, A4_CW2015_MODE_NORMAL)) {
            ESP_LOGI(TAG, "CW2015 fuel gauge woke from sleep mode");
        } else {
            ESP_LOGW(TAG, "CW2015 fuel gauge wake failed (MODE=0x%02x)",
                     mode);
        }
    }
    ESP_LOGI(TAG, "CW2015 fuel gauge enabled at 0x%02x",
             A4_FUEL_GAUGE_ADDRESS);
}

static bool cw2015_read_batinfo(uint8_t *profile)
{
    if (profile == NULL) {
        return false;
    }
    /* Read the model window in bounded chunks for startup diagnostics. */
    for (size_t offset = 0; offset < A4_CW2015_BATINFO_SIZE; offset += 8) {
        if (!cw2015_read((uint8_t)(A4_CW2015_REG_BATINFO + offset),
                         profile + offset, 8)) {
            return false;
        }
    }
    return true;
}

static bool cw2015_write_batinfo_profile(void)
{
    uint8_t config = 0;
    uint8_t mode = 0;
    if (!cw2015_read(A4_CW2015_REG_CONFIG, &config, 1) ||
        !cw2015_read(A4_CW2015_REG_MODE, &mode, 1)) {
        return false;
    }
    for (size_t index = 0; index < A4_CW2015_BATINFO_SIZE; ++index) {
        if (!cw2015_write((uint8_t)(A4_CW2015_REG_BATINFO + index),
                          s_cw2015_batinfo_profile[index])) {
            ESP_LOGW(TAG, "CW2015 BATINFO write failed at offset=%u",
                     (unsigned)index);
            return false;
        }
    }
    config |= A4_CW2015_CONFIG_UPDATE_FLAG;
    if (!cw2015_write(A4_CW2015_REG_CONFIG, config)) {
        return false;
    }
    const uint8_t normal_mode =
        (uint8_t)(mode & (uint8_t)~A4_CW2015_MODE_RESTART_MASK);
    if (!cw2015_write(A4_CW2015_REG_MODE,
                      (uint8_t)(normal_mode | A4_CW2015_MODE_RESTART_MASK))) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!cw2015_write(A4_CW2015_REG_MODE, normal_mode)) {
        return false;
    }
    uint8_t verify[A4_CW2015_BATINFO_SIZE] = {0};
    uint8_t verify_config = 0;
    if (!cw2015_read_batinfo(verify) ||
        !cw2015_read(A4_CW2015_REG_CONFIG, &verify_config, 1)) {
        return false;
    }
    for (size_t index = 0; index < A4_CW2015_BATINFO_SIZE; ++index) {
        if (verify[index] != s_cw2015_batinfo_profile[index]) {
            ESP_LOGW(TAG, "CW2015 BATINFO verify mismatch at offset=%u",
                     (unsigned)index);
            return false;
        }
    }
    if ((verify_config & A4_CW2015_CONFIG_UPDATE_FLAG) == 0) {
        return false;
    }
    ESP_LOGI(TAG, "CW2015 BATINFO stored and verified: bytes=%u CONFIG=0x%02x",
             A4_CW2015_BATINFO_SIZE, (unsigned)verify_config);
    return true;
}


static void init_i2c_devices(void)
{
    if (i2c_master_probe(s_i2c_bus, A4_RTC_ADDRESS, A4_I2C_TIMEOUT_MS) == ESP_OK &&
        add_i2c_device_speed(A4_RTC_ADDRESS, A4_RTC_I2C_SPEED_HZ, &s_rtc) ==
            ESP_OK) {
        ESP_LOGI(TAG, "PCF8563 RTC found at 0x%02x", A4_RTC_ADDRESS);
    } else {
        ESP_LOGW(TAG, "PCF8563 RTC not detected");
    }

    if (i2c_master_probe(s_i2c_bus, 0x40, A4_I2C_TIMEOUT_MS) == ESP_OK &&
        add_i2c_device(0x40, &s_touch) == ESP_OK) {
        ESP_LOGI(TAG, "GSL touch controller found at 0x40");
        if (!gsl_init_firmware()) {
            i2c_master_bus_rm_device(s_touch);
            s_touch = NULL;
        }
    } else {
        ESP_LOGW(TAG, "GSL touch controller not detected");
    }

    init_cw2015();

    /* LM3630A may only acknowledge while its enable input is asserted, and
     * needs time after HWEN rises before it responds; a single probe 5 ms
     * after enable missed the chip on a backlight-fitted unit. */
    gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    s_frontlight_available = false;
    for (int attempt = 0; attempt < 5 && !s_frontlight_available; attempt++) {
        s_frontlight_available =
            i2c_master_probe(s_i2c_bus, A4_FRONTLIGHT_ADDRESS,
                             A4_I2C_TIMEOUT_MS) == ESP_OK;
        if (!s_frontlight_available) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 0);
    ESP_LOGI(TAG, "frontlight controller: %s", s_frontlight_available ? "present" : "absent");

    const gpio_config_t charging_input = {
        .pin_bit_mask = 1ULL << A4_GPIO_CHARGING,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    if (gpio_config(&charging_input) != ESP_OK) {
        ESP_LOGW(TAG, "charging input setup failed");
    }
}

static bool cw2015_read_measurement(uint16_t *voltage_mv, uint8_t *percent)
{
    if (voltage_mv == NULL || percent == NULL) {
        return false;
    }

    /* CW2015 SOC is the primary displayed battery value. VCELL remains
     * available for diagnostics and ADC fallback validation. */
    uint8_t regs[7] = {0};
    if (!cw2015_read(A4_CW2015_REG_VCELL, regs, sizeof(regs))) {
        return false;
    }

    const uint16_t vcell_raw =
        (uint16_t)(((uint16_t)regs[0] << 8) | regs[1]) & 0x3fffU;
    const uint16_t soc_raw = (uint16_t)(((uint16_t)regs[2] << 8) | regs[3]);
    const uint32_t voltage = ((uint32_t)vcell_raw * 305U + 500U) / 1000U;

    uint8_t mode = 0;
    if (!cw2015_read(A4_CW2015_REG_MODE, &mode, 1) ||
        (mode & A4_CW2015_MODE_SLEEP_MASK) == A4_CW2015_MODE_SLEEP_MASK ||
        voltage < 2500U || voltage > 4500U || soc_raw > 100U * 256U) {
        return false;
    }
    const uint32_t soc_percent = ((uint32_t)soc_raw + 128U) / 256U;
    const uint32_t display_percent = soc_percent + 1U;
    *voltage_mv = (uint16_t)voltage;
    *percent = (uint8_t)(display_percent > 100U ? 100U : display_percent);
    ESP_LOGI(TAG, "CW2015 battery voltage=%u mV soc=%u%% display=%u%%",
             (unsigned)voltage, (unsigned)soc_percent,
             (unsigned)*percent);
    return true;
}

/* Log the raw startup sample as a bring-up aid while keeping the production
 * battery path in a4_board_read_battery_percent(). */
static void fuel_gauge_diagnostic(void)
{
    a4_board_i2c_scan();
    if (!s_fuel_gauge_available) {
        ESP_LOGI(TAG, "CW2015 unavailable; legacy battery ADC selected after discovery");
        return;
    }

    uint8_t regs[12] = {0};
    const bool read_ok = cw2015_read(A4_CW2015_REG_VERSION, regs,
                                     sizeof(regs));
    const unsigned vcell_raw = ((unsigned)(regs[2] & 0x3f) << 8) | regs[3];
    const unsigned soc_raw = ((unsigned)regs[4] << 8) | regs[5];
    ESP_LOGI(TAG,
             "CW2015 at 0x%02x read=%d regs=%02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x",
             A4_FUEL_GAUGE_ADDRESS, read_ok, regs[0], regs[1], regs[2], regs[3],
             regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10],
             regs[11]);
    ESP_LOGI(TAG,
             "CW2015 VERSION=0x%02x VCELL=%u mV SOC=%u.%02u%% MODE=0x%02x",
             regs[0], vcell_raw * 305 / 1000, soc_raw / 256U,
             ((soc_raw & 0xffU) * 100U) / 256U, regs[10]);

    uint8_t batinfo[A4_CW2015_BATINFO_SIZE] = {0};
    if (!cw2015_read_batinfo(batinfo)) {
        ESP_LOGW(TAG, "CW2015 BATINFO read failed");
        return;
    }
    for (size_t row = 0; row < sizeof(batinfo) / 16; ++row) {
        ESP_LOGI(TAG,
                 "CW2015 BATINFO %02x-%02x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 (unsigned)(A4_CW2015_REG_BATINFO + row * 16),
                 (unsigned)(A4_CW2015_REG_BATINFO + row * 16 + 15),
                 batinfo[row * 16], batinfo[row * 16 + 1],
                 batinfo[row * 16 + 2], batinfo[row * 16 + 3],
                 batinfo[row * 16 + 4], batinfo[row * 16 + 5],
                 batinfo[row * 16 + 6], batinfo[row * 16 + 7],
                 batinfo[row * 16 + 8], batinfo[row * 16 + 9],
                 batinfo[row * 16 + 10], batinfo[row * 16 + 11],
                 batinfo[row * 16 + 12], batinfo[row * 16 + 13],
                 batinfo[row * 16 + 14], batinfo[row * 16 + 15]);
    }
    bool batinfo_empty = true;
    bool batinfo_matches_target = true;
    for (size_t index = 0; index < sizeof(batinfo); ++index) {
        if (batinfo[index] != 0) {
            batinfo_empty = false;
        }
        if (batinfo[index] != s_cw2015_batinfo_profile[index]) {
            batinfo_matches_target = false;
        }
    }
    uint8_t config = 0;
    (void)cw2015_read(A4_CW2015_REG_CONFIG, &config, 1);
    if (batinfo_empty || !batinfo_matches_target) {
        ESP_LOGW(TAG, "CW2015 BATINFO %s; uploading 3.7V 2000mAh target profile",
                 batinfo_empty ? "empty" : "mismatch");
        (void)cw2015_write_batinfo_profile();
    } else if ((config & A4_CW2015_CONFIG_UPDATE_FLAG) == 0) {
        ESP_LOGW(TAG, "CW2015 BATINFO target profile present but CONFIG update flag is clear; preserving resident profile");
    }
}

static bool init_battery_adc(bool diagnostic)
{
    if (s_battery_adc != NULL && s_battery_adc_cali != NULL) return true;
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_battery_adc_retry_after_us) return false;
    s_battery_adc_retry_after_us = now_us + 5000000;
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_battery_adc);
    if (err != ESP_OK) {
        if (diagnostic) ESP_LOGW(TAG, "legacy battery ADC unit: %s", esp_err_to_name(err));
        return false;
    }
    const adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_battery_adc, ADC_CHANNEL_9, &channel);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_9,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (err == ESP_OK) err = adc_cali_create_scheme_curve_fitting(
        &calibration, &s_battery_adc_cali);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(s_battery_adc);
        s_battery_adc = NULL;
        s_battery_adc_cali = NULL;
        if (diagnostic) ESP_LOGW(TAG, "legacy battery ADC unavailable: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void preserve_power_on_restart(void)
{
    ESP_ERROR_CHECK(gpio_set_level(A4_GPIO_MCU_POWER_ENABLE, 1));
    ESP_ERROR_CHECK(gpio_hold_en(A4_GPIO_MCU_POWER_ENABLE));
}

esp_err_t a4_board_init(void)
{
    if (s_sd_power_initialized) return ESP_OK;
    ESP_RETURN_ON_ERROR(init_power_and_buttons(), TAG, "power and buttons");
    if (!s_power_restart_handler_registered) {
        ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(preserve_power_on_restart),
                            TAG, "power restart handler");
        s_power_restart_handler_registered = true;
    }
    /* USB MSC、文件/字体访问之前绑定 GPIO6 极性；只提前轻量地址探测，
     * RTC、触摸固件和前光仍走后续设备初始化，ADC 仍然惰性初始化。 */
    ESP_RETURN_ON_ERROR(init_i2c_bus(), TAG, "board detection I2C bus");
    ESP_RETURN_ON_ERROR(bind_board_power_profile(), TAG, "board power binding");
    return ESP_OK;
}

esp_err_t a4_board_init_i2c_devices(void)
{
    if (s_i2c_devices_initialized) return ESP_OK;
    if (!s_sd_power_initialized || s_i2c_bus == NULL) return ESP_ERR_INVALID_STATE;
    init_i2c_devices();
    fuel_gauge_diagnostic();
    s_battery_discovery_complete = true;
    s_i2c_devices_initialized = true;
    return ESP_OK;
}

esp_err_t a4_board_set_sd_power(bool enabled)
{
    if (!s_sd_power_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled && !a4_board_sd_power_can_suspend()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_sd_power_enabled == enabled) {
        return ESP_OK;
    }

    const int enabled_level = s_board_power_profile == A4_BOARD_POWER_CW2015 ? 0 : 1;
    /* 板型已在访问 SD 前绑定，运行中不得因 SOC 或 I2C 读失败切换极性。 */
    const esp_err_t err = gpio_set_level(A4_GPIO_SD_POWER_ENABLE,
                                         enabled ? enabled_level : !enabled_level);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD power %s failed: %s",
                 enabled ? "enable" : "disable", esp_err_to_name(err));
        return err;
    }
    s_sd_power_enabled = enabled;
    ESP_LOGI(TAG, "SD power %s: GPIO%d=%d",
             enabled ? "enabled" : "disabled", A4_GPIO_SD_POWER_ENABLE,
             enabled ? enabled_level : !enabled_level);
    return ESP_OK;
}

bool a4_board_sd_power_enabled(void)
{
    return s_sd_power_enabled;
}

bool a4_board_sd_power_can_suspend(void)
{
    return s_sd_power_initialized && s_board_power_profile == A4_BOARD_POWER_CW2015;
}

esp_err_t a4_board_register_reader_key_isr(a4_board_reader_key_isr_t callback,
                                           void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_reader_key_isr_callback = callback;
    s_reader_key_isr_context = context;
    if (s_reader_key_isr_registered) {
        return ESP_OK;
    }

    const esp_err_t service = ensure_gpio_isr_service();
    if (service != ESP_OK) {
        s_reader_key_isr_callback = NULL;
        s_reader_key_isr_context = NULL;
        return service;
    }

    bool up_added = false;
    bool down_added = false;
    bool power_added = false;
    esp_err_t err = gpio_isr_handler_add(
        A4_GPIO_BUTTON_UP, reader_key_gpio_isr,
        (void *)(uintptr_t)A4_BUTTON_UP);
    if (err != ESP_OK) {
        goto cleanup;
    }
    up_added = true;
    err = gpio_isr_handler_add(A4_GPIO_BUTTON_DOWN, reader_key_gpio_isr,
                               (void *)(uintptr_t)A4_BUTTON_DOWN);
    if (err != ESP_OK) {
        goto cleanup;
    }
    down_added = true;
    err = gpio_isr_handler_add(A4_GPIO_BUTTON_POWER, reader_key_gpio_isr,
                               (void *)(uintptr_t)A4_BUTTON_POWER);
    if (err != ESP_OK) {
        goto cleanup;
    }
    power_added = true;
    err = gpio_set_intr_type(A4_GPIO_BUTTON_UP, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_set_intr_type(A4_GPIO_BUTTON_DOWN, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_set_intr_type(A4_GPIO_BUTTON_POWER, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_intr_enable(A4_GPIO_BUTTON_UP);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_intr_enable(A4_GPIO_BUTTON_DOWN);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_intr_enable(A4_GPIO_BUTTON_POWER);
    if (err != ESP_OK) {
        goto cleanup;
    }
    s_reader_key_isr_registered = true;
    ESP_LOGI(TAG, "reader-key GPIO ISR ready: UP=GPIO%d DOWN=GPIO%d POWER=GPIO%d any-edge",
             A4_GPIO_BUTTON_UP, A4_GPIO_BUTTON_DOWN, A4_GPIO_BUTTON_POWER);
    return ESP_OK;

cleanup:
    if (power_added) {
        (void)gpio_intr_disable(A4_GPIO_BUTTON_POWER);
        (void)gpio_isr_handler_remove(A4_GPIO_BUTTON_POWER);
    }
    if (down_added) {
        (void)gpio_intr_disable(A4_GPIO_BUTTON_DOWN);
        (void)gpio_isr_handler_remove(A4_GPIO_BUTTON_DOWN);
    }
    if (up_added) {
        (void)gpio_intr_disable(A4_GPIO_BUTTON_UP);
        (void)gpio_isr_handler_remove(A4_GPIO_BUTTON_UP);
    }
    s_reader_key_isr_callback = NULL;
    s_reader_key_isr_context = NULL;
    s_reader_key_isr_registered = false;
    ESP_LOGE(TAG, "reader-key GPIO ISR registration failed: %s",
             esp_err_to_name(err));
    return err;
}

esp_err_t a4_board_register_touch_int_isr(a4_board_touch_int_isr_t callback,
                                          void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_touch_int_isr_callback = callback;
    s_touch_int_isr_context = context;
    if (s_touch_int_isr_registered) {
        return ESP_OK;
    }

    const esp_err_t service = ensure_gpio_isr_service();
    if (service != ESP_OK) {
        s_touch_int_isr_callback = NULL;
        s_touch_int_isr_context = NULL;
        return service;
    }
    bool handler_added = false;
    esp_err_t err = gpio_isr_handler_add(A4_GPIO_TOUCH_INT,
                                         touch_int_gpio_isr, NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    handler_added = true;
    err = gpio_set_intr_type(A4_GPIO_TOUCH_INT, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_intr_enable(A4_GPIO_TOUCH_INT);
    if (err != ESP_OK) {
        goto cleanup;
    }
    s_touch_int_isr_registered = true;
    ESP_LOGI(TAG, "GSL INT GPIO%d any-edge ISR ready", A4_GPIO_TOUCH_INT);
    return ESP_OK;

cleanup:
    if (handler_added) {
        (void)gpio_intr_disable(A4_GPIO_TOUCH_INT);
        (void)gpio_isr_handler_remove(A4_GPIO_TOUCH_INT);
    }
    s_touch_int_isr_callback = NULL;
    s_touch_int_isr_context = NULL;
    s_touch_int_isr_registered = false;
    ESP_LOGE(TAG, "GSL INT ISR registration failed: %s",
             esp_err_to_name(err));
    return err;
}

esp_err_t a4_board_register_charging_isr(a4_board_charging_isr_t callback,
                                         void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_charging_isr_callback = callback;
    s_charging_isr_context = context;
    if (s_charging_isr_registered) {
        return ESP_OK;
    }

    const esp_err_t service = ensure_gpio_isr_service();
    if (service != ESP_OK) {
        s_charging_isr_callback = NULL;
        s_charging_isr_context = NULL;
        return service;
    }
    esp_err_t err = gpio_isr_handler_add(A4_GPIO_CHARGING,
                                         charging_gpio_isr, NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = gpio_set_intr_type(A4_GPIO_CHARGING, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        goto cleanup_handler;
    }
    err = gpio_intr_enable(A4_GPIO_CHARGING);
    if (err != ESP_OK) {
        goto cleanup_handler;
    }
    s_charging_isr_registered = true;
    ESP_LOGI(TAG, "charging GPIO%d any-edge ISR ready", A4_GPIO_CHARGING);
    return ESP_OK;

cleanup_handler:
    (void)gpio_intr_disable(A4_GPIO_CHARGING);
    (void)gpio_isr_handler_remove(A4_GPIO_CHARGING);
cleanup:
    s_charging_isr_callback = NULL;
    s_charging_isr_context = NULL;
    return err;
}

bool a4_board_touch_interrupt_active(void)
{
    return gpio_get_level(A4_GPIO_TOUCH_INT) != 0;
}

bool a4_board_button_pressed(a4_button_t button)
{
    switch (button) {
    case A4_BUTTON_UP:
        return gpio_get_level(A4_GPIO_BUTTON_UP) == 0;
    case A4_BUTTON_DOWN:
        return gpio_get_level(A4_GPIO_BUTTON_DOWN) == 0;
    case A4_BUTTON_POWER:
        return gpio_get_level(A4_GPIO_BUTTON_POWER) != 0;
    default:
        return false;
    }
}

bool a4_board_read_datetime(a4_datetime_t *datetime)
{
    static bool rtc_fault_reported;

    if (datetime == NULL || s_rtc == NULL) {
        return false;
    }

    const uint8_t first_register = 0x02;
    uint8_t registers[7] = {0};
    esp_err_t err = i2c_master_transmit(s_rtc, &first_register, 1, A4_RTC_IO_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = i2c_master_receive(s_rtc, registers, sizeof(registers), A4_RTC_IO_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        if (!rtc_fault_reported) {
            ESP_LOGW(TAG, "PCF8563 read failed: %s",
                     esp_err_to_name(err));
            rtc_fault_reported = true;
        }
        return false;
    }

    const a4_datetime_t result = {
        .year = (uint16_t)(2000 + bcd_to_binary(registers[6])),
        .month = bcd_to_binary(registers[5] & 0x1f),
        .day = bcd_to_binary(registers[3] & 0x3f),
        .weekday = registers[4] & 0x07,
        .hour = bcd_to_binary(registers[2] & 0x3f),
        .minute = bcd_to_binary(registers[1] & 0x7f),
        .second = bcd_to_binary(registers[0] & 0x7f),
    };
    if (!datetime_is_valid(&result) || result.weekday > 6 || (registers[0] & 0x80U) != 0) {
        if (!rtc_fault_reported) {
            ESP_LOGW(TAG,
                     "PCF8563 invalid time raw=%02x %02x %02x %02x %02x %02x %02x",
                     registers[0], registers[1], registers[2], registers[3],
                     registers[4], registers[5], registers[6]);
            rtc_fault_reported = true;
        }
        return false;
    }
    rtc_fault_reported = false;
    *datetime = result;
    return true;
}

bool a4_board_write_datetime(const a4_datetime_t *datetime)
{
    if (s_rtc == NULL || !datetime_is_valid(datetime)) {
        return false;
    }

    const uint8_t control_register = 0x00;
    uint8_t control = 0;
    esp_err_t err = i2c_master_transmit(s_rtc, &control_register, 1, A4_RTC_IO_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = i2c_master_receive(s_rtc, &control, 1, A4_RTC_IO_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8563 control read failed: %s", esp_err_to_name(err));
        return false;
    }

    /* PCF8563 STOP freezes the divider while the complete time tuple is replaced. */
    const uint8_t stop[] = {0x00, (uint8_t)(control | 0x20U)};
    err = i2c_master_transmit(s_rtc, stop, sizeof(stop), A4_RTC_IO_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8563 STOP request failed: %s", esp_err_to_name(err));
        return false;
    }

    const uint8_t registers[] = {0x02, binary_to_bcd(datetime->second), binary_to_bcd(datetime->minute),
                                 binary_to_bcd(datetime->hour), binary_to_bcd(datetime->day),
                                 datetime_weekday(datetime), binary_to_bcd(datetime->month),
                                 binary_to_bcd((uint8_t)(datetime->year - 2000U))};
    bool wrote = false;
    for (unsigned attempt = 0; attempt < A4_RTC_WRITE_ATTEMPTS; ++attempt) {
        err = i2c_master_transmit(s_rtc, registers, sizeof(registers), A4_RTC_IO_TIMEOUT_MS);
        if (err == ESP_OK) { wrote = true; break; }
        vTaskDelay(pdMS_TO_TICKS(A4_RTC_WRITE_RETRY_DELAY_MS));
    }

    const uint8_t start[] = {0x00, (uint8_t)(control & ~0x20U)};
    bool restarted = false;
    for (unsigned attempt = 0; attempt < A4_RTC_RESTART_RETRIES; ++attempt) {
        uint8_t readback = 0xff;
        err = i2c_master_transmit(s_rtc, start, sizeof(start), A4_RTC_IO_TIMEOUT_MS);
        if (err == ESP_OK) err = i2c_master_transmit(s_rtc, &control_register, 1, A4_RTC_IO_TIMEOUT_MS);
        if (err == ESP_OK) err = i2c_master_receive(s_rtc, &readback, 1, A4_RTC_IO_TIMEOUT_MS);
        if (err == ESP_OK && (readback & 0x20U) == 0) { restarted = true; break; }
        vTaskDelay(pdMS_TO_TICKS(A4_RTC_RETRY_DELAY_MS));
    }
    if (!wrote || !restarted) {
        ESP_LOGE(TAG, "PCF8563 datetime write failed wrote=%s restarted=%s err=%s",
                 wrote ? "yes" : "no", restarted ? "yes" : "no", esp_err_to_name(err));
        return false;
    }

    for (unsigned attempt = 0; attempt < A4_RTC_RESTART_RETRIES; ++attempt) {
        a4_datetime_t readback;
        if (a4_board_read_datetime(&readback) && datetime_minute_matches(&readback, datetime)) {
            ESP_LOGI(TAG, "PCF8563 datetime write verified: %04u-%02u-%02u %02u:%02u:%02u",
                     (unsigned)readback.year, (unsigned)readback.month, (unsigned)readback.day,
                     (unsigned)readback.hour, (unsigned)readback.minute, (unsigned)readback.second);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(A4_RTC_RETRY_DELAY_MS));
    }
    ESP_LOGE(TAG, "PCF8563 datetime readback mismatch");
    return false;
}


bool a4_board_clear_datetime(void)
{
    if (s_rtc == NULL) {
        return false;
    }

    /* STOP the divider and clear status/time registers. This deliberately
     * leaves VL set in seconds so startup will not treat the RTC as valid. */
    const uint8_t clear_registers[] = {
        0x00, 0x20,
        0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const esp_err_t err = i2c_master_transmit(s_rtc, clear_registers,
                                               sizeof(clear_registers),
                                               A4_RTC_IO_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF8563 clear failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "PCF8563 datetime cleared; oscillator stopped");
    return true;
}

void a4_board_sleep_touch(void)
{
    if (s_touch_int_isr_registered) {
        (void)gpio_intr_disable(A4_GPIO_TOUCH_INT);
    }
    /* Application sleep keeps TOUCH_RESET high so the uploaded GSL1680
     * firmware stays resident and wake can skip the ~1.1s re-upload.
     * No in-tree official "green mode" I2C command is available yet; host
     * only gates the INT line. Transport deep sleep still drives RST low
     * via gpio_hold inside a4_board_transport_deep_sleep(). */
    if (gpio_get_level(A4_GPIO_TOUCH_RESET) == 0) {
        gpio_set_level(A4_GPIO_TOUCH_RESET, 1);
    }
}

bool a4_board_wake_touch(void)
{
    if (s_touch == NULL) {
        return false;
    }
    /* A board wired to the GSL SHUTDOWN input may retain its firmware while
     * sleeping. Try the short resume path first; reset-only boards will fail
     * the probe and transparently fall back to the complete upload below. */
    gpio_set_level(A4_GPIO_TOUCH_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gsl_verify_running()) {
        if (s_touch_int_isr_registered) {
            (void)gpio_intr_enable(A4_GPIO_TOUCH_INT);
        }
        ESP_LOGI(TAG, "GSL touch resumed without firmware upload");
        return true;
    }
    const bool loaded = gsl_init_firmware();
    if (loaded && s_touch_int_isr_registered) {
        (void)gpio_intr_enable(A4_GPIO_TOUCH_INT);
    }
    return loaded;
}

esp_err_t a4_board_light_sleep(uint64_t timer_wakeup_us,
                               a4_wakeup_reason_t *reason)
{
    const gpio_num_t wake_gpios[] = {
        A4_GPIO_BUTTON_POWER, A4_GPIO_CHARGING,
    };
    const gpio_int_type_t runtime_interrupts[] = {
        s_reader_key_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
        s_charging_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
    };
    if (reason != NULL) {
        *reason = A4_WAKEUP_OTHER;
    }

    /* The modem domain is not needed while the lockscreen is static.  Force it
     * off for this sleep interval even if a radio driver left its automatic
     * power-management request active.  Restore AUTO immediately after wake so
     * normal Wi-Fi/BLE operation is unaffected. */
    const esp_err_t modem_pd_err =
        esp_sleep_pd_config(ESP_PD_DOMAIN_MODEM, ESP_PD_OPTION_OFF);
    if (modem_pd_err != ESP_OK) {
        ESP_LOGW(TAG, "MODEM power-down request failed: %s",
                 esp_err_to_name(modem_pd_err));
    }

    /* Both sources are level based. The integration layer waits for POWER
     * release before calling this function, preventing an immediate wake.
     * Charging detect is active-high, so arm the opposite level: an already
     * attached cable must not bounce the device straight back out of sleep. */
    uint32_t causes = 0;
    s_charging_sleep_level = gpio_get_level(A4_GPIO_CHARGING) != 0;
    for (size_t index = 0; index < sizeof(wake_gpios) / sizeof(wake_gpios[0]);
         ++index) {
        (void)gpio_intr_disable(wake_gpios[index]);
    }
    esp_err_t err =
        gpio_wakeup_enable(A4_GPIO_BUTTON_POWER, GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POWER wake enable: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = gpio_wakeup_enable(
        A4_GPIO_CHARGING,
        s_charging_sleep_level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CHARGING wake enable: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO wake source: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (timer_wakeup_us > 0) {
        err = esp_sleep_enable_timer_wakeup(timer_wakeup_us);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "timer wake source: %s", esp_err_to_name(err));
            goto cleanup;
        }
    } else {
        /* 静态壁纸仅由电源键/充电脚唤醒，清除可能遗留的时钟定时源。 */
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }

    ESP_LOGI(TAG, "lockscreen light sleep: wake=GPIO%d/GPIO%d timer=%lluus",
             A4_GPIO_BUTTON_POWER, A4_GPIO_CHARGING,
             (unsigned long long)timer_wakeup_us);
    err = esp_light_sleep_start();
    if (err == ESP_OK) {
        causes = esp_sleep_get_wakeup_causes();
    }

    /* Wake sources remain configured across light sleep; clear them before
     * normal runtime or the next explicitly configured sleep interval. */
cleanup:
    for (size_t index = 0; index < sizeof(wake_gpios) / sizeof(wake_gpios[0]);
         ++index) {
        (void)gpio_wakeup_disable(wake_gpios[index]);
        esp_err_t restore_err = gpio_set_intr_type(
            wake_gpios[index], runtime_interrupts[index]);
        if (restore_err == ESP_OK &&
            runtime_interrupts[index] != GPIO_INTR_DISABLE) {
            restore_err = gpio_intr_enable(wake_gpios[index]);
        }
        if (err == ESP_OK) {
            err = restore_err;
        }
    }
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_MODEM, ESP_PD_OPTION_AUTO);

    if (reason != NULL) {
        if ((causes & (1UL << ESP_SLEEP_WAKEUP_GPIO)) != 0) {
            const bool charging_changed =
                (gpio_get_level(A4_GPIO_CHARGING) != 0) !=
                (s_charging_sleep_level != 0);
            *reason = charging_changed ? A4_WAKEUP_CHARGING
                                       : A4_WAKEUP_POWER_BUTTON;
        } else if ((causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0) {
            *reason = A4_WAKEUP_TIMER;
        }
    }
    return err;
}

esp_err_t a4_board_reader_light_sleep(uint64_t timer_wakeup_us,
                                      a4_wakeup_reason_t *reason)
{
    const gpio_num_t wake_gpios[] = {
        A4_GPIO_BUTTON_UP, A4_GPIO_BUTTON_DOWN, A4_GPIO_BUTTON_POWER,
        A4_GPIO_TOUCH_INT, A4_GPIO_CHARGING,
    };
    const gpio_int_type_t runtime_interrupts[] = {
        s_reader_key_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
        s_reader_key_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
        s_reader_key_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
        s_touch_int_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
        s_charging_isr_registered ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
    };
    if (reason != NULL) {
        *reason = A4_WAKEUP_OTHER;
    }

    /* Level wake sources must be inactive before entering sleep. Otherwise a
     * held key or an already pending GSL report would immediately wake the
     * CPU and turn the idle path into a busy loop. */
    s_charging_sleep_level = gpio_get_level(A4_GPIO_CHARGING) != 0;
    if (gpio_get_level(A4_GPIO_BUTTON_UP) == 0 ||
        gpio_get_level(A4_GPIO_BUTTON_DOWN) == 0 ||
        gpio_get_level(A4_GPIO_BUTTON_POWER) != 0 ||
        gpio_get_level(A4_GPIO_TOUCH_INT) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t index = 0; index < sizeof(wake_gpios) / sizeof(wake_gpios[0]);
         ++index) {
        (void)gpio_intr_disable(wake_gpios[index]);
    }
    esp_err_t err = gpio_wakeup_enable(A4_GPIO_BUTTON_UP,
                                       GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) goto cleanup;
    err = gpio_wakeup_enable(A4_GPIO_BUTTON_DOWN, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) goto cleanup;
    err = gpio_wakeup_enable(A4_GPIO_BUTTON_POWER, GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) goto cleanup;
    err = gpio_wakeup_enable(A4_GPIO_TOUCH_INT, GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) goto cleanup;
    err = gpio_wakeup_enable(
        A4_GPIO_CHARGING,
        s_charging_sleep_level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) goto cleanup;
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) goto cleanup;
    if (timer_wakeup_us > 0) {
        err = esp_sleep_enable_timer_wakeup(timer_wakeup_us);
        if (err != ESP_OK) goto cleanup;
    } else {
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }

    ESP_LOGI(TAG, "reader idle light sleep: wake=GPIO%d/GPIO%d/GPIO%d/GPIO%d/GPIO%d"
                  " timer=%lluus",
             A4_GPIO_BUTTON_UP, A4_GPIO_BUTTON_DOWN, A4_GPIO_BUTTON_POWER,
             A4_GPIO_TOUCH_INT, A4_GPIO_CHARGING,
             (unsigned long long)timer_wakeup_us);
    err = esp_light_sleep_start();
    uint32_t causes = err == ESP_OK ? esp_sleep_get_wakeup_causes() : 0;
    if (reason != NULL && err == ESP_OK) {
        if ((causes & (1UL << ESP_SLEEP_WAKEUP_GPIO)) != 0) {
            const bool charging_changed =
                (gpio_get_level(A4_GPIO_CHARGING) != 0) !=
                (s_charging_sleep_level != 0);
            if (charging_changed) {
                *reason = A4_WAKEUP_CHARGING;
            } else if (gpio_get_level(A4_GPIO_TOUCH_INT) != 0) {
                *reason = A4_WAKEUP_TOUCH;
            } else if (gpio_get_level(A4_GPIO_BUTTON_POWER) != 0) {
                *reason = A4_WAKEUP_POWER_BUTTON;
            } else {
                *reason = A4_WAKEUP_READER_KEY;
            }
        } else if ((causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0) {
            *reason = A4_WAKEUP_TIMER;
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "reader idle light sleep resumed: reason=%d",
                 reason == NULL ? A4_WAKEUP_OTHER : *reason);
    }

cleanup:
    for (size_t index = 0; index < sizeof(wake_gpios) / sizeof(wake_gpios[0]);
         ++index) {
        (void)gpio_wakeup_disable(wake_gpios[index]);
        esp_err_t restore_err = gpio_set_intr_type(
            wake_gpios[index], runtime_interrupts[index]);
        if (restore_err == ESP_OK &&
            runtime_interrupts[index] != GPIO_INTR_DISABLE) {
            restore_err = gpio_intr_enable(wake_gpios[index]);
        }
        if (err == ESP_OK) {
            err = restore_err;
        }
    }
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    return err;
}

esp_err_t a4_board_transport_deep_sleep(void)
{
    if (!s_sd_power_initialized ||
        (a4_board_sd_power_can_suspend() && s_sd_power_enabled)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err != ESP_OK) return err;
    err = esp_sleep_enable_ext0_wakeup(A4_GPIO_BUTTON_POWER, 1);
    if (err != ESP_OK) return err;
    /* Charging detect is active-high on GPIO11 (RTC-capable on ESP32-S3).
     * Only arm EXT1 when the pin is currently low so an already-attached
     * charger does not bounce the device out of transport sleep. Hardware
     * power-off (GPIO4 low) still relies on VBUS→POWER_EN for plug-in boot;
     * that path is outside deep sleep. */
    const bool arm_charge_wake = gpio_get_level(A4_GPIO_CHARGING) == 0;
    bool charge_wake_armed = false;
    if (arm_charge_wake) {
        err = rtc_gpio_init(A4_GPIO_CHARGING);
        if (err == ESP_OK) err = rtc_gpio_pullup_dis(A4_GPIO_CHARGING);
        if (err == ESP_OK) err = rtc_gpio_pulldown_en(A4_GPIO_CHARGING);
        if (err == ESP_OK) {
            err = esp_sleep_enable_ext1_wakeup(1ULL << A4_GPIO_CHARGING,
                                               ESP_EXT1_WAKEUP_ANY_HIGH);
        }
        if (err == ESP_OK) {
            charge_wake_armed = true;
        } else {
            ESP_LOGW(TAG, "transport charge EXT1 unavailable: %s",
                     esp_err_to_name(err));
            (void)rtc_gpio_deinit(A4_GPIO_CHARGING);
            err = ESP_OK;
        }
    }
    static const gpio_num_t pins[] = {
        A4_GPIO_SD_POWER_ENABLE, A4_GPIO_TOUCH_RESET,
        A4_GPIO_FRONTLIGHT_ENABLE, A4_GPIO_EPD_CS,
    };
    static const uint8_t levels[] = {1, 0, 0, 1};
    size_t held = 0;
    for (; held < sizeof(pins) / sizeof(pins[0]); ++held) {
        err = gpio_set_level(pins[held], levels[held]);
        if (err != ESP_OK) goto cleanup;
        err = gpio_hold_en(pins[held]);
        if (err != ESP_OK) goto cleanup;
    }
    err = rtc_gpio_init(A4_GPIO_BUTTON_POWER);
    if (err == ESP_OK) err = rtc_gpio_pullup_dis(A4_GPIO_BUTTON_POWER);
    if (err == ESP_OK) err = rtc_gpio_pulldown_en(A4_GPIO_BUTTON_POWER);
    if (err != ESP_OK) goto cleanup;
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG,
             "transport deep sleep: SD GPIO6=1 held, POWER EXT0%s",
             charge_wake_armed ? ", charge EXT1" : "");
    err = esp_deep_sleep_try_to_start();

cleanup:
    gpio_deep_sleep_hold_dis();
    while (held != 0) (void)gpio_hold_dis(pins[--held]);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    (void)rtc_gpio_deinit(A4_GPIO_BUTTON_POWER);
    if (charge_wake_armed) (void)rtc_gpio_deinit(A4_GPIO_CHARGING);
    return err;
}

void a4_board_power_off(void)
{
    ESP_LOGI(TAG, "pulling MCU power latch GPIO%d low for hardware power-off",
             A4_GPIO_MCU_POWER_ENABLE);
    ESP_ERROR_CHECK(gpio_hold_dis(A4_GPIO_MCU_POWER_ENABLE));
    ESP_ERROR_CHECK(gpio_set_level(A4_GPIO_MCU_POWER_ENABLE, 0));
    ESP_ERROR_CHECK(gpio_hold_en(A4_GPIO_MCU_POWER_ENABLE));
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();
}

static bool read_battery_percent_from_adc(uint16_t *voltage_mv, uint8_t *percent)
{
    const int64_t now_us = esp_timer_get_time();
    static int64_t next_diagnostic_us;
    const bool diagnostic = now_us >= next_diagnostic_us;
    if (diagnostic) next_diagnostic_us = now_us + 30000000;
    if (!init_battery_adc(diagnostic)) return false;
    int samples[9];
    int raw_min = 4095, raw_max = 0;
    const size_t sample_count = sizeof(samples) / sizeof(samples[0]);
    for (size_t i = 0; i < sample_count; ++i) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_battery_adc, ADC_CHANNEL_9, &raw);
        if (err != ESP_OK) {
            if (diagnostic) ESP_LOGW(TAG, "legacy battery ADC read: %s", esp_err_to_name(err));
            return false;
        }
        if (raw < raw_min) raw_min = raw;
        if (raw > raw_max) raw_max = raw;
        samples[i] = raw;
    }
    for (size_t i = 1; i < sample_count; ++i) {
        const int value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
    /* 恢复旧版九次原始采样取中五均值；不在前台逐次等待 tick。 */
    int raw_sum = 0;
    for (size_t i = 2; i < sample_count - 2; ++i) raw_sum += samples[i];
    const int raw = raw_sum / (int)(sample_count - 4);
    int adc_mv = 0;
    const esp_err_t err = adc_cali_raw_to_voltage(s_battery_adc_cali, raw, &adc_mv);
    if (err != ESP_OK) {
        if (diagnostic) ESP_LOGW(TAG, "legacy battery ADC calibration: %s", esp_err_to_name(err));
        return false;
    }
    const int battery_mv = a4_legacy_battery_mv_from_adc(adc_mv);
    if (battery_mv == 0) {
        if (diagnostic) ESP_LOGW(TAG, "legacy battery ADC GPIO10 no valid voltage raw=%d..%d trimmed=%d adc=%d mV", raw_min, raw_max, raw, adc_mv);
        return false;
    }
    if (diagnostic) ESP_LOGI(TAG, "legacy battery ADC GPIO10 raw=%d..%d trimmed=%d adc=%d mV battery=%d mV",
                             raw_min, raw_max, raw, adc_mv, battery_mv);
    *voltage_mv = (uint16_t)battery_mv;
    *percent = a4_legacy_battery_percent_from_mv(battery_mv);
    return true;
}

uint8_t a4_board_read_battery_percent(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t cache_age_us = now_us - s_battery_percent_cache_time_us;
    if (s_battery_percent_cache_valid && cache_age_us >= 0 &&
        cache_age_us < 1500000) {
        s_battery_present = true;
        return s_battery_percent_cache;
    }

    uint16_t voltage_mv = 0;
    uint8_t percent = 0;
    const bool gauge_ok = s_fuel_gauge_available &&
        cw2015_read_measurement(&voltage_mv, &percent);
    const bool adc_ok = !s_fuel_gauge_available && s_battery_discovery_complete &&
        s_board_power_profile == A4_BOARD_POWER_LEGACY &&
        read_battery_percent_from_adc(&voltage_mv, &percent);
    if (gauge_ok || adc_ok) {
        s_battery_present = true;
        const uint8_t filtered_percent = a4_battery_percent_filter_update(
            &s_battery_percent_filter, percent, a4_board_is_charging());
        s_battery_percent_cache_valid = true;
        s_battery_percent_cache = filtered_percent;
        s_battery_percent_cache_time_us = now_us;
        const int source = gauge_ok ? 1 : 2;
        if (s_battery_source != source) {
            s_battery_source = source;
            ESP_LOGI(TAG, "battery source=%s voltage=%u mV soc=%u%%",
                     gauge_ok ? "CW2015" : "ADC", (unsigned)voltage_mv,
                     (unsigned)percent);
        }
        return filtered_percent;
    }

    s_battery_present = false;
    s_battery_percent_cache_valid = false;
    s_battery_percent_cache_time_us = 0;
    a4_battery_percent_filter_reset(&s_battery_percent_filter);
    a4_charging_debounce_reset(&s_charging_debounce);
    if (s_battery_source != -1) {
        s_battery_source = -1;
        ESP_LOGW(TAG, "battery source=none (selected source unavailable or read failed)");
    }
    return 0;
}

bool a4_board_battery_present(void)
{
    return s_battery_present;
}

bool a4_board_is_charging(void)
{
    if (!s_battery_present) {
        a4_charging_debounce_reset(&s_charging_debounce);
        return false;
    }
    const bool level_high = gpio_get_level(A4_GPIO_CHARGING) != 0;
    const bool charging = a4_charging_debounce_update(
        &s_charging_debounce, a4_charging_status_is_active(level_high),
        esp_timer_get_time());
    static int previous_charging = -1;
    if (previous_charging != (int)charging) {
        previous_charging = charging;
        ESP_LOGI(TAG, "charge detect GPIO%d stable=%d raw=%d active-high",
                 A4_GPIO_CHARGING, charging, level_high);
    }
    return charging;
}

bool a4_board_charging_debounce_pending(void)
{
    return s_battery_present && a4_charging_debounce_pending(&s_charging_debounce);
}

bool a4_board_has_frontlight(void)
{
    return s_frontlight_available;
}

static bool probe_lm3630a_presence(void)
{
    if (s_i2c_bus == NULL) {
        ESP_LOGE(TAG, "LM3630A_ABSENT reason=i2c-bus-not-ready");
        return false;
    }
    gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 1);
    for (int attempt = 0; attempt < 10; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 50 : 20));
        const esp_err_t probe = i2c_master_probe(
            s_i2c_bus, A4_FRONTLIGHT_ADDRESS, A4_I2C_TIMEOUT_MS);
        if (probe != ESP_OK) {
            ESP_LOGI(TAG, "LM3630A probe attempt=%d address=0x%02x result=%s",
                     attempt + 1, A4_FRONTLIGHT_ADDRESS,
                     esp_err_to_name(probe));
            continue;
        }
        i2c_master_dev_handle_t device = NULL;
        uint8_t control = 0;
        bool read_ok = add_i2c_device(A4_FRONTLIGHT_ADDRESS, &device) == ESP_OK;
        if (read_ok) {
            const uint8_t reg = 0x00;
            read_ok = i2c_master_transmit_receive(device, &reg, 1, &control, 1,
                                                    A4_I2C_TIMEOUT_MS) == ESP_OK;
        }
        if (device != NULL) {
            i2c_master_bus_rm_device(device);
        }
        if (read_ok) {
            ESP_LOGI(TAG, "LM3630A_PRESENT address=0x%02x reg0x00=0x%02x attempt=%d",
                     A4_FRONTLIGHT_ADDRESS, control, attempt + 1);
            return true;
        }
        ESP_LOGW(TAG, "LM3630A probe ACK but register-read failed attempt=%d",
                 attempt + 1);
    }
    gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 0);
    ESP_LOGW(TAG, "LM3630A_ABSENT address=0x%02x after 10 attempts",
             A4_FRONTLIGHT_ADDRESS);
    return false;
}

void a4_board_i2c_scan(void)
{
    if (s_i2c_bus == NULL) {
        ESP_LOGW(TAG, "I2C scan: bus not initialised");
        return;
    }
    const bool lm3630a_present = probe_lm3630a_presence();
    s_frontlight_available = lm3630a_present;
    int found = 0;
    for (uint8_t address = 0x08; address < 0x78; address++) {
        if (address == A4_FRONTLIGHT_ADDRESS) {
            if (lm3630a_present) {
                ESP_LOGI(TAG, "I2C ACK at 0x%02x (LM3630A frontlight)", address);
                found++;
            }
            continue;
        }
        if (i2c_master_probe(s_i2c_bus, address, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C ACK at 0x%02x", address);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %d device(s); LM3630A=%s",
             found, lm3630a_present ? "PRESENT" : "ABSENT");
}

bool a4_board_i2c_read(uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    if (s_i2c_bus == NULL || data == NULL || len == 0) {
        return false;
    }
    i2c_master_dev_handle_t device;
    if (add_i2c_device(address, &device) != ESP_OK) {
        return false;
    }
    const bool ok = i2c_master_transmit_receive(device, &reg, 1, data, len,
                                                A4_I2C_TIMEOUT_MS) == ESP_OK;
    i2c_master_bus_rm_device(device);
    return ok;
}


/* ---- GSL1680 bring-up.  The controller only scans after the host uploads
 * its firmware; the resident state left by a previous boot can pass the
 * 0xb0 memcheck yet never report touches, so we always do the full factory
 * sequence: hard reset, clear registers, reset, upload, start, verify. ---- */

static esp_err_t gsl_write_u32(uint8_t reg, uint32_t value)
{
    const uint8_t buf[5] = {reg,
                            (uint8_t)(value & 0xff),
                            (uint8_t)((value >> 8) & 0xff),
                            (uint8_t)((value >> 16) & 0xff),
                            (uint8_t)(value >> 24)};
    return i2c_master_transmit(s_touch, buf, sizeof(buf), A4_I2C_TIMEOUT_MS);
}

static esp_err_t gsl_write_u8(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_touch, buf, sizeof(buf), A4_I2C_TIMEOUT_MS);
}

static bool gsl_read_u32(uint8_t reg, uint32_t *value)
{
    uint8_t data[4];
    if (i2c_master_transmit_receive(s_touch, &reg, 1, data, sizeof(data),
                                    A4_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
             ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return true;
}

static bool gsl_verify_running(void)
{
    uint32_t magic = 0;
    if (!gsl_read_u32(0xb0, &magic) || magic != 0x5a5a5a5aU) {
        ESP_LOGW(TAG, "GSL memcheck failed: 0x%08lx", (unsigned long)magic);
        return false;
    }
    uint32_t counter_a = 0;
    uint32_t counter_b = 0;
    gsl_read_u32(0xb4, &counter_a);
    vTaskDelay(pdMS_TO_TICKS(60));
    gsl_read_u32(0xb4, &counter_b);
    if (counter_a == counter_b) {
        ESP_LOGW(TAG, "GSL scan counter frozen at %lu", (unsigned long)counter_a);
        return false;
    }
    return true;
}

static bool gsl_init_firmware(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        /* Hardware reset pulse on the dedicated reset line. */
        gpio_set_level(A4_GPIO_TOUCH_RESET, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(A4_GPIO_TOUCH_RESET, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* clr_reg + reset_chip per the GSLX680 reference flow. */
        gsl_write_u8(0xe0, 0x88);
        vTaskDelay(pdMS_TO_TICKS(20));
        gsl_write_u8(0x80, 0x03);
        vTaskDelay(pdMS_TO_TICKS(5));
        gsl_write_u8(0xe4, 0x04);
        vTaskDelay(pdMS_TO_TICKS(5));
        gsl_write_u8(0xe0, 0x00);
        vTaskDelay(pdMS_TO_TICKS(20));

        gsl_write_u8(0xe0, 0x88);
        vTaskDelay(pdMS_TO_TICKS(20));
        gsl_write_u8(0xe4, 0x04);
        vTaskDelay(pdMS_TO_TICKS(10));
        gsl_write_u32(0xbc, 0x00000000);
        vTaskDelay(pdMS_TO_TICKS(10));

        const int64_t start_us = esp_timer_get_time();
        esp_err_t err = ESP_OK;
        for (size_t i = 0; i < gsl1680_fw_count && err == ESP_OK; i++) {
            err = gsl_write_u32(gsl1680_fw[i].reg, gsl1680_fw[i].value);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GSL firmware upload failed: %s", esp_err_to_name(err));
            continue;
        }

        gsl_write_u8(0xe0, 0x00);
        vTaskDelay(pdMS_TO_TICKS(30));

        if (gsl_verify_running()) {
            ESP_LOGI(TAG, "GSL firmware loaded: %u pairs in %d ms (attempt %d)",
                     (unsigned)gsl1680_fw_count,
                     (int)((esp_timer_get_time() - start_us) / 1000),
                     attempt);
            return true;
        }
    }
    ESP_LOGE(TAG, "GSL firmware load failed after retries");
    return false;
}

bool a4_board_read_touch(uint8_t *finger_count, uint16_t *x, uint16_t *y)
{
    if (s_touch == NULL || finger_count == NULL || x == NULL || y == NULL) {
        return false;
    }
    /* GSL layout: 0x80 = finger count, 0x84.. = 4 bytes per point
     * (y low, y high nibble, x low, x high nibble | id << 4). */
    const uint8_t reg = 0x80;
    uint8_t buf[8] = {0};
    if (i2c_master_transmit_receive(s_touch, &reg, 1, buf, sizeof(buf),
                                    A4_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *finger_count = buf[0];
    *y = (uint16_t)(((buf[5] & 0x0f) << 8) | buf[4]);
    *x = (uint16_t)(((buf[7] & 0x0f) << 8) | buf[6]);
    return true;
}

/* LM3630A frontlight, register recipe reverse-engineered from the factory
 * firmware (function at runtime 0x4204899c in app0):
 *   probe -> write 0x50=0x03 (undocumented filter/init register)
 *   cfg 0x01: clear bits 0x07 (PWM off), boost 0x02=0x38,
 *   currents 0x05/0x06=0x1F, ctrl: clear 0x14 and 0x0B (EXPONENTIAL mode,
 *   both channels disabled), brightness regs zeroed.
 * Per-channel set: clear sleep (mask 0x80), 2 ms, write BRT (value scaled
 * 0..150, factory caps at 150), then set only that channel enable bit
 * (0x04 pairs with BRT_A, 0x02 with BRT_B).
 * Channel naming per on-device observation: BRT_A drives the warm string,
 * BRT_B the cold one. */

enum {
    LM3630A_REG_CONTROL = 0x00,
    LM3630A_REG_CONFIGURATION = 0x01,
    LM3630A_REG_BOOST_CONTROL = 0x02,
    LM3630A_REG_BRIGHTNESS_WARM = 0x03,
    LM3630A_REG_BRIGHTNESS_COLD = 0x04,
    LM3630A_REG_CURRENT_WARM = 0x05,
    LM3630A_REG_CURRENT_COLD = 0x06,
    LM3630A_REG_FILTER_STRENGTH = 0x50,
    LM3630A_AWAKE_POLL_COUNT = 10,
    LM3630A_AWAKE_POLL_DELAY_MS = 2,
};

static bool fl_write(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(device, buf, 2, A4_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool fl_read(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *value)
{
    return value != NULL &&
           i2c_master_transmit_receive(device, &reg, 1, value, 1,
                                       A4_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool fl_update(i2c_master_dev_handle_t device, uint8_t reg,
                      uint8_t mask, uint8_t value)
{
    uint8_t old = 0;
    if (!fl_read(device, reg, &old)) {
        return false;
    }
    return fl_write(device, reg, (uint8_t)((old & ~mask) | (value & mask)));
}

static bool fl_wait_awake(i2c_master_dev_handle_t device)
{
    for (unsigned attempt = 0; attempt < LM3630A_AWAKE_POLL_COUNT; ++attempt) {
        uint8_t control = 0;
        if (!fl_read(device, LM3630A_REG_CONTROL, &control)) {
            return false;
        }
        if ((control & A4_FRONTLIGHT_CONTROL_SLEEP_STATUS) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(LM3630A_AWAKE_POLL_DELAY_MS));
    }
    ESP_LOGW(TAG, "frontlight did not leave sleep mode");
    return false;
}

static bool fl_verify_active(i2c_master_dev_handle_t device,
                             uint8_t warm_value,
                             uint8_t cold_value,
                             uint8_t enable_mask)
{
    uint8_t control = 0;
    uint8_t warm = 0;
    uint8_t cold = 0;
    if (!fl_read(device, LM3630A_REG_CONTROL, &control) ||
        !fl_read(device, LM3630A_REG_BRIGHTNESS_WARM, &warm) ||
        !fl_read(device, LM3630A_REG_BRIGHTNESS_COLD, &cold)) {
        return false;
    }

    const uint8_t control_mask = A4_FRONTLIGHT_CONTROL_SLEEP_COMMAND |
                                 A4_FRONTLIGHT_CONTROL_SLEEP_STATUS |
                                 A4_FRONTLIGHT_CONTROL_CHANNEL_ENABLE_MASK;
    const bool matches = (control & control_mask) == enable_mask &&
                         warm == warm_value && cold == cold_value;
    if (!matches) {
        ESP_LOGW(TAG,
                 "frontlight readback ctrl=0x%02x warm=0x%02x cold=0x%02x",
                 control, warm, cold);
    }
    return matches;
}

static bool fl_configure_active(i2c_master_dev_handle_t device,
                                uint8_t warm_value,
                                uint8_t cold_value)
{
    const uint8_t enable_mask = a4_frontlight_channel_enable_mask(
        warm_value != 0, cold_value != 0);

    /* Brightness registers ignore writes while SLEEP_STATUS is set. Start
     * from a deterministic exponential, all-channels-off control word, then
     * wait for the chip to wake before programming either color. */
    const bool configured =
        fl_write(device, LM3630A_REG_FILTER_STRENGTH, 0x03) &&
        fl_update(device, LM3630A_REG_CONFIGURATION, 0x07, 0x00) &&
        fl_write(device, LM3630A_REG_BOOST_CONTROL, 0x38) &&
        fl_update(device, LM3630A_REG_CURRENT_WARM, 0x1f, 0x1f) &&
        fl_write(device, LM3630A_REG_CURRENT_COLD, 0x1f) &&
        fl_update(device, LM3630A_REG_CONTROL,
                  A4_FRONTLIGHT_CONTROL_WRITABLE_MASK, 0x00);
    if (!configured || !fl_wait_awake(device)) {
        return false;
    }

    return fl_write(device, LM3630A_REG_BRIGHTNESS_WARM, warm_value) &&
           fl_write(device, LM3630A_REG_BRIGHTNESS_COLD, cold_value) &&
           fl_update(device, LM3630A_REG_CONTROL,
                     A4_FRONTLIGHT_CONTROL_CHANNEL_ENABLE_MASK,
                     enable_mask) &&
           fl_verify_active(device, warm_value, cold_value, enable_mask);
}

bool a4_board_set_frontlight(uint8_t cold_percent, uint8_t warm_percent)
{
    if (!s_frontlight_available) {
        return false;
    }

    i2c_master_dev_handle_t device;
    if (add_i2c_device(A4_FRONTLIGHT_ADDRESS, &device) != ESP_OK) {
        return false;
    }

    bool ok;
    if (cold_percent == 0 && warm_percent == 0) {
        ok = fl_write(device, LM3630A_REG_BRIGHTNESS_WARM, 0x00) &&
             fl_write(device, LM3630A_REG_BRIGHTNESS_COLD, 0x00) &&
             fl_update(device, LM3630A_REG_CONTROL,
                       A4_FRONTLIGHT_CONTROL_CHANNEL_ENABLE_MASK, 0x00) &&
             fl_update(device, LM3630A_REG_CONTROL,
                       A4_FRONTLIGHT_CONTROL_SLEEP_COMMAND,
                       A4_FRONTLIGHT_CONTROL_SLEEP_COMMAND);
        gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 0);
        s_frontlight_rail_on = false;
    } else {
        /* The chip needs ~50 ms after HWEN before it ACKs (same finding as
         * the presence probe).  GPIO output levels cannot be read back, so
         * the rail state is tracked in software. */
        const bool was_off = !s_frontlight_rail_on;
        gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 1);
        s_frontlight_rail_on = true;
        vTaskDelay(pdMS_TO_TICKS(was_off ? 50 : 2));

        /* The chip runs the factory exponential dimming curve, where low
         * register values emit almost nothing (the higher-Vf cold string
         * stays dark below ~70).  Map 1-100% onto 45..150 so every step is
         * actually visible on both strings. */
        const uint8_t warm_pct = warm_percent > 100 ? 100 : warm_percent;
        const uint8_t cold_pct = cold_percent > 100 ? 100 : cold_percent;
        const uint8_t warm_value =
            warm_pct == 0 ? 0 : (uint8_t)(45u + ((uint32_t)warm_pct * 105u) / 100u);
        const uint8_t cold_value =
            cold_pct == 0 ? 0 : (uint8_t)(45u + ((uint32_t)cold_pct * 105u) / 100u);

        ok = false;
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (attempt > 0) {
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            ok = fl_configure_active(device, warm_value, cold_value);
            if (!ok) {
                ESP_LOGW(TAG, "frontlight sequence attempt %d failed", attempt);
            }
        }
    }
    i2c_master_bus_rm_device(device);
    return ok;
}

bool a4_board_reprobe_frontlight(void)
{
    if (s_i2c_bus == NULL) {
        return false;
    }
    gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 1);
    bool found = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 50 : 20));
        const esp_err_t err =
            i2c_master_probe(s_i2c_bus, A4_FRONTLIGHT_ADDRESS, A4_I2C_TIMEOUT_MS);
        ESP_LOGI(TAG, "frontlight probe %d (EN high, +%dms): %s", attempt,
                 50 + attempt * 20, esp_err_to_name(err));
        if (err == ESP_OK) {
            found = true;
            break;
        }
    }
    if (!found) {
        gpio_set_level(A4_GPIO_FRONTLIGHT_ENABLE, 0);
    }
    s_frontlight_available = found;
    return found;
}

bool a4_board_i2c_write(uint8_t address, uint8_t reg, uint8_t value)
{
    if (s_i2c_bus == NULL) {
        return false;
    }
    i2c_master_dev_handle_t device;
    if (add_i2c_device(address, &device) != ESP_OK) {
        return false;
    }
    const uint8_t buf[2] = {reg, value};
    const bool ok =
        i2c_master_transmit(device, buf, 2, A4_I2C_TIMEOUT_MS) == ESP_OK;
    i2c_master_bus_rm_device(device);
    return ok;
}
