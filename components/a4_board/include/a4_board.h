#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "a4_battery_smoothing.h"
#include "a4_datetime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    A4_BUTTON_UP = 0,
    A4_BUTTON_DOWN,
    A4_BUTTON_POWER,
    A4_BUTTON_COUNT,
} a4_button_t;

/* Called from the GPIO ISR for UP, DOWN, or POWER. The callback must only use
 * IRAM-safe ISR APIs (for example task notifications); it must not perform
 * I2C, file, logging, or UI work. */
typedef void (*a4_board_reader_key_isr_t)(a4_button_t button, void *context);

/* Called on either edge from the running GSL1680's active-high INT output
 * (GPIO9). The release edge is required so the sampler can observe a zero-
 * finger report and dispatch the completed gesture. The callback must only
 * use IRAM-safe ISR APIs; it must not perform I2C, file, logging, or UI work. */
typedef void (*a4_board_touch_int_isr_t)(void *context);

/* Called from the GPIO ISR when the charger-present input changes. */
typedef void (*a4_board_charging_isr_t)(void *context);

typedef enum {
    A4_WAKEUP_OTHER = 0,
    A4_WAKEUP_TIMER,
    A4_WAKEUP_POWER_BUTTON,
    A4_WAKEUP_READER_KEY,
    A4_WAKEUP_TOUCH,
    A4_WAKEUP_CHARGING,
} a4_wakeup_reason_t;

/* 在任何 SD/USB MSC 访问前探测 CW2015 并绑定主板供电策略；未知板型或
 * 绑定存储异常返回错误，不自动擦除 NVS，不猜测 GPIO6 的运行极性。 */
esp_err_t a4_board_init(void);
/* 初始化其余共享 I2C 设备；总线已由主板识别创建，触摸固件/RTC/前光
 * 仍在这里初始化。重复调用不会跳过首次初始化，也不会重复上传固件。 */
esp_err_t a4_board_init_i2c_devices(void);
/* Installs IRAM-safe any-edge ISRs for UP=GPIO5, DOWN=GPIO7 and POWER=GPIO8.
 * Repeated registration is harmless. */
esp_err_t a4_board_register_reader_key_isr(a4_board_reader_key_isr_t callback,
                                           void *context);
esp_err_t a4_board_register_touch_int_isr(a4_board_touch_int_isr_t callback,
                                          void *context);
esp_err_t a4_board_register_charging_isr(a4_board_charging_isr_t callback,
                                         void *context);
bool a4_board_touch_interrupt_active(void);
bool a4_board_button_pressed(a4_button_t button);
bool a4_board_read_datetime(a4_datetime_t *datetime);
bool a4_board_write_datetime(const a4_datetime_t *datetime);
/* Clear PCF8563 time during factory reset; leaves the oscillator stopped. */
bool a4_board_clear_datetime(void);
uint8_t a4_board_read_battery_percent(void);
bool a4_board_battery_present(void);
bool a4_board_is_charging(void);
/* True between a charger GPIO edge and its 200 ms stable recheck.  Callers
 * must keep the MCU awake while this is true so the indicator cannot miss an
 * insert or removal just before sleep. */
bool a4_board_charging_debounce_pending(void);
bool a4_board_has_frontlight(void);

/* 新主板 GPIO6 低有效，可在释放 SD 总线后关闭电源。
 * CW2015 探测/NVS 绑定选择板型；旧板始终保持高电平，关闭请求返回
 * ESP_ERR_NOT_SUPPORTED，新板电量计读取失败不改变已绑定的电源策略。
 * 调用方必须先检查是否可断电，再决定是否卸载 SD。 */
esp_err_t a4_board_set_sd_power(bool enabled);
bool a4_board_sd_power_enabled(void);
bool a4_board_sd_power_can_suspend(void);

/* Application sleep: gate the GSL INT line and keep TOUCH_RESET high so
 * firmware stays resident (fast wake without re-upload). Official chip
 * green-mode I2C command is not in-tree yet. Transport deep sleep still
 * holds RST low separately. Wake with a4_board_wake_touch(). */
void a4_board_sleep_touch(void);
bool a4_board_wake_touch(void);

/* Enter ESP32-S3 light sleep with the active-high POWER key and the active-high
 * charging detect GPIO11 as wake sources. The charging input arms the opposite
 * level of the one sampled before sleep, matching the reader idle path, so an
 * already attached cable does not wake the device immediately.
 * timer_wakeup_us 为 0 时关闭定时唤醒；非零时增加指定间隔的定时唤醒。
 * GPIO4 and the board-specific power GPIO6 remain driven exactly as configured
 * before the call, so the board latch and selected SD rail state are stable;
 * the EPD session may be deliberately closed before sleep. */
esp_err_t a4_board_light_sleep(uint64_t timer_wakeup_us,
                               a4_wakeup_reason_t *reason);
/* Reader idle sleep keeps the EPD, SD rail and running GSL1680 powered. GPIO5
 * and GPIO7 wake for page keys, GPIO8 wakes for the power key, and GPIO9 wakes
 * for the GSL touch interrupt. Unlike lock-screen sleep this function does
 * not reset touch or force the modem/panel power domains off. */
esp_err_t a4_board_reader_light_sleep(uint64_t timer_wakeup_us,
                                      a4_wakeup_reason_t *reason);
/* 仅用于引导完成前的运输态。调用方须卸载 SD 并关闭可控电源、等待电源键
 * 释放；保持 GPIO4 高、GPIO6 高、触摸复位拉低和前光关闭。EXT0 电源键唤醒；
 * 未在充电时额外尝试 EXT1(GPIO11) 插电唤醒。成功不返回，失败解除本次新增的
 * 保持和唤醒配置后返回错误。 */
esp_err_t a4_board_transport_deep_sleep(void);
/* Pull the latest hardware's MCU power latch GPIO4 low after the final
 * lockscreen frame has been submitted. This cuts board power and does not
 * return. */
void a4_board_power_off(void) __attribute__((noreturn));

/* Debug: log every ACKing address on the shared I2C bus. */
void a4_board_i2c_scan(void);

/* Debug: single register read from an arbitrary bus device. */
bool a4_board_i2c_read(uint8_t address, uint8_t reg, uint8_t *data, size_t len);

/* Debug: single register write to an arbitrary bus device. */
bool a4_board_i2c_write(uint8_t address, uint8_t reg, uint8_t value);

/* Reads the GSL touch state: finger count plus the first touch point in
 * controller-native coordinates.  Returns false when no controller or the
 * bus read failed. */
bool a4_board_read_touch(uint8_t *finger_count, uint16_t *x, uint16_t *y);

/* Sets both LM3630A channels (0 = off, 1-100 = percent).  Returns false
 * when no frontlight controller is fitted or the bus write failed. */
bool a4_board_set_frontlight(uint8_t cold_percent, uint8_t warm_percent);

/* Verbose frontlight re-probe with HWEN held high; updates the cached
 * availability flag and returns the outcome. */
bool a4_board_reprobe_frontlight(void);

#ifdef __cplusplus
}
#endif
