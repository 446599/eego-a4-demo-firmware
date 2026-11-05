#include "epd_uc8279c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    UC8279C_WIDTH = EPD_UC8279C_WIDTH_PX,
    UC8279C_VISIBLE_HEIGHT = EPD_UC8279C_VISIBLE_HEIGHT_PX,
    UC8279C_RAM_HEIGHT = 600,
    UC8279C_BYTES_PER_ROW = UC8279C_WIDTH / 8,
    UC8279C_LUT_PHASE_BYTES = 49,
    UC8279C_MONO_LUT_PHASE_BYTES = 42,
    UC8279C_GRAY4_LUT_PHASE_BYTES = 42,
    UC8279C_GRAY4_LUT_SLOT_BYTES = 6,
    UC8279C_GRAY4_LUT_SLOT_COUNT =
        UC8279C_GRAY4_LUT_PHASE_BYTES / UC8279C_GRAY4_LUT_SLOT_BYTES,
    UC8279C_LUT_PHASE_COUNT = 5,
    UC8279C_GRAY16_TRIM_LUT_SIZE_BYTES =
        UC8279C_GRAY4_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT,
    UC8279C_GRAY16_FACTORY_B_TIME_DIVISOR = 4,
    UC8279C_BUSY_TIMEOUT_MS = 15000,
    UC8279C_CORNER_RADIUS_PX = EPD_UC8279C_VISIBLE_CORNER_RADIUS_PX,
    UC8279C_BORDER_THICKNESS_PX = EPD_UC8279C_UI_CLIP_INSET_PX,

    /* 0x50 DDX[1:0] selects the transition source. The official 1.2.8 DU
     * pairs its 49-byte banks with DDX=11 (0xd7). DDX=01 (0xd5) is retained
     * only for the separate 42-byte OLD/NEW experiments. */
    UC8279C_CDI_DU_FACTORY = 0xd7,
    UC8279C_CDI_DU_NEW_OLD = 0xd5,
    UC8279C_CDI_GC_NEW_OLD = 0x97,

    /* The reader transition path uses the two DTM planes as a four-way
     * selector, not as the target gray image itself.  The selector integer is
     * (DTM1/OLD << 1) | (DTM2/NEW << 0).  The UC8179C data sheet prints its
     * CDI table as {NEW, OLD}, so its 01/10 labels appear reversed relative
     * to the integer used here.  With 0x50=00 and the A4 bank map below, 00
     * is idle, 01 selects the middle-gray dose, 10 selects the strongest
     * black dose, and 11 selects the white-clear trajectory.  Keep these
     * values explicit: swapping the two bits silently weakens black text or
     * drives an unchanged white background. */
    UC8279C_GRAY3_SELECTOR_IDLE = 0x00,
    UC8279C_GRAY3_SELECTOR_GRAY = 0x01,
    UC8279C_GRAY3_SELECTOR_BLACK = 0x02,
    UC8279C_GRAY3_SELECTOR_WHITE = 0x03,

    /* LAB ONLY: rebuild with exactly one of these modes. LEGACY is the
     * default. TIMED_ZERO is a source-code-zero timing experiment, NOT a
     * claim of zero effective field. TARGET_BLACK removes the recovered
     * white-prefix level codes and shares the black dose with B2B; it is
     * NOT a separately calibrated short B2B waveform. No mode is optically
     * validated here. Recovery/maintenance always use the legacy recipe. */
    UC8279C_GRAY3_RETENTION_LEGACY = 0,
    UC8279C_GRAY3_RETENTION_TIMED_ZERO = 1,
    UC8279C_GRAY3_RETENTION_TARGET_BLACK = 2,
    UC8279C_GRAY3_RETENTION_EXPERIMENT = UC8279C_GRAY3_RETENTION_LEGACY,


    /* LAB ONLY: UC8179C-compatible EVS hypothesis, NOT verified for UC8279C.
     * 0 sends no R52 command. 1 establishes the explicit R52=02 control.
     * Only after control matches baseline, 2 tests VCEND floating (R52=0a).
     * Keep retention LEGACY and every LUT/power/scan setting unchanged.
     * Each successful experiment restores the assumed control R52=02 so
     * the candidate cannot leak into the next mono or diagnostic refresh. */
    UC8279C_GRAY3_EVS_UNTOUCHED = 0,
    UC8279C_GRAY3_EVS_CONTROL_02 = 1,
    UC8279C_GRAY3_EVS_FLOAT_0A = 2,
    UC8279C_GRAY3_EVS_EXPERIMENT = UC8279C_GRAY3_EVS_UNTOUCHED,

    /* PSR/PLL values used by the production reset path and by the reversible
     * Gate-direction probe.  UC8279C is treated as a UC8179C-compatible
     * hypothesis here; the physical propagation direction still requires
     * slow-motion validation on the mounted A4 glass. */
    UC8279C_PSR_DEFAULT_0 = 0x3f,
    UC8279C_PSR_DEFAULT_1 = 0x4a,
    UC8279C_PSR_GATE_DIRECTION_BIT = 0x08,
    UC8279C_PLL_DEFAULT = 0x0f,

    /* The recovered reader white-clear trajectory is two plus sixteen
     * frames.  The selective variant changes only its second timing slot to
     * twenty-two frames, for a 24-frame total.  It is never selected when
     * the old optical frame is unknown. */
    UC8279C_READER_GRAY3_WHITE_CLEAR_BANK = 1,
    UC8279C_READER_GRAY3_WHITE_CLEAR_COUNT_OFFSET = 2,
    UC8279C_READER_GRAY3_WHITE_CLEAR_SECOND_SLOT_FRAMES = 22,

    /* The strongest reader-black trajectory keeps the recovered 18-frame
     * white prefix, followed by fourteen black-drive frames.  Increase only
     * that final black drive to eighteen frames.  The upper two bits (the
     * electrical drive selection) are deliberately preserved. */
    UC8279C_READER_GRAY3_BLACK_BANK = 3,
    UC8279C_READER_GRAY3_BLACK_COUNT_OFFSET = 8,
    UC8279C_READER_GRAY3_BLACK_DRIVE_FRAMES = 18,

};

extern const uint8_t uc8279c_lut_gc[245];
extern const size_t uc8279c_lut_gc_len;
extern const uint8_t uc8279c_lut_gc_custom_v1[245];
extern const size_t uc8279c_lut_gc_custom_v1_len;
extern const uint8_t uc8279c_lut_gc_xiaodouzi[245];
extern const size_t uc8279c_lut_gc_xiaodouzi_len;
extern const uint8_t uc8279c_lut_du[245];
extern const size_t uc8279c_lut_du_len;
extern const uint8_t uc8279c_lut_page_turn_single_frame_v1[245];
extern const size_t uc8279c_lut_page_turn_single_frame_v1_len;
extern const uint8_t uc8279c_lut_du_fast[245];
extern const size_t uc8279c_lut_du_fast_len;
extern const uint8_t uc8279c_lut_gc_a4_factory[210];
extern const size_t uc8279c_lut_gc_a4_factory_len;
extern const uint8_t uc8279c_lut_du_a4_factory[210];
extern const size_t uc8279c_lut_du_a4_factory_len;
extern const uint8_t uc8279c_lut_du_xiaodouzi[245];
extern const size_t uc8279c_lut_du_xiaodouzi_len;
extern const uint8_t uc8279c_lut_gray4_factory_a[210];
extern const size_t uc8279c_lut_gray4_factory_a_len;
extern const uint8_t uc8279c_lut_gray4_factory_b[210];
extern const size_t uc8279c_lut_gray4_factory_b_len;
extern const uint8_t uc8279c_lut_gray4_reader_white_clear[245];
extern const size_t uc8279c_lut_gray4_reader_white_clear_len;
extern const uint8_t uc8279c_lut_gray4_factory_c[210];
extern const size_t uc8279c_lut_gray4_factory_c_len;
extern const uint8_t uc8279c_lut_gray4_waveshare_4in2[210];
extern const size_t uc8279c_lut_gray4_waveshare_4in2_len;
extern const uint8_t uc8279c_lut_gray4_a4[245];
extern const size_t uc8279c_lut_gray4_a4_len;

typedef enum {
    GRAY3_TIMING_BASELINE = 0,
    GRAY3_TIMING_ADVANCED,
    GRAY3_TIMING_BLACK_CONTROL,
    GRAY3_TIMING_ALIGNED,
    GRAY3_TIMING_RETURN,
    GRAY3_TIMING_REVERSE,
    GRAY3_TIMING_REVERSE_TIMED_HOLD,
    GRAY3_TIMING_RECOVERY_ALIGNED,
} gray3_timing_t;

static const char *TAG = "uc8279c";
static epd_uc8279c_config_t s_config;
static spi_device_handle_t s_spi;
typedef enum {
    PANEL_SESSION_NONE = 0,
    PANEL_SESSION_MONO,
    PANEL_SESSION_GRAY3,
    PANEL_SESSION_GRAY4,
    PANEL_SESSION_GRAY49,
} panel_session_mode_t;
static panel_session_mode_t s_panel_session_mode;
/* 仅专用唤醒黑帧成功后授予一次复用资格；不把普通 MONO 会话全局
 * 视为灰阶会话。任何代次/配置/刷新变化都使该资格失效。 */
typedef struct {
    bool pending;
    uint32_t session_generation;
    uint32_t profile_generation;
    uint32_t refresh_count;
} reader_wake_black_handoff_t;
static reader_wake_black_handoff_t s_reader_wake_black_handoff;
/* The reader LUT is immutable after its first construction, but the
 * controller's programmed waveform is session-local. Reuse it while a gray3
 * session remains powered, and resend after reset or POWER_OFF. */
static bool s_reader_gray3_lut_loaded;
static bool s_reader_gray3_lut_white_clear_boosted;
static bool s_reader_gray3_lut_black_boosted;
static uint32_t s_reader_gray3_lut_fingerprint;
static uint32_t s_reader_gray3_lut_profile_generation;
static uint32_t s_reader_gray3_lut_session_generation;
static uint32_t s_panel_session_generation;
static bool s_gray3_full_ram_mode;
static uint8_t s_reader_gray3_black_drive_frames =
    UC8279C_READER_GRAY3_BLACK_DRIVE_FRAMES;
static uint8_t s_gray3_reverse_white_frames = 2U;
/* PSRAM framebuffers cannot be used as SPI DMA sources. Every framebuffer
 * row is synchronously copied here before it is submitted to the controller. */
DMA_ATTR static uint8_t s_dma_row[UC8279C_BYTES_PER_ROW];
static epd_uc8279c_busy_wait_hook_t s_busy_wait_hook;
static void *s_busy_wait_hook_context;
static uint8_t s_gray16_custom_trim_lut[UC8279C_GRAY16_TRIM_LUT_SIZE_BYTES];
static bool s_gray16_custom_trim_lut_enabled;
static uint8_t s_gray16_factory_b_duration_divisor =
    UC8279C_GRAY16_FACTORY_B_TIME_DIVISOR;
static uint8_t s_gray16_factory_b_active_bank = 2;
static bool s_driver_initialized;
/* 控制器休眠与图像会话有效性独立；灰阶提交会丢弃会话，但控制器仍唤醒。 */
static bool s_controller_sleeping;
static uint32_t s_display_refresh_count;
/* Software copy of the last scan-timing values sent in the current session.
 * After invalidation the controller is considered unknown and the next
 * hardware reset always writes the production defaults again. */
static uint8_t s_scan_psr0 = UC8279C_PSR_DEFAULT_0;
static uint8_t s_scan_pll = UC8279C_PLL_DEFAULT;
static bool s_scan_debug_override_active;
static uint32_t s_live_profile_generation;
static const epd_uc8279c_live_profile_t s_live_profile_stable = {
    .power_setting = {0x03, 0x00, 0x72, 0x72, 0x17},
    .booster = {0x25, 0x25, 0x3c, 0x00},
    .booster_len = 3,
    .gc_cdi = UC8279C_CDI_GC_NEW_OLD,
    .du_cdi = UC8279C_CDI_DU_FACTORY,
    .gc_lut = EPD_UC8279C_GC_LUT_FACTORY_49,
    .du_lut = EPD_UC8279C_DU_LUT_FACTORY_49,
};
static epd_uc8279c_live_profile_t s_live_profile = {
    .power_setting = {0x03, 0x00, 0x72, 0x72, 0x17},
    .booster = {0x25, 0x25, 0x3c, 0x00},
    .booster_len = 3,
    .gc_cdi = UC8279C_CDI_GC_NEW_OLD,
    .du_cdi = UC8279C_CDI_DU_FACTORY,
    .gc_lut = EPD_UC8279C_GC_LUT_FACTORY_49,
    .du_lut = EPD_UC8279C_DU_LUT_FACTORY_49,
};

static esp_err_t write_visible_framebuffer_continuous_cs(
    const uint8_t *framebuffer);
static esp_err_t write_gray3_transition_plane_continuous_cs(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    bool old_valid, epd_uc8279c_gray3_refresh_reason_t reason,
    uint8_t selector_mask, uint32_t *out_digest, int *out_failure_row);

/* Keep every experimental profile behind an explicit selector. Production
 * DU uses the 245-byte table reconstructed from this A4's official 1.2.8
 * firmware; the cross-project Xiaodouzi profiles remain disabled. */
#define UC8279C_USE_CUSTOM_GC_V1 0
#define UC8279C_USE_XIAODOUZI_GC 0
#define UC8279C_USE_XIAODOUZI_DU_FAST 0
#define UC8279C_USE_XIAODOUZI_DU_REFERENCE 0
#define UC8279C_USE_DU_A4_FACTORY_42 0
/* With a 42-byte OLD/NEW experiment selected, DDX=01 makes LUTKK serve
 * unchanged black pixels. Clearing that bank leaves them floating while
 * VCOM toggles, so keep the no-KK variant disabled by default. */
#define UC8279C_USE_DU_A4_FACTORY_NO_KK 0
#define UC8279C_USE_DU_BLACK_BOOST 0
#define UC8279C_USE_DU_BLACK_DURATION_BOOST 0
/* Extend only the black->black (LUTKK, bank 0x24) state.  In the native
 * 49-byte table byte 1 is part of the drive selector/timing encoding; the
 * previously tested 0x64->0x84 change made DU fail to render.  Byte 5 is the
 * state repeat count, so the current profile adds two extra black-hold passes
 * while leaving the white/black transition timing untouched. */
#define UC8279C_USE_DU_BLACK_BLACK_REPEAT 0
#define UC8279C_DU_BLACK_BLACK_REPEAT_COUNT 0x03
/* The A4 sample exposes 42-byte banks, while the extracted production
 * firmware writes complete 49-byte UC8279 banks. Keep the sample selectable
 * for comparison, but use the complete factory bank in production. */
#define UC8279C_USE_A4_GC_42 0

/* The A4 factory DU table is the native seven-slot UC8179C format (42 bytes
 * per bank).  Build one controlled high-dose variant from that table instead
 * of altering the retained 49-byte official profile in place.  Slot 0 is the
 * active black/white drive state on this panel; repeating VCOM together with
 * white->black and black->black adds exactly one extra black drive pass. */
static uint8_t s_du_black_boost_lut[210];
static bool s_du_black_boost_lut_ready;

/* Keep the device-proven 49-byte DU profile, but extend only the final
 * white-to-black drive duration.  The active state is the first 7-byte
 * record in LUTWK (0x23), where byte 1 is its frame count (0x64 in the
 * factory table).  Do not change the voltage selector or repeat byte. */
static uint8_t s_du_black_duration_lut[245];
static bool s_du_black_duration_lut_ready;

static uint8_t s_du_black_black_repeat_lut[245];
static bool s_du_black_black_repeat_lut_ready;
static uint8_t s_du_a4_no_kk_lut[210];
static bool s_du_a4_no_kk_lut_ready;

static const uint8_t *du_a4_no_kk_lut(void)
{
    if (!s_du_a4_no_kk_lut_ready) {
        memcpy(s_du_a4_no_kk_lut,
               uc8279c_lut_du_a4_factory,
               sizeof(s_du_a4_no_kk_lut));
        /* Register 0x24 is LUTKK, the fifth 42-byte bank.  An unchanged
         * black pixel must not receive a white/black transition pulse. */
        memset(s_du_a4_no_kk_lut + (4 * UC8279C_MONO_LUT_PHASE_BYTES),
               0,
               UC8279C_MONO_LUT_PHASE_BYTES);
        s_du_a4_no_kk_lut_ready = true;
    }
    return s_du_a4_no_kk_lut;
}

static const uint8_t *du_black_duration_lut(void)
{
    if (!s_du_black_duration_lut_ready) {
        memcpy(s_du_black_duration_lut,
               uc8279c_lut_du,
               sizeof(s_du_black_duration_lut));
        s_du_black_duration_lut[(3 * 49) + 1] = 0x84; /* LUTWK frame-0 */
        s_du_black_duration_lut_ready = true;
    }
    return s_du_black_duration_lut;
}

static const uint8_t *du_black_black_repeat_lut(void)
{
    if (!s_du_black_black_repeat_lut_ready) {
        memcpy(s_du_black_black_repeat_lut,
               uc8279c_lut_du,
               sizeof(s_du_black_black_repeat_lut));
        /* Bank 0x24 (LUTKK), first seven-byte state, repeat field. */
        s_du_black_black_repeat_lut[(4 * 49) + 5] =
            UC8279C_DU_BLACK_BLACK_REPEAT_COUNT;
        s_du_black_black_repeat_lut_ready = true;
    }
    return s_du_black_black_repeat_lut;
}

static const uint8_t *du_black_boost_lut(void)
{
    if (!s_du_black_boost_lut_ready) {
        memcpy(s_du_black_boost_lut,
               uc8279c_lut_du_a4_factory,
               sizeof(s_du_black_boost_lut));
        /* Seven six-byte slots: byte 5 is the slot repeat count. */
        s_du_black_boost_lut[(0 * 42) + 5] = 3; /* LUTC */
        s_du_black_boost_lut[(3 * 42) + 5] = 3; /* LUTWK: white -> black */
        s_du_black_boost_lut[(4 * 42) + 5] = 3; /* LUTKK: black hold */
        s_du_black_boost_lut_ready = true;
    }
    return s_du_black_boost_lut;
}

/* A's factory branch uses 0xa9 07 after its waveform bank, but this panel
 * holds BUSY low with both that pair and C's 0x29 07 pair. Keep the CDI
 * pairing selectable while isolating the A LUT itself: 0=strict factory,
 * 1=C-compatible pair, 2=no 0x50 (same register state as factory B). */
#define UC8279C_GRAY4_A_CDI_VARIANT 2

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t transmit(const void *data, size_t size)
{
    if (data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t transaction = {
        .length = size * 8,
        .tx_buffer = data,
    };
    return spi_device_transmit(s_spi, &transaction);
}

static esp_err_t send_command(uint8_t command)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_dc, 0), TAG, "DC command level");
    const esp_err_t err = transmit(&command, sizeof(command));
    if (err == ESP_OK && command == 0x12) {
        /* Count the command at the wire boundary.  A later BUSY timeout is
         * still a real activation attempt and must not be hidden from the
         * single-refresh diagnostics. */
        ++s_display_refresh_count;
    }
    return err;
}

static esp_err_t send_data(const void *data, size_t size)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_dc, 1), TAG, "DC data level");
    return transmit(data, size);
}

static esp_err_t send_command_data(uint8_t command, const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_ERROR(send_command(command), TAG, "command 0x%02x", command);
    if (size > 0) {
        ESP_RETURN_ON_ERROR(send_data(data, size), TAG, "data for command 0x%02x", command);
    }
    return ESP_OK;
}

/* The factory gray4 0x50 sequence raises CS after each data byte. Keep this
 * separate from bulk register/LUT writes so this narrow compatibility path
 * does not change the normal command-data transport. */
static esp_err_t send_command_data_bytewise(uint8_t command,
                                            const uint8_t *data,
                                            size_t size)
{
    ESP_RETURN_ON_ERROR(send_command(command), TAG, "command 0x%02x", command);
    for (size_t index = 0; index < size; ++index) {
        ESP_RETURN_ON_ERROR(send_data(data + index, 1), TAG,
                            "byte %u for command 0x%02x",
                            (unsigned)index, command);
    }
    return ESP_OK;
}

static esp_err_t wait_until_idle(uint32_t timeout_ms, const char *stage)
{
    const int64_t start_us = esp_timer_get_time();
    const int busy_level = gpio_get_level(s_config.pin_busy);
    const int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (gpio_get_level(s_config.pin_busy) == 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "BUSY stage=%s initial=%d waited=%lldus timeout=%lums",
                     stage, busy_level,
                     (long long)(esp_timer_get_time() - start_us),
                     (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(2);
        if (s_busy_wait_hook != NULL) {
            s_busy_wait_hook(s_busy_wait_hook_context);
        }
    }
    ESP_LOGI(TAG, "BUSY stage=%s initial=%d waited=%lldus",
             stage, busy_level, (long long)(esp_timer_get_time() - start_us));
    return ESP_OK;
}

/* Wait for an activation command to actually assert BUSY.  Some controllers
 * assert the line a short but nonzero time after the SPI command completes;
 * treating that initial "still high" sample as completion loses the whole
 * refresh.  Callers must still wait for the release afterwards. */
static esp_err_t wait_until_busy(uint32_t timeout_ms, const char *stage)
{
    const int64_t start_us = esp_timer_get_time();
    const int initial_level = gpio_get_level(s_config.pin_busy);
    const int64_t deadline = start_us + ((int64_t)timeout_ms * 1000);
    while (gpio_get_level(s_config.pin_busy) != 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG,
                     "BUSY stage=%s initial=%d waited=%lldus timeout=%lums",
                     stage, initial_level,
                     (long long)(esp_timer_get_time() - start_us),
                     (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(1);
        if (s_busy_wait_hook != NULL) {
            s_busy_wait_hook(s_busy_wait_hook_context);
        }
    }
    ESP_LOGI(TAG, "BUSY stage=%s initial=%d waited=%lldus",
             stage, initial_level,
             (long long)(esp_timer_get_time() - start_us));
    return ESP_OK;
}

void epd_uc8279c_set_busy_wait_hook(epd_uc8279c_busy_wait_hook_t hook,
                                    void *context)
{
    s_busy_wait_hook = hook;
    s_busy_wait_hook_context = context;
}

static esp_err_t hardware_reset(void)
{
    s_reader_wake_black_handoff.pending = false;
    s_controller_sleeping = false;
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_reset, 0), TAG, "reset low");
    delay_ms(10);
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_reset, 1), TAG, "reset high");
    delay_ms(100);
    delay_ms(1000);
    return wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "hardware reset");
}

static esp_err_t send_lut_scaled(bool fast, bool half_duration)
{
    /* Keep each waveform paired with the CDI used by its source. The exact
     * Eego 1.2.8 DU uses 0xd7; the A4 42-byte OLD/NEW experiments use 0xd5. */
    const uint8_t *lut;
    size_t lut_len;
    size_t lut_phase_bytes = UC8279C_MONO_LUT_PHASE_BYTES;
    const char *lut_name;
    uint8_t vcom_and_data_interval;
    bool remap_xiaodouzi_polarity = false;
    if (fast) {
        vcom_and_data_interval = UC8279C_CDI_DU_NEW_OLD;
        switch (s_live_profile.du_lut) {
        case EPD_UC8279C_DU_LUT_FACTORY_49:
            lut = uc8279c_lut_du;
            lut_len = uc8279c_lut_du_len;
            lut_name = "DU-factory-previous-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            break;
        case EPD_UC8279C_DU_LUT_XIAODOUZI_FAST_49:
            lut = uc8279c_lut_du_fast;
            lut_len = uc8279c_lut_du_fast_len;
            lut_name = "DU-fast-xiaodouzi-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            break;
        case EPD_UC8279C_DU_LUT_XIAODOUZI_REFERENCE_49:
            lut = uc8279c_lut_du_xiaodouzi;
            lut_len = uc8279c_lut_du_xiaodouzi_len;
            lut_name = "DU-xiaodouzi-reference-49byte-A4-polarity";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            remap_xiaodouzi_polarity = true;
            break;
        case EPD_UC8279C_DU_LUT_A4_FACTORY_42:
            lut = uc8279c_lut_du_a4_factory;
            lut_len = uc8279c_lut_du_a4_factory_len;
            lut_name = "DU-factory-a4-42byte";
            break;
        case EPD_UC8279C_DU_LUT_A4_NO_KK_42:
            lut = du_a4_no_kk_lut();
            lut_len = sizeof(s_du_a4_no_kk_lut);
            lut_name = "DU-factory-a4-42byte-no-kk";
            break;
        case EPD_UC8279C_DU_LUT_A4_BLACK_BOOST_42:
            lut = du_black_boost_lut();
            lut_len = sizeof(s_du_black_boost_lut);
            lut_name = "DU-black-boost-A4-42byte";
            break;
        case EPD_UC8279C_DU_LUT_FACTORY_BLACK_DURATION_49:
            lut = du_black_duration_lut();
            lut_len = sizeof(s_du_black_duration_lut);
            lut_name = "DU-black-duration-boost-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            break;
        case EPD_UC8279C_DU_LUT_FACTORY_BLACK_REPEAT_49:
            lut = du_black_black_repeat_lut();
            lut_len = sizeof(s_du_black_black_repeat_lut);
            lut_name = "DU-black-black-repeat-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
        vcom_and_data_interval = s_live_profile.du_cdi;
    } else {
        switch (s_live_profile.gc_lut) {
        case EPD_UC8279C_GC_LUT_FACTORY_49:
            lut = uc8279c_lut_gc;
            lut_len = uc8279c_lut_gc_len;
            lut_name = "GC-factory-previous-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            break;
        case EPD_UC8279C_GC_LUT_CUSTOM_V1_49:
            lut = uc8279c_lut_gc_custom_v1;
            lut_len = uc8279c_lut_gc_custom_v1_len;
            lut_name = "GC-custom-v1-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            break;
        case EPD_UC8279C_GC_LUT_XIAODOUZI_49:
            lut = uc8279c_lut_gc_xiaodouzi;
            lut_len = uc8279c_lut_gc_xiaodouzi_len;
            lut_name = "GC-xiaodouzi-A4-polarity-49byte";
            lut_phase_bytes = UC8279C_LUT_PHASE_BYTES;
            remap_xiaodouzi_polarity = true;
            break;
        case EPD_UC8279C_GC_LUT_A4_FACTORY_42:
            lut = uc8279c_lut_gc_a4_factory;
            lut_len = uc8279c_lut_gc_a4_factory_len;
            lut_name = "GC-factory-a4-42byte";
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
        vcom_and_data_interval = s_live_profile.gc_cdi;
    }
    if (lut_len != (lut_phase_bytes * UC8279C_LUT_PHASE_COUNT)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* A normal mono waveform replaces the reader selector waveform. */
    s_reader_gray3_lut_loaded = false;
    s_reader_gray3_lut_white_clear_boosted = false;
    s_reader_gray3_lut_fingerprint = 0;
    ESP_RETURN_ON_ERROR(send_command_data(0x50, &vcom_and_data_interval, 1), TAG, "LUT mode");

    /*
     * Xiaodouzi and this A4 assign opposite pixel polarity to the four
     * transition banks. Preserve its VCOM bank (0x20), but map both the
     * Xiaodouzi GC profile and the exact DU reference profile as follows:
     *   0x21 WW <- source 0x24 KK
     *   0x22 KW <- source 0x23 WK
     *   0x23 WK <- source 0x22 KW
     *   0x24 KK <- source 0x21 WW
     * This changes only bank selection, not voltage levels or durations.
     */
    static const uint8_t xiaodouzi_a4_bank_map[UC8279C_LUT_PHASE_COUNT] = {
        0, 4, 3, 2, 1,
    };
    for (size_t phase = 0; phase < UC8279C_LUT_PHASE_COUNT; ++phase) {
        const size_t source_phase =
            remap_xiaodouzi_polarity
                ? xiaodouzi_a4_bank_map[phase]
                : phase;
        uint8_t phase_data[UC8279C_LUT_PHASE_BYTES];
        memcpy(phase_data,
               lut + (source_phase * lut_phase_bytes),
               lut_phase_bytes);
        if (half_duration) {
            /* Seven-byte states: keep drive bits and repeat counts intact. */
            for (size_t state = 0; state + 6 < lut_phase_bytes; state += 7) {
                for (size_t slot = 1; slot <= 4; ++slot) {
                    const uint8_t value = phase_data[state + slot];
                    const uint8_t frames = value & 0x3f;
                    phase_data[state + slot] = (value & 0xc0) | ((frames + 1) / 2);
                }
            }
        }
        ESP_RETURN_ON_ERROR(
            send_command_data((uint8_t)(0x20 + phase),
                              phase_data,
                              lut_phase_bytes),
            TAG,
            "LUT phase %u",
            (unsigned)phase);
    }
    ESP_LOGI(TAG,
             "profile gen=%lu loaded %s LUT (0x50=%02x phase=%u bank-bytes=%u total=%u)",
             (unsigned long)s_live_profile_generation, lut_name,
             vcom_and_data_interval, (unsigned)UC8279C_LUT_PHASE_COUNT,
             (unsigned)lut_phase_bytes, (unsigned)lut_len);
    if (fast && UC8279C_USE_DU_BLACK_BOOST) {
        ESP_LOGI(TAG, "DU black boost: LUTC/LUTWK/LUTKK slot0 repeat=3");
    }
    if (fast && UC8279C_USE_DU_BLACK_DURATION_BOOST) {
        ESP_LOGI(TAG,
                 "DU black duration boost: LUTWK frame0 0x64->0x84");
    }
    if (fast && UC8279C_USE_DU_BLACK_BLACK_REPEAT) {
        ESP_LOGI(TAG,
                 "DU black-black boost: LUTKK state0 repeat 0x01->0x%02x",
                 UC8279C_DU_BLACK_BLACK_REPEAT_COUNT);
    }
    s_panel_session_mode = PANEL_SESSION_MONO;
    return ESP_OK;
}

static esp_err_t send_lut(bool fast)
{
    return send_lut_scaled(fast, false);
}

static bool custom_lut_shape_is_valid(const uint8_t *lut, size_t lut_len,
                                      size_t bank_bytes)
{
    return lut != NULL &&
           (bank_bytes == UC8279C_MONO_LUT_PHASE_BYTES ||
            bank_bytes == UC8279C_LUT_PHASE_BYTES) &&
           lut_len == bank_bytes * UC8279C_LUT_PHASE_COUNT;
}

static bool custom_lut_config_is_valid(
    const epd_uc8279c_gate_probe_config_t *config)
{
    if (config == NULL) {
        return false;
    }
    if (config->lut == NULL) {
        return config->lut_size == 0 && config->lut_bank_size == 0;
    }
    return custom_lut_shape_is_valid(config->lut, config->lut_size,
                                     config->lut_bank_size);
}

/* Send a caller-owned waveform in the controller's already-mapped bank
 * order: 0x20=VCOM, 0x21=WW, 0x22=WB, 0x23=BW, 0x24=BB.  Unlike send_lut(),
 * this helper does not infer CDI or swap polarity; an experiment must pass
 * the exact bank order and 0x50 value it has validated. */
static esp_err_t send_custom_lut(const uint8_t *lut, size_t lut_len,
                                 size_t bank_bytes, uint8_t cdi)
{
    if (!custom_lut_shape_is_valid(lut, lut_len, bank_bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(send_command_data(0x50, &cdi, 1), TAG,
                        "custom LUT mode");
    for (size_t bank = 0; bank < UC8279C_LUT_PHASE_COUNT; ++bank) {
        /* The caller may keep the LUT in PSRAM. Copy each short bank into an
         * internal stack buffer before handing it to the SPI/DMA driver. */
        uint8_t bank_data[UC8279C_LUT_PHASE_BYTES];
        memcpy(bank_data, lut + bank * bank_bytes, bank_bytes);
        ESP_RETURN_ON_ERROR(
            send_command_data((uint8_t)(0x20 + bank),
                              bank_data, bank_bytes),
            TAG, "custom LUT bank %u", (unsigned)bank);
    }
    ESP_LOGI(TAG, "loaded caller LUT: banks=%u bytes=%u cdi=0x%02x",
             (unsigned)UC8279C_LUT_PHASE_COUNT, (unsigned)bank_bytes, cdi);
    return ESP_OK;
}

static esp_err_t send_reader_white_clear_lut(void)
{
    static const uint8_t bank_map[UC8279C_LUT_PHASE_COUNT] = {0, 4, 3, 2, 1};
    const uint8_t cdi = 0x00;

    if (uc8279c_lut_gray4_reader_white_clear_len !=
        UC8279C_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(send_command_data_bytewise(0x50, &cdi, 1), TAG,
                        "reader white-clear LUT mode");
    for (size_t bank = 0; bank < UC8279C_LUT_PHASE_COUNT; ++bank) {
        ESP_RETURN_ON_ERROR(
            send_command_data(
                (uint8_t)(0x20 + bank),
                uc8279c_lut_gray4_reader_white_clear +
                    ((size_t)bank_map[bank] * UC8279C_LUT_PHASE_BYTES),
                UC8279C_LUT_PHASE_BYTES),
            TAG, "reader white-clear LUT bank %u", (unsigned)bank);
    }
    ESP_LOGI(TAG, "loaded four-gray reader-white-clear LUT (0x50=00)");
    return ESP_OK;
}

static uint32_t waveform_fingerprint(const uint8_t *data, size_t size)
{
    uint32_t fingerprint = UINT32_C(2166136261);
    for (size_t index = 0; index < size; ++index) {
        fingerprint ^= data[index];
        fingerprint *= UINT32_C(16777619);
    }
    return fingerprint;
}

/* Build the reader's transition table from the already recovered A4 table.
 * The recovered table puts a white-clear prefix in every target bank.  That
 * is correct for a baseline gray paint, but it is precisely why an unchanged
 * white background flashes on every page turn.  Keep the proven target
 * trajectories and replace the bank selected by 00 with a zero-frame idle
 * bank.  The A4 polarity map is intentional:
 *
 *   hardware 0x21 (00) <- an explicit idle bank
 *   hardware 0x22 (10) <- source bank 4: black (14 black frames)
 *   hardware 0x23 (01) <- source bank 2: middle gray (5 black frames)
 *   hardware 0x24 (11) <- source bank 1: white clear
 *
 * This is a waveform-selection change only; the optical result still needs
 * validation on the physical panel.  Do not alter the historical table in
 * place because it remains the rollback/baseline diagnostic path. */
static const uint8_t *reader_gray3_transition_lut(void)
{
    static uint8_t lut[UC8279C_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT];
    static bool ready;
    if (!ready) {
        const uint8_t *const source = uc8279c_lut_gray4_reader_white_clear;
        memcpy(lut, source, sizeof(lut));
        /* Source bank 1: white-clear trajectory. */
        memcpy(lut + (1U * UC8279C_LUT_PHASE_BYTES),
               source + (1U * UC8279C_LUT_PHASE_BYTES),
               UC8279C_LUT_PHASE_BYTES);
        /* Source bank 2 is the known middle-gray dose. With CDI/DDX=00 the
         * selector value 01 is the controller's 0x23 (WK) bank. */
        memcpy(lut + (2U * UC8279C_LUT_PHASE_BYTES),
               source + (2U * UC8279C_LUT_PHASE_BYTES),
               UC8279C_LUT_PHASE_BYTES);
        /* Source bank 4 is the strongest black trajectory. With CDI/DDX=00
         * the selector value 10 is the controller's 0x22 (KW) bank. */
        memcpy(lut + (3U * UC8279C_LUT_PHASE_BYTES),
               source + (4U * UC8279C_LUT_PHASE_BYTES),
               UC8279C_LUT_PHASE_BYTES);
        /* Legacy selector 00 is an empty source bank. This does NOT prove
         * electrical neutrality while VCOM and the other banks are active.
         * Keep it unchanged as the control for retention experiments. */
        memset(lut + (4U * UC8279C_LUT_PHASE_BYTES), 0,
               UC8279C_LUT_PHASE_BYTES);
        ready = true;
    }
    return lut;
}

/* Keep the recovered table unchanged for rollback.  The selective variants
 * alter only their own target banks: old->white gets a longer white-clear
 * slot; a new black pixel gets a longer black-drive slot. */
static const uint8_t *reader_gray3_transition_lut_boosted(
    bool boost_white_clear, bool boost_black)
{
    static uint8_t lut[4][UC8279C_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT];
    static bool ready[4];
    static uint8_t cached_black_frames[4];
    const size_t variant = (boost_white_clear ? 1U : 0U) |
                           (boost_black ? 2U : 0U);
    if (!ready[variant] ||
        cached_black_frames[variant] != s_reader_gray3_black_drive_frames) {
        memcpy(lut[variant], reader_gray3_transition_lut(),
               sizeof(lut[variant]));
        if (boost_white_clear) {
            const size_t count_offset =
            (size_t)UC8279C_READER_GRAY3_WHITE_CLEAR_BANK *
                UC8279C_LUT_PHASE_BYTES +
            UC8279C_READER_GRAY3_WHITE_CLEAR_COUNT_OFFSET;
            const uint8_t baseline = lut[variant][count_offset];
            lut[variant][count_offset] = (uint8_t)(
            (baseline & 0xc0U) |
            UC8279C_READER_GRAY3_WHITE_CLEAR_SECOND_SLOT_FRAMES);
        }
        if (boost_black) {
            const size_t count_offset =
                (size_t)UC8279C_READER_GRAY3_BLACK_BANK *
                    UC8279C_LUT_PHASE_BYTES +
                UC8279C_READER_GRAY3_BLACK_COUNT_OFFSET;
            const uint8_t baseline = lut[variant][count_offset];
            lut[variant][count_offset] = (uint8_t)(
                (baseline & 0xc0U) |
                s_reader_gray3_black_drive_frames);
        }
        cached_black_frames[variant] = s_reader_gray3_black_drive_frames;
        ready[variant] = true;
    }
    return lut[variant];
}

/* 仅处理本机恢复表的两个已知 7 字节记录，不是通用 LUT 解码器。
 * 实验假设：把第二记录的黑向段移到起点，原白向段移后并改为代码 00。
 * 黑向帧数和两个记录的总帧数保留；00 不承诺零电场或浮空。
 * 只改调用方选中的目标 bank；灰分支取消白前缀后 B2G 可能无法变浅，
 * 因此灰前移仅用于独立诊断。真实重叠时序必须用实机测量确认。 */
static esp_err_t gray3_advance_target_bank(uint8_t *bank)
{
    const uint8_t prefix[7] = {1, 0x82, 0x90, 0, 0, 1, 1};
    uint8_t black_record[7] = {1, 0, 0, 0, 0, 1, 1};
    black_record[1] = bank[8];
    if (memcmp(bank, prefix, sizeof(prefix)) != 0 ||
        memcmp(bank + 7, black_record, sizeof(black_record)) != 0 ||
        (bank[8] & 0xc0U) != 0x40U || (bank[8] & 0x3fU) == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 14; i < UC8279C_LUT_PHASE_BYTES; ++i) {
        if (bank[i] != 0U) return ESP_ERR_INVALID_ARG;
    }
    memcpy(bank, black_record, sizeof(black_record));
    memcpy(bank + 7, prefix, sizeof(prefix));
    bank[8] &= 0x3fU;
    bank[9] &= 0x3fU;
    return ESP_OK;
}

static esp_err_t send_reader_gray3_transition_lut(bool boost_white_clear,
                                                  bool boost_black,
                                                  unsigned retention_mode,
                                                  bool parallel_black,
                                                  gray3_timing_t gray_timing)
{
    const bool parallel_gray = gray_timing == GRAY3_TIMING_ADVANCED;
    const bool gray_bank_control = gray_timing == GRAY3_TIMING_BLACK_CONTROL;
    const bool gray_aligned = gray_timing == GRAY3_TIMING_ALIGNED;
    const bool gray_return = gray_timing == GRAY3_TIMING_RETURN;
    const bool timed_hold = gray_timing == GRAY3_TIMING_REVERSE_TIMED_HOLD;
    const bool recovery_aligned = gray_timing == GRAY3_TIMING_RECOVERY_ALIGNED;
    const bool gray_reverse = gray_timing == GRAY3_TIMING_REVERSE || timed_hold;
    static const uint8_t bank_map[UC8279C_LUT_PHASE_COUNT] = {0, 4, 3, 2, 1};
    const uint8_t cdi = 0x00;
    const uint8_t *const baseline = (boost_white_clear || boost_black)
                                   ? reader_gray3_transition_lut_boosted(
                                         boost_white_clear, boost_black)
                                   : reader_gray3_transition_lut();
    uint8_t candidate[UC8279C_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT];
    memcpy(candidate, baseline, sizeof(candidate));
    uint8_t *const black = candidate + 3U * UC8279C_LUT_PHASE_BYTES;
    uint8_t *const gray = candidate + 2U * UC8279C_LUT_PHASE_BYTES;
    uint8_t *const hold = candidate + 4U * UC8279C_LUT_PHASE_BYTES;
    if (retention_mode > UC8279C_GRAY3_RETENTION_TARGET_BLACK ||
        (parallel_black && retention_mode != UC8279C_GRAY3_RETENTION_LEGACY) ||
        (gray_timing != GRAY3_TIMING_BASELINE && !parallel_black &&
         !recovery_aligned) ||
        (recovery_aligned && (parallel_black ||
         retention_mode != UC8279C_GRAY3_RETENTION_LEGACY)) ||
        (unsigned)gray_timing > GRAY3_TIMING_RECOVERY_ALIGNED) {
        return ESP_ERR_INVALID_ARG;
    }
    if (retention_mode != UC8279C_GRAY3_RETENTION_LEGACY &&
        (black[1] != 0x82U || black[2] != 0x90U ||
         (black[8] & 0xc0U) != 0x40U)) {
        /* These are narrow edits to the recovered native table, not a
         * generic 49-byte LUT decoder. Reject an unrecognized recipe. */
        return ESP_ERR_INVALID_ARG;
    }
    if (retention_mode == UC8279C_GRAY3_RETENTION_TIMED_ZERO) {
        /* Preserve this native SOURCE bank's state/timing framing, but
         * replace its three known active pulse level fields with code 00.
         * Do not copy VCOM level encodings into a source bank. Actual panel
         * neutrality and end-of-state behavior require hardware tests. */
        memcpy(hold, black, UC8279C_LUT_PHASE_BYTES);
        hold[1] &= 0x3fU;
        hold[2] &= 0x3fU;
        hold[8] &= 0x3fU;
    } else if (retention_mode == UC8279C_GRAY3_RETENTION_TARGET_BLACK) {
        /* Native recovered source prefix: 0x82,0x90 -> 0x02,0x10.
         * Keep its timing/framing and final black-drive byte intact. This
         * removes the KNOWN white level codes, not every possible transient
         * field. Do not shorten the prefix or alter VCOM in the same test. */
        black[1] &= 0x3fU;
        black[2] &= 0x3fU;
    }
    if (parallel_black) {
        ESP_RETURN_ON_ERROR(gray3_advance_target_bank(black), TAG,
                            "parallel black recipe mismatch");
    }
    if (timed_hold) {
        /* 空 bank 是否结束了源极驱动是待验证假设。仅补齐与当前黑 bank
         * 相同的记录/计数，已知电平字段全部改为代码 00；VCOM 不变。 */
        memcpy(hold, black, UC8279C_LUT_PHASE_BYTES);
        hold[1] &= 0x3fU;
        hold[8] &= 0x3fU;
        hold[9] &= 0x3fU;
    }
    if (parallel_gray) {
        ESP_RETURN_ON_ERROR(gray3_advance_target_bank(gray), TAG,
                            "parallel gray recipe mismatch");
    }
    if (gray_bank_control) {
        /* 完整复制已验证黑分支，不依赖对七字节时序字段的推断。 */
        memcpy(gray, black, UC8279C_LUT_PHASE_BYTES);
    }
    if (gray_reverse) {
        /* 默认计数 2 相对 reason=6 只改第二记录首槽的电平位。
         * 调剂量时借用后续代码 00 的计数，第二记录合计仍为 18。 */
        memcpy(gray, black, UC8279C_LUT_PHASE_BYTES);
        gray[8] = (uint8_t)(0x80U | s_gray3_reverse_white_frames);
        gray[9] = (uint8_t)(18U - s_gray3_reverse_white_frames);
    }
    if (gray_aligned) {
        ESP_RETURN_ON_ERROR(gray3_advance_target_bank(gray), TAG,
                            "aligned gray recipe mismatch");
        const uint8_t gray_pulse = gray[1];
        const unsigned gray_frames = gray_pulse & 0x3fU;
        const unsigned black_frames = black[1] & 0x3fU;
        if (black_frames < gray_frames) return ESP_ERR_INVALID_ARG;
        /* 单变量对照：从用户观察同期变黑的完整黑 bank 出发，把首槽
         * 黑向计数减为灰档，并在同记录下一槽补足代码 00 的计数。
         * 不再缩短整个记录；00 的光学保持效果仍需独立确认。 */
        memcpy(gray, black, UC8279C_LUT_PHASE_BYTES);
        gray[1] = gray_pulse;
        gray[2] = (uint8_t)(black_frames - gray_frames);
    }
    if (recovery_aligned) {
        const uint8_t gray_pulse = gray[8];
        const unsigned gray_frames = gray_pulse & 0x3fU;
        const unsigned black_frames = black[8] & 0x3fU;
        if (memcmp(gray, black, 7U) != 0 ||
            (gray_pulse & 0xc0U) != 0x40U ||
            (black[8] & 0xc0U) != 0x40U || gray_frames == 0U ||
            black_frames < gray_frames) {
            return ESP_ERR_INVALID_ARG;
        }
        /* 首帧仍需从未知状态恢复，不能直接借用差分波形或虚构白历史。
         * 保留原 18 帧白清；只在灰 bank 第二记录内补代码 00，使 5 帧
         * 灰驱动与 18 帧黑驱动等时结束。代码 00 的墨色仍需实测。 */
        memcpy(gray, black, UC8279C_LUT_PHASE_BYTES);
        gray[8] = gray_pulse;
        gray[9] = (uint8_t)(black_frames - gray_frames);
    }
    if (gray_return) {
        ESP_RETURN_ON_ERROR(gray3_advance_target_bank(gray), TAG,
                            "return gray recipe mismatch");
        const uint8_t gray_pulse = gray[1];
        const unsigned gray_frames = gray_pulse & 0x3fU;
        const unsigned black_frames = black[1] & 0x3fU;
        if (black_frames < gray_frames) return ESP_ERR_INVALID_ARG;
        /* W2G keeps the short-gray candidate. B2G uses a separate source
         * bank: a short white reverse pulse followed by the same gray pulse.
         * This is a deliberately narrow optical hypothesis, not a generic
         * LUT decoder or a reader waveform. */
        memcpy(gray, black, UC8279C_LUT_PHASE_BYTES);
        gray[1] = gray_pulse;
        gray[2] = (uint8_t)(black_frames - gray_frames);
        uint8_t *const return_gray =
            candidate + 1U * UC8279C_LUT_PHASE_BYTES;
        memset(return_gray, 0, UC8279C_LUT_PHASE_BYTES);
        return_gray[0] = 1U;
        return_gray[1] = 0x82U;
        return_gray[5] = 1U;
        return_gray[6] = 1U;
        return_gray[7] = 1U;
        return_gray[8] = gray_pulse;
        return_gray[12] = 1U;
        return_gray[13] = 1U;
    }
    const uint8_t *const lut = candidate;
    const uint32_t fingerprint =
        waveform_fingerprint(lut, UC8279C_LUT_PHASE_BYTES *
                                      UC8279C_LUT_PHASE_COUNT);

    if (s_reader_gray3_lut_loaded &&
        s_panel_session_mode == PANEL_SESSION_GRAY3 &&
        s_reader_gray3_lut_fingerprint == fingerprint &&
        s_reader_gray3_lut_profile_generation == s_live_profile_generation &&
        s_reader_gray3_lut_session_generation == s_panel_session_generation) {
        return ESP_OK;
    }

    /* Commit the cache only after every bank is written successfully. */
    s_reader_gray3_lut_loaded = false;
    ESP_RETURN_ON_ERROR(send_command_data_bytewise(0x50, &cdi, 1), TAG,
                        "reader transition LUT mode");
    for (size_t bank = 0; bank < UC8279C_LUT_PHASE_COUNT; ++bank) {
        ESP_RETURN_ON_ERROR(
            send_command_data(
                (uint8_t)(0x20 + bank),
                lut + ((size_t)bank_map[bank] * UC8279C_LUT_PHASE_BYTES),
                UC8279C_LUT_PHASE_BYTES),
            TAG, "reader transition LUT bank %u", (unsigned)bank);
    }
    ESP_LOGI(TAG,
             "loaded reader gray3 transition LUT: 00=hold 11=white-clear "
             "10=black 01=gray white-clear=%s black=%s "
             "black_frames=%u retention=%u fingerprint=%08lx profile_gen=%lu "
             "session_gen=%lu parallel_probe=%d parallel_gray=%d "
             "gray_bank_control=%d gray_aligned=%d gray_return=%d "
             "gray_reverse=%d reverse_white_count=%u timed_hold=%d "
             "recovery_aligned=%d (0x50=00)",
             boost_white_clear ? "24f-local" : "18f-baseline",
             boost_black ? "configured-drive" : "14f-baseline",
             (unsigned)s_reader_gray3_black_drive_frames,
             retention_mode,
             (unsigned long)fingerprint,
             (unsigned long)s_live_profile_generation,
             (unsigned long)s_panel_session_generation, parallel_black,
             parallel_gray, gray_bank_control, gray_aligned, gray_return,
             gray_reverse, gray_reverse ? s_gray3_reverse_white_frames : 0U,
             timed_hold, recovery_aligned);
    s_reader_gray3_lut_loaded = true;
    s_reader_gray3_lut_white_clear_boosted = boost_white_clear;
    s_reader_gray3_lut_black_boosted = boost_black;
    s_reader_gray3_lut_fingerprint = fingerprint;
    s_reader_gray3_lut_profile_generation = s_live_profile_generation;
    s_reader_gray3_lut_session_generation = s_panel_session_generation;
    s_panel_session_mode = PANEL_SESSION_GRAY3;
    return ESP_OK;
}

static esp_err_t send_gray4_lut(epd_uc8279c_gray4_mode_t mode)
{
    if (mode == EPD_UC8279C_GRAY4_READER_WHITE_CLEAR) {
        return send_reader_white_clear_lut();
    }
#if UC8279C_GRAY4_A_CDI_VARIANT == 0
    static const uint8_t gray4_vcom_a_factory[] = {0xa9, 0x07};
#elif UC8279C_GRAY4_A_CDI_VARIANT == 1
    static const uint8_t gray4_vcom_a_no_border_hiz[] = {0x29, 0x07};
#endif
    static const uint8_t gray4_vcom_c[] = {0x29, 0x07};
    static const uint8_t gray4_cdi_waveshare_4in2[] = {0x97};
    const uint8_t *lut;
    size_t lut_len;
    const uint8_t *vcom_and_data_interval = NULL;
    size_t vcom_and_data_interval_len = 0;
    size_t lut_phase_bytes = UC8279C_GRAY4_LUT_PHASE_BYTES;
    const char *mode_name;
    switch (mode) {
    case EPD_UC8279C_GRAY4_FACTORY_A:
        lut = uc8279c_lut_gray4_factory_a;
        lut_len = uc8279c_lut_gray4_factory_a_len;
#if UC8279C_GRAY4_A_CDI_VARIANT == 0
        vcom_and_data_interval = gray4_vcom_a_factory;
        vcom_and_data_interval_len = sizeof(gray4_vcom_a_factory);
#elif UC8279C_GRAY4_A_CDI_VARIANT == 1
        vcom_and_data_interval = gray4_vcom_a_no_border_hiz;
        vcom_and_data_interval_len = sizeof(gray4_vcom_a_no_border_hiz);
#elif UC8279C_GRAY4_A_CDI_VARIANT == 2
        /* Intentionally leave 0x50 untouched for the A-LUT-only test. */
#else
#error "Unsupported UC8279C_GRAY4_A_CDI_VARIANT"
#endif
        mode_name = "A";
        break;
    case EPD_UC8279C_GRAY4_FACTORY_B:
    case EPD_UC8279C_GRAY4_FACTORY_B_GC2:
        lut = uc8279c_lut_gray4_factory_b;
        lut_len = uc8279c_lut_gray4_factory_b_len;
        /* Official mode 0 sends this LUT without a 0x50 command. */
        mode_name = mode == EPD_UC8279C_GRAY4_FACTORY_B_GC2 ? "B-GC2" : "B";
        break;
    case EPD_UC8279C_GRAY4_FACTORY_C:
        lut = uc8279c_lut_gray4_factory_c;
        lut_len = uc8279c_lut_gray4_factory_c_len;
        vcom_and_data_interval = gray4_vcom_c;
        vcom_and_data_interval_len = sizeof(gray4_vcom_c);
        mode_name = "C";
        break;
    case EPD_UC8279C_GRAY4_WAVESHARE_4IN2:
        lut = uc8279c_lut_gray4_waveshare_4in2;
        lut_len = uc8279c_lut_gray4_waveshare_4in2_len;
        vcom_and_data_interval = gray4_cdi_waveshare_4in2;
        vcom_and_data_interval_len = sizeof(gray4_cdi_waveshare_4in2);
        mode_name = "Waveshare-4in2-open";
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (lut_len != (lut_phase_bytes * UC8279C_LUT_PHASE_COUNT)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t phase = 0; phase < UC8279C_LUT_PHASE_COUNT; ++phase) {
        ESP_RETURN_ON_ERROR(
            send_command_data(
                (uint8_t)(0x20 + phase),
                lut + (phase * lut_phase_bytes),
                lut_phase_bytes),
            TAG,
            "four-gray LUT phase %u",
            (unsigned)phase);
    }
    if (mode == EPD_UC8279C_GRAY4_WAVESHARE_4IN2) {
        /* Waveshare's fixed reference also copies LUTWW (0x21) into the
         * controller's border LUT (0x25). */
        ESP_RETURN_ON_ERROR(
            send_command_data(0x25,
                              lut + UC8279C_GRAY4_LUT_PHASE_BYTES,
                              UC8279C_GRAY4_LUT_PHASE_BYTES),
            TAG, "four-gray border LUT");
    }
    if (vcom_and_data_interval == NULL) {
        ESP_LOGI(TAG, "loaded four-gray %s LUT (no 0x50)", mode_name);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        send_command_data_bytewise(0x50, vcom_and_data_interval,
                                   vcom_and_data_interval_len),
        TAG, "four-gray LUT mode");
    if (vcom_and_data_interval_len == 1) {
        ESP_LOGI(TAG, "loaded four-gray %s LUT (0x50=%02x)", mode_name,
                 vcom_and_data_interval[0]);
    } else {
        ESP_LOGI(TAG, "loaded four-gray %s LUT (0x50=%02x %02x)", mode_name,
                 vcom_and_data_interval[0], vcom_and_data_interval[1]);
    }
    return ESP_OK;
}

/* The A4 49-byte gray table has eight idle bytes before each bank's first
 * active slot. It is kept as a dedicated table so the direct FreeInk A4 port
 * cannot accidentally inherit the 42-byte Factory-B framing. */
static esp_err_t send_a4_gray49_lut(void)
{
    if (uc8279c_lut_gray4_a4_len !=
        UC8279C_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t phase = 0; phase < UC8279C_LUT_PHASE_COUNT; ++phase) {
        ESP_RETURN_ON_ERROR(
            send_command_data(
                (uint8_t)(0x20 + phase),
                uc8279c_lut_gray4_a4 + phase * UC8279C_LUT_PHASE_BYTES,
                UC8279C_LUT_PHASE_BYTES),
            TAG, "A4 gray49 LUT bank %u", (unsigned)phase);
    }
    ESP_LOGI(TAG, "loaded A4 gray LUT: 5x49, no 0x50 override");
    return ESP_OK;
}

/* Factory-B writes 42 bytes to each LUT command. UC8179C defines that as
 * seven six-byte slots: level selection, four full-byte frame counts, then
 * repeat count. The 49-byte Xiaodouzi table is a different format and must
 * not be used to reinterpret this panel's 42-byte factory banks. */
static uint8_t scale_factory_b_frame_count(uint8_t value, uint8_t divisor)
{
    if (value == 0) {
        return 0;
    }
    return (uint8_t)((value + divisor - 1U) / divisor);
}

static void copy_scaled_factory_b_bank(uint8_t *destination,
                                       const uint8_t *source,
                                       uint8_t divisor)
{
    memcpy(destination, source, UC8279C_GRAY4_LUT_PHASE_BYTES);
    for (size_t slot = 0; slot < UC8279C_GRAY4_LUT_SLOT_COUNT; ++slot) {
        const size_t offset = slot * UC8279C_GRAY4_LUT_SLOT_BYTES;
        for (size_t field = 1; field <= 4; ++field) {
            destination[offset + field] =
                scale_factory_b_frame_count(destination[offset + field], divisor);
        }
    }
}

esp_err_t epd_uc8279c_set_gray16_trim_lut(const uint8_t *lut, size_t lut_size)
{
    if (lut == NULL || lut_size != sizeof(s_gray16_custom_trim_lut)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_gray16_custom_trim_lut, lut, sizeof(s_gray16_custom_trim_lut));
    s_gray16_custom_trim_lut_enabled = true;
    ESP_LOGI(TAG, "stored custom gray16 trim LUT: %u bytes",
             (unsigned)sizeof(s_gray16_custom_trim_lut));
    return ESP_OK;
}

void epd_uc8279c_clear_gray16_trim_lut(void)
{
    s_gray16_custom_trim_lut_enabled = false;
    ESP_LOGI(TAG, "cleared custom gray16 trim LUT");
}

bool epd_uc8279c_gray16_trim_lut_is_custom(void)
{
    return s_gray16_custom_trim_lut_enabled;
}

esp_err_t epd_uc8279c_set_gray16_factory_b_duration_divisor(uint8_t divisor)
{
    if (divisor == 0 || divisor > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    s_gray16_factory_b_duration_divisor = divisor;
    return ESP_OK;
}

esp_err_t epd_uc8279c_set_gray16_factory_b_active_bank(uint8_t bank)
{
    if (bank != 2U && bank != 3U) {
        return ESP_ERR_INVALID_ARG;
    }
    s_gray16_factory_b_active_bank = bank;
    return ESP_OK;
}

uint8_t epd_uc8279c_gray16_factory_b_active_bank(void)
{
    return s_gray16_factory_b_active_bank;
}

static esp_err_t send_gray16_trim_lut(void)
{
    if (s_gray16_custom_trim_lut_enabled) {
        for (size_t phase = 0; phase < UC8279C_LUT_PHASE_COUNT; ++phase) {
            const uint8_t *bank =
                s_gray16_custom_trim_lut +
                (phase * UC8279C_GRAY4_LUT_PHASE_BYTES);
            ESP_RETURN_ON_ERROR(
                send_command_data((uint8_t)(0x20 + phase), bank,
                                  UC8279C_GRAY4_LUT_PHASE_BYTES),
                TAG, "custom gray16 trim LUT phase %u", (unsigned)phase);
        }
        ESP_LOGI(TAG, "loaded custom gray16 trim LUT from serial");
        return ESP_OK;
    }

    uint8_t trim_lut[UC8279C_GRAY4_LUT_PHASE_BYTES *
                     UC8279C_LUT_PHASE_COUNT];
    const uint8_t *factory_vcom = uc8279c_lut_gray4_factory_b;
    const uint8_t *factory_active_transition =
        uc8279c_lut_gray4_factory_b +
        ((size_t)s_gray16_factory_b_active_bank *
         UC8279C_GRAY4_LUT_PHASE_BYTES);
    const uint8_t *factory_black_hold =
        uc8279c_lut_gray4_factory_b +
        (4 * UC8279C_GRAY4_LUT_PHASE_BYTES);

    /* Each temporal phase chooses one native Factory-B transition branch for
     * active pixels and keeps 00 pixels on the matching K->K branch. VCOM is
     * global, so bank 4 must remain present rather than being a zero-length
     * no-op. */
    memset(trim_lut, 0, sizeof(trim_lut));
    copy_scaled_factory_b_bank(trim_lut, factory_vcom,
                               s_gray16_factory_b_duration_divisor);
    copy_scaled_factory_b_bank(
        trim_lut + ((size_t)s_gray16_factory_b_active_bank *
                    UC8279C_GRAY4_LUT_PHASE_BYTES),
        factory_active_transition,
        s_gray16_factory_b_duration_divisor);
    copy_scaled_factory_b_bank(
        trim_lut + (4 * UC8279C_GRAY4_LUT_PHASE_BYTES),
        factory_black_hold,
        s_gray16_factory_b_duration_divisor);
    /* Bank 1 and the inactive opposite transition bank remain off. */

    for (size_t phase = 0; phase < UC8279C_LUT_PHASE_COUNT; ++phase) {
        uint8_t *bank = trim_lut + (phase * UC8279C_GRAY4_LUT_PHASE_BYTES);
        ESP_RETURN_ON_ERROR(
            send_command_data((uint8_t)(0x20 + phase), bank,
                              UC8279C_GRAY4_LUT_PHASE_BYTES),
            TAG, "gray16 trim LUT phase %u", (unsigned)phase);
    }
    ESP_LOGI(TAG,
             "loaded gray16 Factory-B microdrive: 00=0x24 active=0x2%u, frame counts /%u",
             (unsigned)s_gray16_factory_b_active_bank,
             (unsigned)s_gray16_factory_b_duration_divisor);
    return ESP_OK;
}

static esp_err_t write_gray4_planes(const uint8_t *dtm1_msb,
                                    const uint8_t *dtm2_lsb,
                                    const char *stage)
{
    ESP_RETURN_ON_ERROR(send_command(0x10), TAG, "%s DTM1 command", stage);
    ESP_RETURN_ON_ERROR(write_visible_framebuffer_continuous_cs(dtm1_msb),
                        TAG, "%s DTM1 frame", stage);
    ESP_RETURN_ON_ERROR(send_command(0x13), TAG, "%s DTM2 command", stage);
    ESP_RETURN_ON_ERROR(write_visible_framebuffer_continuous_cs(dtm2_lsb),
                        TAG, "%s DTM2 frame", stage);
    return ESP_OK;
}

static const uint8_t s_font5x7[128][7] = {
    ['0'] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    ['1'] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    ['2'] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    ['3'] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
    ['4'] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    ['5'] = {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
    ['6'] = {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    ['7'] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    ['8'] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    ['9'] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e},
    ['A'] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
    ['B'] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
    ['C'] = {0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f},
    ['E'] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},
    ['F'] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
    ['G'] = {0x0f, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0f},
    ['H'] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
    ['I'] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f},
    ['L'] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},
    ['M'] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},
    ['N'] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    ['O'] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
    ['P'] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
    ['R'] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},
    ['T'] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    ['X'] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},
    ['Y'] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04},
};

static int text_width(const char *text, int scale)
{
    return (int)strlen(text) * 6 * scale - scale;
}

static bool text_pixel_is_black(int x,
                                int y,
                                int origin_x,
                                int origin_y,
                                const char *text,
                                int scale)
{
    if (x < origin_x || y < origin_y || y >= origin_y + (7 * scale)) {
        return false;
    }

    const int char_cell = 6 * scale;
    const int char_index = (x - origin_x) / char_cell;
    const size_t text_len = strlen(text);
    if (char_index < 0 || (size_t)char_index >= text_len) {
        return false;
    }

    const int glyph_x = ((x - origin_x) % char_cell) / scale;
    const int glyph_y = (y - origin_y) / scale;
    if (glyph_x >= 5) {
        return false;
    }

    const unsigned char character = (unsigned char)text[char_index];
    if (character >= 128) {
        return false;
    }
    return (s_font5x7[character][glyph_y] & (1U << (4 - glyph_x))) != 0;
}

static bool text_box_pixel(int x,
                           int y,
                           int origin_x,
                           int origin_y,
                           const char *text,
                           int scale,
                           bool *inside_box)
{
    const int width = text_width(text, scale);
    *inside_box = x >= origin_x - 3 && x < origin_x + width + 3 &&
                  y >= origin_y - 3 && y < origin_y + (7 * scale) + 3;
    return *inside_box && text_pixel_is_black(x, y, origin_x, origin_y, text, scale);
}

static bool rounded_outline_pixel(int x, int y, int inset, int radius, int thickness)
{
    const int left = inset;
    const int right = UC8279C_WIDTH - 1 - inset;
    const int top = inset;
    const int bottom = UC8279C_VISIBLE_HEIGHT - 1 - inset;
    if (x < left || x > right || y < top || y > bottom) {
        return false;
    }

    const int inner_left = left + radius;
    const int inner_right = right - radius;
    const int inner_top = top + radius;
    const int inner_bottom = bottom - radius;

    if ((x >= inner_left && x <= inner_right) &&
        (y < top + thickness || y > bottom - thickness)) {
        return true;
    }
    if ((y >= inner_top && y <= inner_bottom) &&
        (x < left + thickness || x > right - thickness)) {
        return true;
    }

    int center_x = -1;
    int center_y = -1;
    if (x < inner_left) {
        center_x = inner_left;
    } else if (x > inner_right) {
        center_x = inner_right;
    }
    if (y < inner_top) {
        center_y = inner_top;
    } else if (y > inner_bottom) {
        center_y = inner_bottom;
    }
    if (center_x < 0 || center_y < 0) {
        return false;
    }

    const int dx = x - center_x;
    const int dy = y - center_y;
    const int distance_squared = dx * dx + dy * dy;
    const int outer_radius = radius;
    const int inner_radius = radius - thickness;
    return distance_squared <= outer_radius * outer_radius &&
           distance_squared >= inner_radius * inner_radius;
}

static bool __attribute__((unused)) calibration_pixel_is_black(int x, int y)
{
    if (y >= UC8279C_VISIBLE_HEIGHT) {
        return false;
    }

    /* Labels are rendered on white boxes so the grid cannot obscure them. */
    struct label {
        const char *text;
        int x;
        int y;
        int scale;
    };
    const struct label labels[] = {
        {"TOP", (UC8279C_WIDTH - 51) / 2, 74, 3},
        {"BOTTOM", (UC8279C_WIDTH - 82) / 2, UC8279C_VISIBLE_HEIGHT - 98, 2},
        {"LEFT", 82, (UC8279C_VISIBLE_HEIGHT - 14) / 2, 2},
        {"RIGHT", UC8279C_WIDTH - 146, (UC8279C_VISIBLE_HEIGHT - 14) / 2, 2},
        {"TL1", 72, 72, 2},
        {"TR2", UC8279C_WIDTH - 111, 72, 2},
        {"BL4", 72, UC8279C_VISIBLE_HEIGHT - 88, 2},
        {"BR3", UC8279C_WIDTH - 111, UC8279C_VISIBLE_HEIGHT - 88, 2},
        {"R47", 108, 104, 2},
        {"BIT", 108, 190, 2},
        {"X384 Y276", (UC8279C_WIDTH - 130) / 2, (UC8279C_VISIBLE_HEIGHT / 2) + 18, 2},
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        bool inside_box = false;
        const bool black = text_box_pixel(x,
                                          y,
                                          labels[i].x,
                                          labels[i].y,
                                          labels[i].text,
                                          labels[i].scale,
                                          &inside_box);
        if (inside_box) {
            return black;
        }
    }

    /* 8-pixel lattices in the four 96x96 corners expose the rounded mask. */
    const bool in_left_corner = x < 96;
    const bool in_right_corner = x >= UC8279C_WIDTH - 96;
    const bool in_top_corner = y < 96;
    const bool in_bottom_corner = y >= UC8279C_VISIBLE_HEIGHT - 96;
    if ((in_left_corner || in_right_corner) &&
        (in_top_corner || in_bottom_corner) &&
        ((x % 8) == 0 || (y % 8) == 0)) {
        return true;
    }

    /* Parallel outlines around the estimated 5 mm / 47 pixel corner radius. */
    static const int rounded_insets[] = {0, 8, 16, 24};
    for (size_t i = 0; i < sizeof(rounded_insets) / sizeof(rounded_insets[0]); ++i) {
        const int inset = rounded_insets[i];
        if (rounded_outline_pixel(x,
                                  y,
                                  inset,
                                  UC8279C_CORNER_RADIUS_PX - inset,
                                  2)) {
            return true;
        }
    }

    /* Exact edge-distance rails. Count inward in 8-pixel steps. */
    static const int edge_offsets[] = {0, 4, 8, 16, 24, 32, 40, 48, 56, 64};
    for (size_t i = 0; i < sizeof(edge_offsets) / sizeof(edge_offsets[0]); ++i) {
        const int offset = edge_offsets[i];
        if ((y == offset || y == UC8279C_VISIBLE_HEIGHT - 1 - offset) &&
            x >= 128 && x < UC8279C_WIDTH - 128) {
            return true;
        }
        if ((x == offset || x == UC8279C_WIDTH - 1 - offset) &&
            y >= 128 && y < UC8279C_VISIBLE_HEIGHT - 128) {
            return true;
        }
    }

    /* Dashed 32-pixel grid with solid 128-pixel major axes. */
    if ((x % 32) == 0 && ((y / 8) & 1) == 0) {
        return true;
    }
    if ((y % 32) == 0 && ((x / 8) & 1) == 0) {
        return true;
    }
    if ((x % 128) <= 1 || (y % 128) <= 1) {
        return true;
    }

    /* A non-byte-aligned 1-pixel diagonal exposes an incorrect bit reversal. */
    if (y >= 214 && y < 278 && x == 101 + (y - 214)) {
        return true;
    }
    if (y >= 214 && y < 278 && x == 180 - (y - 214)) {
        return true;
    }

    /* Center crosshair and a small hollow center box reveal any row/column skew. */
    const int center_x = UC8279C_WIDTH / 2;
    const int center_y = UC8279C_VISIBLE_HEIGHT / 2;
    if ((x >= center_x - 48 && x <= center_x + 48 && y >= center_y - 1 && y <= center_y + 1) ||
        (y >= center_y - 48 && y <= center_y + 48 && x >= center_x - 1 && x <= center_x + 1)) {
        return true;
    }
    if ((x == center_x - 12 || x == center_x + 12) &&
        y >= center_y - 12 && y <= center_y + 12) {
        return true;
    }
    if ((y == center_y - 12 || y == center_y + 12) &&
        x >= center_x - 12 && x <= center_x + 12) {
        return true;
    }

    return false;
}

static bool border_test_pixel_is_black(int x, int y)
{
    if (y >= UC8279C_VISIBLE_HEIGHT) {
        return false;
    }

    return rounded_outline_pixel(x,
                                 y,
                                 0,
                                 UC8279C_CORNER_RADIUS_PX,
                                 UC8279C_BORDER_THICKNESS_PX);
}

static esp_err_t write_test_frame(void)
{
    /* The factory application sends visible rows 551..0, then 48 white padding rows. */
    for (int panel_y = UC8279C_VISIBLE_HEIGHT - 1; panel_y >= 0; --panel_y) {
        memset(s_dma_row, 0xff, sizeof(s_dma_row));

        for (int x = 0; x < UC8279C_WIDTH; ++x) {
            if (border_test_pixel_is_black(x, panel_y)) {
                s_dma_row[x >> 3] &= (uint8_t)~(1U << (7 - (x & 7)));
            }
        }

        /* Pixels above are already packed in the panel's on-wire MSB-first order.
         * The factory application's reverseBits converted its LSB-first software
         * framebuffer; applying it again here mirrors every 8-pixel byte. */

        ESP_RETURN_ON_ERROR(send_data(s_dma_row, sizeof(s_dma_row)), TAG,
                            "border row %d", panel_y);
    }

    memset(s_dma_row, 0xff, sizeof(s_dma_row));
    for (int padding_row = 0;
         padding_row < (UC8279C_RAM_HEIGHT - UC8279C_VISIBLE_HEIGHT);
         ++padding_row) {
        ESP_RETURN_ON_ERROR(send_data(s_dma_row, sizeof(s_dma_row)), TAG,
                            "padding row %d", padding_row);
    }
    return ESP_OK;
}

/* The factory mono path sends each row as a separate transaction and then
 * fills the controller's hidden 48-row tail with white. Keep that framing for
 * GC/DU; the gray path has its own continuous-CS writer below. */
static esp_err_t write_visible_framebuffer(const uint8_t *framebuffer)
{
    for (int panel_y = UC8279C_VISIBLE_HEIGHT - 1; panel_y >= 0; --panel_y) {
        memcpy(s_dma_row,
               framebuffer + ((size_t)panel_y * UC8279C_BYTES_PER_ROW),
               sizeof(s_dma_row));
        ESP_RETURN_ON_ERROR(send_data(s_dma_row, sizeof(s_dma_row)), TAG,
                            "frame row %d", panel_y);
    }

    return ESP_OK;
}

/* Hold CS across all 552 visible rows of a DTM plane. */
static esp_err_t write_visible_framebuffer_continuous_cs(const uint8_t *framebuffer)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_dc, 1), TAG,
                        "DC data level");
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG,
                        "acquire four-gray data bus");

    esp_err_t err = ESP_OK;
    for (int panel_y = UC8279C_VISIBLE_HEIGHT - 1; panel_y >= 0; --panel_y) {
        memcpy(s_dma_row,
               framebuffer + ((size_t)panel_y * UC8279C_BYTES_PER_ROW),
               sizeof(s_dma_row));
        spi_transaction_t transaction = {
            .length = UC8279C_BYTES_PER_ROW * 8,
            .tx_buffer = s_dma_row,
            /* Release CS only after the last row in the plane. */
            .flags = panel_y > 0 ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(s_spi, &transaction);
        if (err != ESP_OK) {
            break;
        }
    }

    spi_device_release_bus(s_spi);
    return err;
}

/* 阅读 BLACK_HOLD 使 B2B 与新黑共用前移的黑向驱动，固定白/灰仍保持。
 * 旧版 NORMAL 与 PARALLEL_PROBE 保留为独立对照，不随阅读入口切换。
 * Legacy / TIMED_ZERO keep every unchanged tone on selector 00. The
 * TARGET_BLACK experiment sends B2B to the same no-white-prefix SOURCE bank
 * as W2B/G2B during NORMAL updates with trusted history. Re-driving black
 * through a white-prefix waveform would flash; this experiment removes those
 * known white codes before doing so. Physical no-flash/retention still needs
 * calibration. Unknown history, recovery, and maintenance retain the baseline.
 * The compile-time experiment is immutable throughout a refresh transaction. */
static uint8_t gray3_transition_selector(
    epd_uc8279c_gray3_refresh_reason_t reason,
    bool old_valid, uint8_t old_tone, uint8_t new_tone)
{
    const bool use_history =
        (reason == EPD_UC8279C_GRAY3_REFRESH_NORMAL ||
         reason == EPD_UC8279C_GRAY3_REFRESH_PARALLEL_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_BLACK_HOLD ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_PARALLEL_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_BANK_CONTROL ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_ALIGNED_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_RETURN_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_REVERSE_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_HOLD_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_TIMED_HOLD_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC_PROBE ||
         reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC) && old_valid;
    if (reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_RETURN_PROBE && old_valid &&
        old_tone == 0U && new_tone == 1U) {
        /* Diagnostic split: selector 11 maps to source bank 0x24, allowing
         * B2G to try the short white-reverse + gray return bank while W2G
         * continues to use selector 01/source bank 0x23. */
        return UC8279C_GRAY3_SELECTOR_WHITE;
    }
    if (use_history && old_tone == new_tone) {
        if (new_tone == 1U &&
            reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_HOLD_PROBE) {
            return UC8279C_GRAY3_SELECTOR_GRAY;
        }
        if (new_tone == 0U &&
            (reason == EPD_UC8279C_GRAY3_REFRESH_BLACK_HOLD ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_PARALLEL_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_BANK_CONTROL ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_ALIGNED_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_RETURN_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_REVERSE_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_HOLD_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_TIMED_HOLD_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC_PROBE ||
             reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC ||
             UC8279C_GRAY3_RETENTION_EXPERIMENT ==
                UC8279C_GRAY3_RETENTION_TARGET_BLACK)) {
            return UC8279C_GRAY3_SELECTOR_BLACK;
        }
        return UC8279C_GRAY3_SELECTOR_IDLE;
    }
    switch (new_tone) {
    case 3U:
        return UC8279C_GRAY3_SELECTOR_WHITE;
    case 1U:
        return UC8279C_GRAY3_SELECTOR_GRAY;
    default:
        return UC8279C_GRAY3_SELECTOR_BLACK;
    }
}

typedef struct {
    uint32_t keep_white; /* selector 00 idle; name retained for log ABI */
    uint32_t clear_white;
    uint32_t drive_gray;
    uint32_t drive_black;
    uint32_t pair[3][3]; /* [old_tone][new_tone], 0=black 1=gray 2=white */
    uint32_t driven_left;
    uint32_t driven_right;
    uint32_t driven_top;
    uint32_t driven_bottom;
} gray3_transition_stats_t;

/* 并行统计一个 32 像素块的置位数，不依赖 Xtensa 的硬件 popcount。 */
static uint32_t gray3_count_bits(uint32_t bits)
{
    bits -= (bits >> 1U) & UINT32_C(0x55555555);
    bits = (bits & UINT32_C(0x33333333)) +
           ((bits >> 2U) & UINT32_C(0x33333333));
    bits = (bits + (bits >> 4U)) & UINT32_C(0x0f0f0f0f);
    bits += bits >> 8U;
    bits += bits >> 16U;
    return bits & 0x3fU;
}

static void collect_gray3_transition_stats(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    bool old_valid, epd_uc8279c_gray3_refresh_reason_t reason,
    gray3_transition_stats_t *stats)
{
    /* 块不能跨越左右半屏分界；memcpy 同时支持未对齐的调用方缓冲区。 */
    _Static_assert(UC8279C_WIDTH % 64U == 0U,
                   "gray3 statistics require 32-pixel half-row blocks");
    static const uint8_t tones[3] = {0U, 1U, 3U};
    uint8_t selectors[3][3];
    uint32_t selector_counts[4] = {0};
    for (unsigned o = 0; o < 3U; ++o)
        for (unsigned n = 0; n < 3U; ++n)
            selectors[o][n] = gray3_transition_selector(
                reason, old_valid, tones[o], tones[n]);
    memset(stats, 0, sizeof(*stats));
    for (unsigned y = 0; y < UC8279C_VISIBLE_HEIGHT; ++y) {
        for (unsigned x = 0; x < UC8279C_BYTES_PER_ROW; x += 4U) {
            const size_t offset = (size_t)y * UC8279C_BYTES_PER_ROW + x;
            uint32_t om = 0, ol = 0, nm, nl;
            if (old_valid) {
                memcpy(&om, old_msb + offset, sizeof(om));
                memcpy(&ol, old_lsb + offset, sizeof(ol));
            }
            memcpy(&nm, new_msb + offset, sizeof(nm));
            memcpy(&nl, new_lsb + offset, sizeof(nl));
            /* 00=黑，01=灰，10/11=白；未知历史按原规则归入黑类。 */
            const uint32_t old_masks[3] = {~(om | ol), ~om & ol, om};
            const uint32_t new_masks[3] = {~(nm | nl), ~nm & nl, nm};
            uint32_t driven = 0;
            for (unsigned o = 0; o < 3U; ++o) {
                if (old_masks[o] == 0U) continue;
                for (unsigned n = 0; n < 3U; ++n) {
                    const uint32_t pair_mask = old_masks[o] & new_masks[n];
                    if (pair_mask == 0U) continue;
                    const uint32_t count = gray3_count_bits(pair_mask);
                    const uint8_t selector = selectors[o][n];
                    stats->pair[o][n] += count;
                    selector_counts[selector] += count;
                    if (selector != UC8279C_GRAY3_SELECTOR_IDLE) driven += count;
                }
            }
            if (x < UC8279C_BYTES_PER_ROW / 2U) stats->driven_left += driven;
            else stats->driven_right += driven;
            if (y < UC8279C_VISIBLE_HEIGHT / 2U) stats->driven_top += driven;
            else stats->driven_bottom += driven;
        }
    }
    stats->keep_white = selector_counts[UC8279C_GRAY3_SELECTOR_IDLE];
    stats->clear_white = selector_counts[UC8279C_GRAY3_SELECTOR_WHITE];
    stats->drive_gray = selector_counts[UC8279C_GRAY3_SELECTOR_GRAY];
    stats->drive_black = selector_counts[UC8279C_GRAY3_SELECTOR_BLACK];
}

/* 将原有逐像素选择规则编译为每个目标色允许的旧色集合。
 * 三位分别代表黑/灰/白；在一整张平面中只构建一次，不改任何波形策略。 */
static void gray3_build_selector_classes(
    epd_uc8279c_gray3_refresh_reason_t reason, bool old_valid,
    uint8_t selector_mask, uint8_t classes[3])
{
    static const uint8_t tones[3] = {0U, 1U, 3U};
    for (unsigned n = 0; n < 3U; ++n) {
        classes[n] = 0U;
        for (unsigned o = 0; o < 3U; ++o) {
            if ((gray3_transition_selector(reason, old_valid, tones[o], tones[n]) &
                 selector_mask) != 0U) classes[n] |= (uint8_t)(1U << o);
        }
    }
}

static uint8_t gray3_old_class_mask(uint8_t msb, uint8_t lsb, uint8_t classes)
{
    switch (classes) {
    case 0U: return 0U;
    case 1U: return (uint8_t)~(msb | lsb);
    case 2U: return (uint8_t)(~msb & lsb);
    case 3U: return (uint8_t)~msb;
    case 4U: return msb;
    case 5U: return (uint8_t)(msb | ~lsb);
    case 6U: return (uint8_t)(msb | lsb);
    default: return 0xffU;
    }
}

static uint8_t gray3_pack_transition_byte(
    uint8_t old_msb, uint8_t old_lsb, uint8_t new_msb, uint8_t new_lsb,
    const uint8_t classes[3])
{
    const uint8_t black = (uint8_t)~(new_msb | new_lsb);
    const uint8_t gray = (uint8_t)(~new_msb & new_lsb);
    /* 沿用原有编码，10 与 11 均视作白色。 */
    return (uint8_t)(
        (black & gray3_old_class_mask(old_msb, old_lsb, classes[0])) |
        (gray & gray3_old_class_mask(old_msb, old_lsb, classes[1])) |
        (new_msb & gray3_old_class_mask(old_msb, old_lsb, classes[2])));
}

static esp_err_t write_gray3_transition_plane_continuous_cs(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    bool old_valid, epd_uc8279c_gray3_refresh_reason_t reason,
    uint8_t selector_mask, uint32_t *out_digest, int *out_failure_row)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_dc, 1), TAG,
                        "gray3 transition DC data level");
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG,
                        "acquire gray3 transition data bus");

    const int rows = s_gray3_full_ram_mode ? UC8279C_RAM_HEIGHT
                                           : UC8279C_VISIBLE_HEIGHT;
    uint8_t classes[3];
    gray3_build_selector_classes(reason, old_valid, selector_mask, classes);
    uint32_t digest = UINT32_C(2166136261);
    esp_err_t err = ESP_OK;
    for (int row = 0; row < rows; ++row) {
        const int panel_y = UC8279C_VISIBLE_HEIGHT - 1 - row;
        if (panel_y < 0) {
            /* Non-visible RAM tail: selector 00 in both DTM planes. */
            memset(s_dma_row, 0, sizeof(s_dma_row));
            for (int byte_index = 0; byte_index < UC8279C_BYTES_PER_ROW;
                 ++byte_index) {
                digest ^= s_dma_row[byte_index];
                digest *= UINT32_C(16777619);
            }
        } else {
            const size_t row_offset =
                (size_t)panel_y * UC8279C_BYTES_PER_ROW;
            for (int byte_index = 0; byte_index < UC8279C_BYTES_PER_ROW;
                 ++byte_index) {
                const size_t pixel = row_offset + (size_t)byte_index;
                const uint8_t packed = gray3_pack_transition_byte(
                    old_valid ? old_msb[pixel] : 0U,
                    old_valid ? old_lsb[pixel] : 0U,
                    new_msb[pixel], new_lsb[pixel], classes);
                s_dma_row[byte_index] = packed;
                digest ^= packed;
                digest *= UINT32_C(16777619);
            }
        }
        spi_transaction_t transaction = {
            .length = UC8279C_BYTES_PER_ROW * 8,
            .tx_buffer = s_dma_row,
            .flags = row + 1 < rows
                         ? SPI_TRANS_CS_KEEP_ACTIVE
                         : 0,
        };
        err = spi_device_polling_transmit(s_spi, &transaction);
        if (err != ESP_OK) {
            if (out_failure_row != NULL) {
                *out_failure_row = row;
            }
            break;
        }
    }

    if (out_digest != NULL) {
        *out_digest = digest;
    }
    spi_device_release_bus(s_spi);
    return err;
}

/* A4 gray writes a complete 600-row DTM plane under one CS assertion:
 * visible source rows 551..0 followed by the controller's 48 white rows. */
static esp_err_t write_a4_gray_plane_continuous_cs(const uint8_t *framebuffer)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_dc, 1), TAG,
                        "A4 gray DC data level");
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG,
                        "acquire A4 gray data bus");

    esp_err_t err = ESP_OK;
    for (int row = 0; row < UC8279C_RAM_HEIGHT; ++row) {
        const int panel_y = UC8279C_VISIBLE_HEIGHT - 1 - row;
        if (panel_y >= 0) {
            memcpy(s_dma_row,
                   framebuffer + (size_t)panel_y * UC8279C_BYTES_PER_ROW,
                   sizeof(s_dma_row));
        } else {
            memset(s_dma_row, 0xff, sizeof(s_dma_row));
        }
        spi_transaction_t transaction = {
            .length = UC8279C_BYTES_PER_ROW * 8,
            .tx_buffer = s_dma_row,
            .flags = row + 1 < UC8279C_RAM_HEIGHT ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(s_spi, &transaction);
        if (err != ESP_OK) {
            break;
        }
    }

    spi_device_release_bus(s_spi);
    return err;
}

static esp_err_t write_framebuffer(const uint8_t *framebuffer)
{
    ESP_RETURN_ON_ERROR(write_visible_framebuffer(framebuffer), TAG,
                        "visible frame");
    memset(s_dma_row, 0xff, sizeof(s_dma_row));
    for (int padding_row = 0;
         padding_row < (UC8279C_RAM_HEIGHT - UC8279C_VISIBLE_HEIGHT);
         ++padding_row) {
        ESP_RETURN_ON_ERROR(send_data(s_dma_row, sizeof(s_dma_row)), TAG,
                            "padding row %d", padding_row);
    }
    return ESP_OK;
}

typedef struct {
    const uint8_t *framebuffer;
    int x;
    int y;
    int width;
    int height;
} partial_frame_t;

static bool normalize_partial_frame(partial_frame_t *frame)
{
    if (frame == NULL || frame->framebuffer == NULL || frame->width <= 0 ||
        frame->height <= 0) {
        return false;
    }
    int x0 = frame->x;
    int y0 = frame->y;
    int x1 = frame->x + frame->width;
    int y1 = frame->y + frame->height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > UC8279C_WIDTH) x1 = UC8279C_WIDTH;
    if (y1 > UC8279C_VISIBLE_HEIGHT) y1 = UC8279C_VISIBLE_HEIGHT;
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }
    x0 &= ~7;
    x1 = (x1 + 7) & ~7;
    if (x1 > UC8279C_WIDTH) x1 = UC8279C_WIDTH;
    frame->x = x0;
    frame->y = y0;
    frame->width = x1 - x0;
    frame->height = y1 - y0;
    return true;
}

static esp_err_t enter_partial_window(const partial_frame_t *frame)
{
    /* Full-frame transfers feed software row 551 first, so controller RAM
     * row zero corresponds to the bottom visible software row. */
    const int controller_y0 =
        UC8279C_VISIBLE_HEIGHT - (frame->y + frame->height);
    const int controller_y1 = UC8279C_VISIBLE_HEIGHT - 1 - frame->y;
    const int x1 = frame->x + frame->width - 1;
    const uint8_t payload[] = {
        (uint8_t)(frame->x >> 8), (uint8_t)frame->x,
        (uint8_t)(x1 >> 8), (uint8_t)x1,
        (uint8_t)(controller_y0 >> 8), (uint8_t)controller_y0,
        (uint8_t)(controller_y1 >> 8), (uint8_t)controller_y1,
        0x01,
    };
    ESP_RETURN_ON_ERROR(send_command(0x91), TAG, "partial in");
    return send_command_data(0x90, payload, sizeof(payload));
}

static esp_err_t write_partial_plane(uint8_t command,
                                     const partial_frame_t *frame)
{
    ESP_RETURN_ON_ERROR(send_command(command), TAG,
                        "partial plane 0x%02x", command);
    const size_t row_offset = (size_t)frame->x / 8U;
    const size_t row_bytes = (size_t)frame->width / 8U;
    for (int panel_y = frame->y + frame->height - 1;
         panel_y >= frame->y; --panel_y) {
        const uint8_t *row = frame->framebuffer +
                             (size_t)panel_y * UC8279C_BYTES_PER_ROW +
                             row_offset;
        memcpy(s_dma_row, row, row_bytes);
        ESP_RETURN_ON_ERROR(send_data(s_dma_row, row_bytes), TAG,
                            "partial row %d", panel_y);
    }
    return ESP_OK;
}

/* The factory 0x10 white seed is a CS-continuous 552-row visible plane.
 * Keep it distinct from the ordinary framebuffer writer for clear logging. */
static esp_err_t write_uniform_rows_continuous_cs(uint8_t value, int rows)
{
    memset(s_dma_row, value, sizeof(s_dma_row));

    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.pin_dc, 1), TAG,
                        "DC data level");
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG,
                        "acquire uniform frame bus");

    esp_err_t err = ESP_OK;
    for (int row_index = 0; row_index < rows; ++row_index) {
        spi_transaction_t transaction = {
            .length = UC8279C_BYTES_PER_ROW * 8,
            .tx_buffer = s_dma_row,
            .flags = row_index + 1 < rows ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(s_spi, &transaction);
        if (err != ESP_OK) {
            break;
        }
    }

    spi_device_release_bus(s_spi);
    return err;
}

static esp_err_t write_factory_white_seed(void)
{
    return write_uniform_rows_continuous_cs(0xff, UC8279C_VISIBLE_HEIGHT);
}

static esp_err_t write_factory_black_seed(void)
{
    ESP_RETURN_ON_ERROR(
        write_uniform_rows_continuous_cs(0x00, UC8279C_VISIBLE_HEIGHT), TAG,
        "uniform black visible block");
    /* The A4 panel exposes 552 of the controller's 600 rows. Keep the hidden
     * 48-row tail white exactly like the factory frame path. */
    return write_uniform_rows_continuous_cs(
        0xff, UC8279C_RAM_HEIGHT - UC8279C_VISIBLE_HEIGHT);
}

esp_err_t epd_uc8279c_init(const epd_uc8279c_config_t *config)
{
    if (config == NULL || config->clock_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "driver built %s %s; gray4 DTM CS=continuous",
             __DATE__, __TIME__);
    s_config = *config;

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << s_config.pin_dc) | (1ULL << s_config.pin_reset),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "EPD output GPIO config");

    const gpio_config_t busy_config = {
        .pin_bit_mask = (1ULL << s_config.pin_busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_config), TAG, "EPD BUSY GPIO config");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = s_config.pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = s_config.pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = UC8279C_BYTES_PER_ROW,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(s_config.host, &bus_config, SPI_DMA_CH_AUTO),
                        TAG,
                        "SPI bus init");

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = s_config.clock_hz,
        .mode = 0,
        .spics_io_num = s_config.pin_cs,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(s_config.host, &device_config, &s_spi),
                        TAG,
                        "SPI device add");

    /* ESP_SLEEP_GPIO_RESET_WORKAROUND otherwise switches every pin to a
     * floating, disabled sleep configuration. Preserve the UC8279C control
     * and SPI output levels so light sleep cannot reset the controller or
     * discard the previous-frame RAM required by DU refreshes. */
    static const char *const pin_names[] = {"SCLK", "MOSI", "CS", "DC", "RST"};
    const int output_pins[] = {
        s_config.pin_sclk,
        s_config.pin_mosi,
        s_config.pin_cs,
        s_config.pin_dc,
        s_config.pin_reset,
    };
    for (size_t i = 0; i < sizeof(output_pins) / sizeof(output_pins[0]); ++i) {
        ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis((gpio_num_t)output_pins[i]), TAG,
                            "preserve %s in light sleep", pin_names[i]);
    }

    ESP_LOGI(TAG,
             "SPI ready: host=%d clock=%dHz SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
             s_config.host,
             s_config.clock_hz,
             s_config.pin_sclk,
             s_config.pin_mosi,
             s_config.pin_cs,
             s_config.pin_dc,
             s_config.pin_reset,
             s_config.pin_busy);
    s_live_profile = s_live_profile_stable;
    s_live_profile_generation = 0;
    s_display_refresh_count = 0;
    s_scan_psr0 = UC8279C_PSR_DEFAULT_0;
    s_scan_pll = UC8279C_PLL_DEFAULT;
    s_scan_debug_override_active = false;
    s_driver_initialized = true;
    ESP_LOGI(TAG, "live profile reset to compiled stable default (non-persistent)");
    return ESP_OK;
}

typedef esp_err_t (*write_new_frame_fn_t)(const void *context);

static esp_err_t write_test_frame_adapter(const void *context)
{
    (void)context;
    return write_test_frame();
}

static esp_err_t write_framebuffer_adapter(const void *context)
{
    return write_framebuffer((const uint8_t *)context);
}

static bool s_panel_ready;
static bool s_panel_powered;
static bool s_power_hold_enabled;
typedef enum {
    BOOSTER_PROFILE_UNKNOWN = 0,
    BOOSTER_PROFILE_GC,
    BOOSTER_PROFILE_DU,
} booster_profile_t;
static booster_profile_t s_booster_profile;

static void invalidate_panel_session(void)
{
    s_reader_wake_black_handoff.pending = false;
    /* A failed power command leaves both the controller RAM and rail state
     * uncertain.  Force the next refresh through hardware reset instead of
     * trying to reuse a potentially half-powered session. */
    s_panel_ready = false;
    s_panel_powered = false;
    s_panel_session_mode = PANEL_SESSION_NONE;
    if (++s_panel_session_generation == 0) {
        ++s_panel_session_generation;
    }
    s_reader_gray3_lut_loaded = false;
    s_reader_gray3_lut_white_clear_boosted = false;
    s_reader_gray3_lut_black_boosted = false;
    s_reader_gray3_lut_fingerprint = 0;
    s_reader_gray3_lut_profile_generation = 0;
    s_reader_gray3_lut_session_generation = 0;
    s_booster_profile = BOOSTER_PROFILE_UNKNOWN;
    s_scan_psr0 = UC8279C_PSR_DEFAULT_0;
    s_scan_pll = UC8279C_PLL_DEFAULT;
    s_scan_debug_override_active = false;
}

static bool gate_direction_is_valid(epd_uc8279c_gate_direction_t direction)
{
    return direction == EPD_UC8279C_GATE_FORWARD ||
           direction == EPD_UC8279C_GATE_REVERSE;
}

static uint8_t psr0_for_gate_direction(
    epd_uc8279c_gate_direction_t direction)
{
    uint8_t psr0 = UC8279C_PSR_DEFAULT_0;
    if (direction == EPD_UC8279C_GATE_REVERSE) {
        psr0 &= (uint8_t)~UC8279C_PSR_GATE_DIRECTION_BIT;
    }
    return psr0;
}

/* These helpers deliberately refuse to touch a powered-down or uninitialized
 * session.  A timing register written into an uncertain controller would be
 * indistinguishable from a half-applied waveform experiment; the next normal
 * refresh must instead rebuild the session from its known defaults. */
static esp_err_t write_gate_direction_register(
    epd_uc8279c_gate_direction_t direction)
{
    if (!gate_direction_is_valid(direction)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_driver_initialized || !s_panel_ready || !s_panel_powered) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                                        "gate direction setup"),
                        TAG, "gate direction idle");
    const uint8_t panel_setting[] = {
        psr0_for_gate_direction(direction),
        UC8279C_PSR_DEFAULT_1,
    };
    ESP_RETURN_ON_ERROR(send_command_data(0x00, panel_setting,
                                          sizeof(panel_setting)),
                        TAG, "gate direction PSR");
    s_scan_psr0 = panel_setting[0];
    s_scan_debug_override_active = true;
    ESP_LOGI(TAG, "gate direction set: PSR=0x%02x (%s)",
             panel_setting[0],
             direction == EPD_UC8279C_GATE_FORWARD ? "forward" : "reverse");
    return ESP_OK;
}

static esp_err_t write_pll_register(uint8_t pll)
{
    if (!s_driver_initialized || !s_panel_ready || !s_panel_powered) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                                        "PLL setup"),
                        TAG, "PLL idle");
    ESP_RETURN_ON_ERROR(send_command_data(0x30, &pll, 1), TAG,
                        "PLL setup");
    s_scan_pll = pll;
    s_scan_debug_override_active = true;
    ESP_LOGI(TAG, "PLL set: 0x%02x", pll);
    return ESP_OK;
}

static esp_err_t write_scan_defaults(void)
{
    if (!s_driver_initialized || !s_panel_ready || !s_panel_powered) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                                        "scan default setup"),
                        TAG, "scan default idle");
    const uint8_t panel_setting[] = {
        UC8279C_PSR_DEFAULT_0,
        UC8279C_PSR_DEFAULT_1,
    };
    const uint8_t pll = UC8279C_PLL_DEFAULT;
    ESP_RETURN_ON_ERROR(send_command_data(0x00, panel_setting,
                                          sizeof(panel_setting)),
                        TAG, "restore PSR");
    ESP_RETURN_ON_ERROR(send_command_data(0x30, &pll, 1), TAG,
                        "restore PLL");
    s_scan_psr0 = panel_setting[0];
    s_scan_pll = pll;
    s_scan_debug_override_active = false;
    ESP_LOGI(TAG, "scan defaults restored: PSR=0x%02x PLL=0x%02x",
             s_scan_psr0, s_scan_pll);
    return ESP_OK;
}

static esp_err_t restore_scan_defaults_before_normal_refresh(void)
{
    if (!s_scan_debug_override_active) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "discarding session-local scan debug override before normal refresh");
    return write_scan_defaults();
}

static esp_err_t panel_power_on(void)
{
    if (s_panel_powered) {
        return ESP_OK;
    }

    esp_err_t err = send_command(0x04);
    if (err == ESP_OK) {
        err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "power on");
    }
    if (err != ESP_OK) {
        invalidate_panel_session();
        return err;
    }

    s_panel_powered = true;
    return ESP_OK;
}

static esp_err_t panel_power_off_impl(const char *caller)
{
    s_reader_wake_black_handoff.pending = false;
    if (!s_panel_powered) {
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "EPD 0x02 POWER_OFF caller=%s session=%d ready=%d powered=%d "
             "controller_sleeping=%d hold=%d refresh_count=%lu",
             caller != NULL ? caller : "unknown", (int)s_panel_session_mode,
             s_panel_ready ? 1 : 0, s_panel_powered ? 1 : 0,
             s_controller_sleeping ? 1 : 0, s_power_hold_enabled ? 1 : 0,
             (unsigned long)s_display_refresh_count);
    esp_err_t err = send_command(0x02);
    if (err == ESP_OK) {
        err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "power off");
    }
    if (err != ESP_OK) {
        invalidate_panel_session();
        return err;
    }

    s_panel_powered = false;
    if (++s_panel_session_generation == 0) {
        ++s_panel_session_generation;
    }
    s_reader_gray3_lut_loaded = false;
    s_reader_gray3_lut_white_clear_boosted = false;
    s_reader_gray3_lut_black_boosted = false;
    s_reader_gray3_lut_fingerprint = 0;
    s_reader_gray3_lut_profile_generation = 0;
    s_reader_gray3_lut_session_generation = 0;
    return ESP_OK;
}

#define panel_power_off() panel_power_off_impl(__func__)

static esp_err_t configure_booster_for_refresh(bool fast)
{
    /* 0x06 is programmed by panel_session_begin() and remains fixed until a
     * hardware reset. Switching it here required POWER_OFF/POWER_ON between
     * pages and caused the controller's old-image history to lose its
     * reference, producing the white stripe at the previous text edge. */
    (void)fast;
    return ESP_OK;
}

static esp_err_t abort_refresh(esp_err_t cause, const char *stage)
{
    ESP_LOGE(TAG, "%s failed: %s; discard panel session",
             stage, esp_err_to_name(cause));

    /* A partially written LUT/frame is not safe to reuse.  Best-effort
     * POWER_OFF limits the time that the panel can remain biased, then the
     * next refresh starts from hardware reset regardless of the result. */
    if (s_panel_powered) {
        const esp_err_t power_off_err = panel_power_off();
        if (power_off_err != ESP_OK) {
            ESP_LOGE(TAG, "refresh abort power-off failed: %s",
                     esp_err_to_name(power_off_err));
        }
    }
    invalidate_panel_session();
    return cause;
}

/* Bring the panel out of reset and configure it. DTM1 starts white and the
 * mono refresh path synchronizes every displayed target back into DTM1. */
static esp_err_t panel_session_begin(bool fast)
{
    const uint8_t panel_setting[] = {0x3f, 0x4a};
    const uint8_t *const power_setting = s_live_profile.power_setting;
    const uint8_t *const booster_soft_start = s_live_profile.booster;
    const size_t booster_soft_start_size = s_live_profile.booster_len;
    const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58}; /* 768 x 600 RAM */
    const uint8_t flash_control[] = {0x00, 0x00, 0x00, 0x00};
    /* PFS must match the official gray-path initializer. */
    const uint8_t power_off_sequence = 0x20;
    const uint8_t one_byte_24 = 0x32;
    const uint8_t one_byte_0f = 0x0f;
    const uint8_t one_byte_02 = 0x02;

    ESP_LOGI(TAG,
             "profile gen=%lu reset/init %s: 0x01=%02x %02x %02x %02x %02x; "
             "0x06=%02x %02x %02x %02x len=%u; 0x50 GC=%02x DU=%02x",
             (unsigned long)s_live_profile_generation,
             fast ? "DU" : "GC",
             power_setting[0], power_setting[1], power_setting[2],
             power_setting[3], power_setting[4],
             booster_soft_start[0], booster_soft_start[1],
             booster_soft_start[2], booster_soft_start_size == 4 ? booster_soft_start[3] : 0,
             (unsigned)booster_soft_start_size, s_live_profile.gc_cdi,
             s_live_profile.du_cdi);
    invalidate_panel_session();
    ESP_RETURN_ON_ERROR(hardware_reset(), TAG, "hardware reset");
    ESP_RETURN_ON_ERROR(send_command_data(0x00, panel_setting, sizeof(panel_setting)), TAG, "panel setting");
    ESP_RETURN_ON_ERROR(send_command_data(0x03, &power_off_sequence, 1), TAG,
                        "power off sequence");
    ESP_RETURN_ON_ERROR(send_command_data(0x01, power_setting, sizeof(power_setting)), TAG, "power setting");
    ESP_RETURN_ON_ERROR(send_command_data(0x06, booster_soft_start,
                                          booster_soft_start_size),
                        TAG, "booster");
    /* GC and DU use the same factory booster profile. Keep the selected mode
     * in the session state for diagnostics, but never rewrite register 0x06
     * without a full panel reset. */
    s_booster_profile = fast ? BOOSTER_PROFILE_DU : BOOSTER_PROFILE_GC;
    ESP_RETURN_ON_ERROR(send_command_data(0x82, &one_byte_24, 1), TAG, "VCOM DC");
    ESP_RETURN_ON_ERROR(send_command_data(0x30, &one_byte_0f, 1), TAG, "PLL");
    ESP_RETURN_ON_ERROR(send_command_data(0x61, resolution, sizeof(resolution)), TAG, "resolution");
    ESP_RETURN_ON_ERROR(wait_until_idle(5000, "resolution setup"), TAG, "resolution BUSY");
    ESP_RETURN_ON_ERROR(send_command_data(0x65, flash_control, sizeof(flash_control)), TAG, "flash control");
    ESP_RETURN_ON_ERROR(send_command_data(0xe1, &one_byte_02, 1), TAG, "temperature sensor");
    ESP_LOGI(TAG,
             "profile gen=%lu init registers: 0x01=%02x %02x %02x %02x %02x; "
             "0x06=%02x %02x %02x %02x (%u); 0x65=%02x %02x %02x %02x",
             (unsigned long)s_live_profile_generation,
             power_setting[0], power_setting[1], power_setting[2],
             power_setting[3], power_setting[4], booster_soft_start[0],
             booster_soft_start[1], booster_soft_start[2],
             booster_soft_start_size == 4 ? booster_soft_start[3] : 0,
             (unsigned)booster_soft_start_size, flash_control[0],
             flash_control[1], flash_control[2], flash_control[3]);

    ESP_RETURN_ON_ERROR(send_command(0x10), TAG,
                        "initial old image command");
    ESP_RETURN_ON_ERROR(write_factory_white_seed(), TAG,
                        "initial old image white frame");

    s_panel_ready = true;
    s_panel_session_mode = PANEL_SESSION_MONO;
    return ESP_OK;
}

/* A4's full-power gray initializer is deliberately separate from the live
 * GC/DU profile. Its 0x01/0x65 values and PON-before-white-seed order belong
 * to the 49-byte gray LUT and must not leak into ordinary page refreshes. */
static esp_err_t panel_session_begin_a4_gray49(void)
{
    static const uint8_t panel_setting[] = {0x3f, 0x4a};
    static const uint8_t power_setting[] = {0x43, 0x00, 0x78, 0x78, 0x17};
    static const uint8_t booster[] = {0x25, 0x25, 0x3c};
    static const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58};
    static const uint8_t gate_source[] = {0x00, 0x00, 0x20, 0x00};
    const uint8_t power_off_sequence = 0x20;
    const uint8_t vcom_dc = 0x24;
    const uint8_t pll = 0x0f;
    const uint8_t tcon = 0x02;

    invalidate_panel_session();
    ESP_RETURN_ON_ERROR(hardware_reset(), TAG, "A4 gray reset");
    ESP_RETURN_ON_ERROR(send_command_data(0x00, panel_setting,
                                          sizeof(panel_setting)), TAG,
                        "A4 gray panel setting");
    ESP_RETURN_ON_ERROR(send_command_data(0x03, &power_off_sequence, 1), TAG,
                        "A4 gray power-off sequence");
    ESP_RETURN_ON_ERROR(send_command_data(0x01, power_setting,
                                          sizeof(power_setting)), TAG,
                        "A4 gray power setting");
    ESP_RETURN_ON_ERROR(send_command_data(0x06, booster, sizeof(booster)),
                        TAG, "A4 gray booster");
    ESP_RETURN_ON_ERROR(send_command_data(0x82, &vcom_dc, 1), TAG,
                        "A4 gray VCOM DC");
    ESP_RETURN_ON_ERROR(send_command_data(0x30, &pll, 1), TAG,
                        "A4 gray PLL");
    ESP_RETURN_ON_ERROR(send_command_data(0x61, resolution,
                                          sizeof(resolution)), TAG,
                        "A4 gray resolution");
    ESP_RETURN_ON_ERROR(send_command_data(0x65, gate_source,
                                          sizeof(gate_source)), TAG,
                        "A4 gray gate/source");
    ESP_RETURN_ON_ERROR(send_command_data(0xe1, &tcon, 1), TAG,
                        "A4 gray TCON");
    ESP_RETURN_ON_ERROR(panel_power_on(), TAG, "A4 gray power on");
    ESP_RETURN_ON_ERROR(send_command(0x10), TAG,
                        "A4 gray initial DTM1 white command");
    ESP_RETURN_ON_ERROR(write_uniform_rows_continuous_cs(
                            0xff, UC8279C_RAM_HEIGHT), TAG,
                        "A4 gray initial DTM1 white frame");
    s_panel_ready = true;
    s_panel_session_mode = PANEL_SESSION_GRAY49;
    ESP_LOGI(TAG, "A4 gray49 reset/init: 0x01=43 00 78 78 17 "
                  "0x65=00 00 20 00 PON before DTM1 white");
    return ESP_OK;
}

static bool live_profile_is_valid(const epd_uc8279c_live_profile_t *profile)
{
    return profile != NULL &&
           (profile->booster_len == 3 || profile->booster_len == 4) &&
           profile->gc_lut <= EPD_UC8279C_GC_LUT_A4_FACTORY_42 &&
           profile->du_lut <= EPD_UC8279C_DU_LUT_FACTORY_BLACK_REPEAT_49;
}

void epd_uc8279c_live_profile_default(epd_uc8279c_live_profile_t *profile)
{
    if (profile != NULL) *profile = s_live_profile_stable;
}

esp_err_t epd_uc8279c_set_live_profile(const epd_uc8279c_live_profile_t *profile)
{
    if (!live_profile_is_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(&s_live_profile, profile, sizeof(s_live_profile)) != 0) {
        s_live_profile = *profile;
        ++s_live_profile_generation;
    }
    ESP_LOGI(TAG, "live profile selected for production gen=%lu (no refresh)",
             (unsigned long)s_live_profile_generation);
    return ESP_OK;
}

void epd_uc8279c_get_live_profile(epd_uc8279c_live_profile_t *profile)
{
    if (profile != NULL) *profile = s_live_profile;
}

uint32_t epd_uc8279c_live_profile_generation(void)
{
    return s_live_profile_generation;
}

static esp_err_t apply_current_live_profile(
    bool use_du, const uint8_t *old_framebuffer,
    const uint8_t *target_framebuffer, bool hold_power,
    int64_t *busy_start_us)
{
    const bool direct_transition = old_framebuffer != NULL;
    const int64_t stage_start_us = esp_timer_get_time();
    esp_err_t err = panel_session_begin(use_du);
    if (err != ESP_OK) goto failed;
    if (direct_transition) {
        err = send_command(0x10);
        if (err != ESP_OK) goto failed;
        err = write_framebuffer(old_framebuffer);
        if (err != ESP_OK) goto failed;
    }
    const bool pon_reused = s_panel_powered;
    err = panel_power_on();
    if (err != ESP_OK) goto failed;
    ESP_LOGI(TAG, "profile apply PON=%s session=%s",
             pon_reused ? "reused" : "new", direct_transition ? "direct" : "seed");
    err = send_lut(use_du);
    if (err != ESP_OK) goto failed;
    err = send_command(0x13);
    if (err != ESP_OK) goto failed;
    err = direct_transition ? write_framebuffer(target_framebuffer)
                            : write_factory_white_seed();
    if (err != ESP_OK) goto failed;
    err = send_command(0x12);
    if (err != ESP_OK) goto failed;
    *busy_start_us = esp_timer_get_time();
    err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "profile apply refresh");
    if (err != ESP_OK) goto failed;
    if (direct_transition) {
        err = send_command(0x10);
        if (err != ESP_OK) goto failed;
        err = write_framebuffer(target_framebuffer);
        if (err != ESP_OK) goto failed;
        ESP_LOGI(TAG,
                 "profile apply direct transition synchronized target into DTM1");
    }
    if (!hold_power) {
        err = panel_power_off();
        if (err != ESP_OK) goto failed;
    }
    ESP_LOGI(TAG, "profile apply complete mode=%s HV=%s stages=%lldus",
             use_du ? "DU" : "GC", hold_power ? "held" : "off",
             (long long)(esp_timer_get_time() - stage_start_us));
    return ESP_OK;

failed:
    return abort_refresh(err, "profile apply");
}

esp_err_t epd_uc8279c_apply_live_profile(
    const epd_uc8279c_live_profile_t *profile, bool use_du,
    const uint8_t *old_framebuffer, const uint8_t *target_framebuffer,
    size_t framebuffer_size, bool hold_power,
    epd_uc8279c_live_profile_result_t *result)
{
    const int64_t start_us = esp_timer_get_time();
    const epd_uc8279c_live_profile_t previous_profile = s_live_profile;
    const uint32_t previous_generation = s_live_profile_generation;
    int64_t busy_start_us = 0;
    esp_err_t err = ESP_OK;
    if (result != NULL) *result = (epd_uc8279c_live_profile_result_t){0};
    const bool has_old_frame = old_framebuffer != NULL;
    const bool has_target_frame = target_framebuffer != NULL;
    const bool invalid_transition =
        has_old_frame != has_target_frame ||
        (has_old_frame &&
         framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES);
    if (!s_driver_initialized || !live_profile_is_valid(profile) ||
        invalid_transition) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (s_panel_powered) {
        const esp_err_t off_err = panel_power_off();
        if (off_err != ESP_OK) {
            ESP_LOGW(TAG, "profile apply pre-reset POWER_OFF: %s", esp_err_to_name(off_err));
        }
    }
    invalidate_panel_session();
    s_live_profile = *profile;
    ++s_live_profile_generation;
    ESP_LOGI(TAG, "profile apply gen=%lu mode=%s transition=%s begin",
             (unsigned long)s_live_profile_generation,
             use_du ? "DU" : "GC", has_old_frame ? "direct" : "white");
    err = apply_current_live_profile(use_du, old_framebuffer,
                                     target_framebuffer, hold_power, &busy_start_us);
    if (err != ESP_OK) goto rollback;
    ESP_LOGI(TAG, "profile apply gen=%lu complete mode=%s refresh=%lldus",
             (unsigned long)s_live_profile_generation, use_du ? "DU" : "GC",
             (long long)(esp_timer_get_time() - busy_start_us));
    goto done;
rollback:
    /* The candidate may have reached only part of the controller. Keep its
     * failure from becoming the next UI refresh configuration. The aborted
     * session stays invalid, so that refresh rebuilds from this profile. */
    s_live_profile = previous_profile;
    s_live_profile_generation = previous_generation;
    invalidate_panel_session();
    ESP_LOGW(TAG, "profile apply failed: restored gen=%lu; next refresh hard-rebuilds",
             (unsigned long)s_live_profile_generation);
done:
    if (result != NULL) {
        result->err = err;
        result->busy_wait_us = busy_start_us == 0 ? 0 : esp_timer_get_time() - busy_start_us;
        result->elapsed_us = esp_timer_get_time() - start_us;
        result->generation = s_live_profile_generation;
    }
    return err;
}

esp_err_t epd_uc8279c_restore_stable_profile(
    epd_uc8279c_live_profile_result_t *result)
{
    const int64_t start_us = esp_timer_get_time();
    int64_t busy_start_us = 0;
    const bool already_stable =
        memcmp(&s_live_profile, &s_live_profile_stable, sizeof(s_live_profile)) == 0;
    esp_err_t err;

    if (result != NULL) *result = (epd_uc8279c_live_profile_result_t){0};
    if (!s_driver_initialized) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    /* Commit the compiled profile before touching hardware. A failed reset or
     * BUSY wait must still leave ordinary UI refreshes configured for stable. */
    s_live_profile = s_live_profile_stable;
    if (!already_stable) ++s_live_profile_generation;
    if (s_panel_powered) {
        const esp_err_t off_err = panel_power_off();
        if (off_err != ESP_OK) {
            ESP_LOGW(TAG, "stable restore pre-reset POWER_OFF: %s",
                     esp_err_to_name(off_err));
        }
    }
    invalidate_panel_session();
    ESP_LOGI(TAG, "stable restore gen=%lu begin", (unsigned long)s_live_profile_generation);
    err = apply_current_live_profile(false, NULL, NULL, false, &busy_start_us);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stable restore gen=%lu complete refresh=%lldus",
                 (unsigned long)s_live_profile_generation,
                 (long long)(esp_timer_get_time() - busy_start_us));
    } else {
        /* apply_current_live_profile has already powered down and invalidated
         * the uncertain session. The stable software profile remains active. */
        ESP_LOGE(TAG, "stable restore failed: stable gen=%lu retained; next refresh hard-rebuilds",
                 (unsigned long)s_live_profile_generation);
    }
done:
    if (result != NULL) {
        result->err = err;
        result->busy_wait_us = busy_start_us == 0 ? 0 : esp_timer_get_time() - busy_start_us;
        result->elapsed_us = esp_timer_get_time() - start_us;
        result->generation = s_live_profile_generation;
    }
    return err;
}

static esp_err_t refresh_with_writer(write_new_frame_fn_t writer,
                                     const void *context,
                                     bool fast, bool lockscreen_double_black)
{
    s_reader_wake_black_handoff.pending = false;
    const int64_t total_start_us = esp_timer_get_time();
    int64_t stage_start_us = total_start_us;
    uint32_t init_ms = 0;

    if (!s_panel_ready || s_panel_session_mode != PANEL_SESSION_MONO) {
        /* A retained gray session has different plane/LUT semantics. Close
         * any held rails before resetting into the mono old-frame contract. */
        if (s_panel_powered) {
            ESP_RETURN_ON_ERROR(panel_power_off(), TAG,
                                "power off incompatible session");
        }
        ESP_RETURN_ON_ERROR(panel_session_begin(fast), TAG, "panel init");
        init_ms = (uint32_t)((esp_timer_get_time() - stage_start_us + 500) /
                             1000);
    } else {
        ESP_RETURN_ON_ERROR(configure_booster_for_refresh(fast), TAG,
                            "select booster");
    }

    const bool reused_power = s_panel_powered;
    stage_start_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(panel_power_on(), TAG, "power on");
    esp_err_t scan_err = restore_scan_defaults_before_normal_refresh();
    if (scan_err != ESP_OK) {
        return abort_refresh(scan_err, "restore scan defaults");
    }
    const uint32_t power_on_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    esp_err_t err = send_lut_scaled(fast, lockscreen_double_black);
    if (err != ESP_OK) {
        return abort_refresh(err, "load LUT");
    }
    const uint32_t lut_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    err = send_command(0x13);
    if (err == ESP_OK) {
        err = writer(context);
    }
    if (err != ESP_OK) {
        return abort_refresh(err, "new image frame");
    }
    const uint32_t frame_write_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    const unsigned passes = lockscreen_double_black ? 2 : 1;
    for (unsigned pass = 0; pass < passes; ++pass) {
        err = send_command(0x12);
        if (err == ESP_OK) {
            err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                                 lockscreen_double_black ? "lockscreen half-black" : "refresh");
        }
        if (err != ESP_OK) return abort_refresh(err, "display refresh");
    }
    const uint32_t waveform_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    const uint32_t old_frame_write_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    if (!s_power_hold_enabled) {
        ESP_RETURN_ON_ERROR(panel_power_off(), TAG, "power off");
    }
    const uint32_t power_off_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);
    const uint32_t total_ms =
        (uint32_t)((esp_timer_get_time() - total_start_us + 500) / 1000);
    ESP_LOGI(TAG,
             "refresh %s timing: init=%lums power=%lums lut=%lums "
             "frame=%lums wave=%lums old=%lums off=%lums total=%lums "
             "power=%s",
             fast ? "DU" : "GC",
             (unsigned long)init_ms,
             (unsigned long)power_on_ms,
             (unsigned long)lut_ms,
             (unsigned long)frame_write_ms,
             (unsigned long)waveform_ms,
             (unsigned long)old_frame_write_ms,
             (unsigned long)power_off_ms,
             (unsigned long)total_ms,
             s_power_hold_enabled ? (reused_power ? "held/reused" : "held")
                                  : "off");
    return ESP_OK;
}

static esp_err_t refresh_partial(partial_frame_t *frame)
{
    s_reader_wake_black_handoff.pending = false;
    if (!normalize_partial_frame(frame)) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t total_start_us = esp_timer_get_time();
    int64_t stage_start_us = total_start_us;
    uint32_t init_ms = 0;
    if (!s_panel_ready || s_panel_session_mode != PANEL_SESSION_MONO) {
        if (s_panel_powered) {
            ESP_RETURN_ON_ERROR(panel_power_off(), TAG,
                                "power off incompatible partial session");
        }
        ESP_RETURN_ON_ERROR(panel_session_begin(true), TAG,
                            "partial panel init");
        init_ms = (uint32_t)((esp_timer_get_time() - stage_start_us + 500) /
                             1000);
    } else {
        ESP_RETURN_ON_ERROR(configure_booster_for_refresh(true), TAG,
                            "select partial booster");
    }

    const bool reused_power = s_panel_powered;
    stage_start_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(panel_power_on(), TAG, "partial power on");
    esp_err_t scan_err = restore_scan_defaults_before_normal_refresh();
    if (scan_err != ESP_OK) {
        return abort_refresh(scan_err, "restore partial scan defaults");
    }
    const uint32_t power_on_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    esp_err_t err = send_lut(true);
    if (err != ESP_OK) {
        return abort_refresh(err, "partial load LUT");
    }
    const uint32_t lut_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    bool window_open = false;
    stage_start_us = esp_timer_get_time();
    err = enter_partial_window(frame);
    if (err == ESP_OK) {
        window_open = true;
        err = write_partial_plane(0x13, frame);
    }
    if (err != ESP_OK) {
        if (window_open) (void)send_command(0x92);
        return abort_refresh(err, "partial new image");
    }
    const uint32_t frame_write_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    err = send_command(0x12);
    if (err == ESP_OK) {
        err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "partial refresh");
    }
    if (err != ESP_OK) {
        (void)send_command(0x92);
        return abort_refresh(err, "partial refresh waveform");
    }
    const uint32_t waveform_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    err = write_partial_plane(0x10, frame);
    if (err == ESP_OK) {
        err = send_command(0x92);
        window_open = false;
    }
    if (err != ESP_OK) {
        if (window_open) (void)send_command(0x92);
        return abort_refresh(err, "partial old image");
    }
    const uint32_t old_frame_write_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);

    stage_start_us = esp_timer_get_time();
    if (!s_power_hold_enabled) {
        err = panel_power_off();
        if (err != ESP_OK) {
            return abort_refresh(err, "partial power off");
        }
    }
    const uint32_t power_off_ms =
        (uint32_t)((esp_timer_get_time() - stage_start_us + 500) / 1000);
    const uint32_t total_ms =
        (uint32_t)((esp_timer_get_time() - total_start_us + 500) / 1000);
    ESP_LOGI(TAG,
             "refresh DU-partial rect=%d,%d,%d,%d timing: init=%lums "
             "power=%lums lut=%lums frame=%lums wave=%lums old=%lums "
             "off=%lums total=%lums power=%s",
             frame->x, frame->y, frame->width, frame->height,
             (unsigned long)init_ms, (unsigned long)power_on_ms,
             (unsigned long)lut_ms, (unsigned long)frame_write_ms,
             (unsigned long)waveform_ms, (unsigned long)old_frame_write_ms,
             (unsigned long)power_off_ms, (unsigned long)total_ms,
             s_power_hold_enabled ? (reused_power ? "held/reused" : "held")
                                  : "off");
    return ESP_OK;
}

esp_err_t epd_uc8279c_set_power_hold(bool enabled)
{
    if (enabled == s_power_hold_enabled) {
        return ESP_OK;
    }

    if (!enabled && s_panel_powered) {
        ESP_RETURN_ON_ERROR(panel_power_off(), TAG,
                            "disable panel power retention");
    }

    s_power_hold_enabled = enabled;
    if (enabled) {
        ESP_LOGI(TAG,
                 "panel power retention enabled; HV rails remain on "
                 "after refresh");
    } else {
        ESP_LOGI(TAG, "panel power retention disabled");
    }
    return ESP_OK;
}

bool epd_uc8279c_power_hold_enabled(void)
{
    return s_power_hold_enabled;
}

esp_err_t epd_uc8279c_power_off(void)
{
    if (!s_driver_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = panel_power_off_impl(__func__);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "EPD reader idle: POF 0x02 only; controller sleep skipped");
    }
    return err;
}

esp_err_t epd_uc8279c_sleep(void)
{
    s_reader_wake_black_handoff.pending = false;
    if (!s_driver_initialized || s_controller_sleeping) {
        return ESP_OK;
    }
    /* 会话失效可能来自灰阶完成或失败。明确发送 POF 并等待 BUSY，
     * 再关闭控制器；不能把 s_panel_ready=false 当成已休眠。 */
    esp_err_t err = ESP_OK;
    if (s_panel_powered) {
        ESP_LOGW(TAG,
                 "EPD 0x02 DEEP_SLEEP_POWER_OFF caller=%s session=%d ready=%d "
                 "powered=1 refresh_count=%lu",
                 __func__, (int)s_panel_session_mode, s_panel_ready ? 1 : 0,
                 (unsigned long)s_display_refresh_count);
        err = send_command(0x02);
        if (err == ESP_OK) {
            err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                                  "power off before deep sleep");
        }
    } else {
        ESP_LOGI(TAG,
                 "EPD deep sleep: skip redundant 0x02, panel rails already off "
                 "refresh_count=%lu",
                 (unsigned long)s_display_refresh_count);
    }
    if (err == ESP_OK) {
        const uint8_t deep_sleep_check = 0xa5;
        err = send_command_data(0x07, &deep_sleep_check, 1);
    }
    invalidate_panel_session();
    s_controller_sleeping = err == ESP_OK;
    return err;
}

esp_err_t epd_uc8279c_cold_reset(void)
{
    if (!s_driver_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_panel_powered) {
        ESP_RETURN_ON_ERROR(panel_power_off(), TAG,
                            "power off before cold reset");
    }
    invalidate_panel_session();
    s_controller_sleeping = false;
    ESP_RETURN_ON_ERROR(hardware_reset(), TAG, "cold hardware reset");
    ESP_LOGI(TAG, "panel cold reset complete");
    return ESP_OK;
}

esp_err_t epd_uc8279c_show_test_pattern(void)
{
    return refresh_with_writer(write_test_frame_adapter, NULL, false, false);
}

uint32_t epd_uc8279c_display_refresh_count(void)
{
    return s_display_refresh_count;
}

void epd_uc8279c_reset_display_refresh_count(void)
{
    s_display_refresh_count = 0;
}

esp_err_t epd_uc8279c_set_gate_direction(
    epd_uc8279c_gate_direction_t direction)
{
    return write_gate_direction_register(direction);
}

esp_err_t epd_uc8279c_set_pll(uint8_t pll)
{
    return write_pll_register(pll);
}

esp_err_t epd_uc8279c_restore_scan_defaults(void)
{
    return write_scan_defaults();
}

esp_err_t epd_uc8279c_load_custom_lut(const uint8_t *lut, size_t lut_size,
                                      size_t lut_bank_size, uint8_t lut_cdi)
{
    if (!s_driver_initialized || !s_panel_ready || !s_panel_powered) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!custom_lut_shape_is_valid(lut, lut_size, lut_bank_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                                        "custom LUT setup"),
                        TAG, "custom LUT idle");
    return send_custom_lut(lut, lut_size, lut_bank_size, lut_cdi);
}

esp_err_t epd_uc8279c_gate_probe_use_single_frame_lut(
    epd_uc8279c_gate_probe_config_t *config)
{
    if (config == NULL ||
        uc8279c_lut_page_turn_single_frame_v1_len !=
            UC8279C_LUT_PHASE_BYTES * UC8279C_LUT_PHASE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    config->lut = uc8279c_lut_page_turn_single_frame_v1;
    config->lut_size = uc8279c_lut_page_turn_single_frame_v1_len;
    config->lut_bank_size = UC8279C_LUT_PHASE_BYTES;
    /* Preserve the live DU bank-selection contract. This unit may restore a
     * device-validated 0xd5 profile from NVS even though the compiled factory
     * fallback is 0xd7; changing CDI together with timing would make the
     * optical result ambiguous. */
    config->lut_cdi = s_live_profile.du_cdi;
    return ESP_OK;
}

esp_err_t epd_uc8279c_run_gate_probe(
    const uint8_t *old_framebuffer, const uint8_t *next_framebuffer,
    size_t framebuffer_size,
    const epd_uc8279c_gate_probe_config_t *config,
    epd_uc8279c_gate_probe_result_t *result)
{
    const int64_t total_start_us = esp_timer_get_time();
    const uint32_t refresh_count_before = s_display_refresh_count;
    int64_t busy_start_us = 0;
    esp_err_t err = ESP_OK;

    if (result != NULL) {
        *result = (epd_uc8279c_gate_probe_result_t){0};
    }
    if (!s_driver_initialized || old_framebuffer == NULL ||
        next_framebuffer == NULL ||
        framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES ||
        config == NULL || !gate_direction_is_valid(config->direction) ||
        !custom_lut_config_is_valid(config)) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* A probe is a complete diagnostic transaction. Do not inherit an
     * uncertain powered session from a previous failed experiment. */
    if (s_panel_powered) {
        err = panel_power_off();
        if (err != ESP_OK) {
            goto failed;
        }
    }
    invalidate_panel_session();

    err = panel_session_begin(true);
    if (err != ESP_OK) goto failed;
    err = panel_power_on();
    if (err != ESP_OK) goto failed;

    err = send_command(0x10);
    if (err == ESP_OK) err = write_framebuffer(old_framebuffer);
    if (err != ESP_OK) goto failed;

    err = send_command(0x13);
    if (err == ESP_OK) err = write_framebuffer(next_framebuffer);
    if (err != ESP_OK) goto failed;

    /* The production reset path remains 0x3f/0x0f. Override only this idle
     * probe session, then restore those values after BUSY releases. Keep the
     * old/new image writes ahead of the experiment-specific registers so the
     * transaction mirrors the page-turn plan exactly. */
    err = write_gate_direction_register(config->direction);
    if (err != ESP_OK) goto failed;
    err = write_pll_register(config->pll);
    if (err != ESP_OK) goto failed;

    if (config->lut != NULL) {
        err = send_custom_lut(config->lut, config->lut_size,
                              config->lut_bank_size, config->lut_cdi);
    } else {
        err = send_lut(true);
    }
    if (err != ESP_OK) goto failed;

    ESP_LOGI(TAG,
             "gate probe begin: direction=%s PSR=0x%02x PLL=0x%02x",
             config->direction == EPD_UC8279C_GATE_FORWARD ? "forward" : "reverse",
             s_scan_psr0, s_scan_pll);
    busy_start_us = esp_timer_get_time();
    err = send_command(0x12);
    if (err == ESP_OK) {
        err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "gate probe refresh");
    }
    if (err != ESP_OK) goto failed;

    /* Synchronize both controller image planes without a second activation.
     * Keeping DTM2 aligned is important for a later experiment that switches
     * between K/W waveform modes; it is intentionally not added to the
     * ordinary mono path until that behavior is separately validated. */
    err = send_command(0x10);
    if (err == ESP_OK) err = write_framebuffer(next_framebuffer);
    if (err != ESP_OK) goto failed;
    err = send_command(0x13);
    if (err == ESP_OK) err = write_framebuffer(next_framebuffer);
    if (err != ESP_OK) goto failed;

    /* BUSY is known high here, so restoring timing registers cannot race the
     * waveform. A restore failure invalidates the session rather than leaving
     * the next normal refresh on a diagnostic PLL/direction. */
    err = write_scan_defaults();
    if (err != ESP_OK) goto failed;
    if (!config->hold_power) {
        err = panel_power_off();
        if (err != ESP_OK) goto failed;
    }
    ESP_LOGI(TAG,
             "gate probe complete: direction=%s pll=0x%02x busy=%lldus "
             "refresh_delta=%lu power=%s",
             config->direction == EPD_UC8279C_GATE_FORWARD ? "forward" : "reverse",
             config->pll,
             (long long)(esp_timer_get_time() - busy_start_us),
             (unsigned long)(s_display_refresh_count - refresh_count_before),
             config->hold_power ? "held" : "off");
    goto done;

failed:
    if (err == ESP_OK) err = ESP_FAIL;
    err = abort_refresh(err, "gate probe");

done:
    if (result != NULL) {
        result->err = err;
        result->display_refresh_delta =
            s_display_refresh_count - refresh_count_before;
        result->busy_wait_us = busy_start_us == 0
                                   ? 0
                                   : esp_timer_get_time() - busy_start_us;
        result->elapsed_us = esp_timer_get_time() - total_start_us;
    }
    return err;
}

esp_err_t epd_uc8279c_show_frame(const uint8_t *framebuffer, size_t framebuffer_size)
{
    s_reader_wake_black_handoff.pending = false;
    if (framebuffer == NULL || framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return refresh_with_writer(write_framebuffer_adapter, framebuffer, false, false);
}

esp_err_t epd_uc8279c_show_reader_wake_black(const uint8_t *framebuffer,
                                            size_t framebuffer_size)
{
    s_reader_wake_black_handoff.pending = false;
    if (framebuffer == NULL || framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < framebuffer_size; ++i) {
        if (framebuffer[i] != 0) return ESP_ERR_INVALID_ARG;
    }
    /* 与原单黑帧完全相同的 GC/一次 DRF；不修改电压、LUT 或保电策略。
     * 后续未知历史的阅读恢复会完整重写 DTM1/DTM2 和灰阶 LUT，因此
     * 可复用这次 panel_session_begin(false) 的同一套基础寄存器配置。 */
    const esp_err_t err = epd_uc8279c_show_frame(framebuffer, framebuffer_size);
    if (err == ESP_OK && s_panel_ready && s_panel_powered &&
        !s_controller_sleeping && s_panel_session_mode == PANEL_SESSION_MONO) {
        s_reader_wake_black_handoff = (reader_wake_black_handoff_t){
            .pending = true,
            .session_generation = s_panel_session_generation,
            .profile_generation = s_live_profile_generation,
            .refresh_count = s_display_refresh_count,
        };
    }
    return err;
}

esp_err_t epd_uc8279c_show_lockscreen_double_black(const uint8_t *framebuffer,
                                                size_t framebuffer_size)
{
    if (framebuffer == NULL || framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES)
        return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < framebuffer_size; ++i) {
        if (framebuffer[i] != 0) return ESP_ERR_INVALID_ARG;
    }
    return refresh_with_writer(write_framebuffer_adapter, framebuffer, false, true);
}

esp_err_t epd_uc8279c_show_frame_fast(const uint8_t *framebuffer, size_t framebuffer_size)
{
    if (framebuffer == NULL || framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return refresh_with_writer(write_framebuffer_adapter, framebuffer, true, false);
}

esp_err_t epd_uc8279c_show_frame_fast_partial(
    const uint8_t *framebuffer, size_t framebuffer_size,
    int x, int y, int width, int height)
{
    if (framebuffer == NULL ||
        framebuffer_size < EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    partial_frame_t frame = {
        .framebuffer = framebuffer,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    return refresh_partial(&frame);
}

esp_err_t epd_uc8279c_show_frame_gray4(
    const uint8_t *dtm1_msb, const uint8_t *dtm2_lsb,
    size_t framebuffer_size, epd_uc8279c_gray4_mode_t mode)
{
    if (dtm1_msb == NULL || dtm2_lsb == NULL ||
        framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mode != EPD_UC8279C_GRAY4_FACTORY_A &&
        mode != EPD_UC8279C_GRAY4_FACTORY_B &&
        mode != EPD_UC8279C_GRAY4_FACTORY_C &&
        mode != EPD_UC8279C_GRAY4_FACTORY_B_GC2 &&
        mode != EPD_UC8279C_GRAY4_WAVESHARE_4IN2 &&
        mode != EPD_UC8279C_GRAY4_READER_WHITE_CLEAR) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t refresh_start_us = esp_timer_get_time();
    const char *mode_name = mode == EPD_UC8279C_GRAY4_FACTORY_A
                                ? "Factory A"
                                : mode == EPD_UC8279C_GRAY4_FACTORY_B
                                      ? "Factory B"
                                      : mode == EPD_UC8279C_GRAY4_FACTORY_C
                                            ? "Factory C"
                                            : mode == EPD_UC8279C_GRAY4_FACTORY_B_GC2
                                                  ? "Factory B-GC2"
                                                  : mode == EPD_UC8279C_GRAY4_READER_WHITE_CLEAR
                                                        ? "reader white-clear"
                                                        : "Waveshare-4in2-open";
    ESP_LOGI(TAG,
             "four-gray %s begin: ready=%d powered=%d power-hold=%d",
             mode_name, s_panel_ready, s_panel_powered, s_power_hold_enabled);
    /* Factory gray paths always reset/init before writing their two planes.
     * The reset establishes the controller power state; no pre-reset 0x02 is
     * sent by the factory four-gray path. */
    const esp_err_t init_err = panel_session_begin(true);
    if (init_err != ESP_OK) {
        return init_err;
    }
    ESP_LOGI(TAG, "four-gray %s reset/init complete", mode_name);

    esp_err_t err = panel_power_on();
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray power on");
    }
    ESP_LOGI(TAG, "four-gray %s power on complete", mode_name);

    err = send_command(0x10);
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray DTM1 command");
    }
    err = write_visible_framebuffer_continuous_cs(dtm1_msb);
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray DTM1 frame");
    }

    err = send_command(0x13);
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray DTM2 command");
    }
    err = write_visible_framebuffer_continuous_cs(dtm2_lsb);
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray DTM2 frame");
    }
    ESP_LOGI(TAG, "four-gray %s planes written", mode_name);

    err = send_gray4_lut(mode);
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray LUT");
    }
    ESP_LOGI(TAG, "four-gray %s trigger refresh", mode_name);
    err = send_command(0x12);
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray display refresh");
    }
    err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "four-gray refresh");
    if (err != ESP_OK) {
        return abort_refresh(err, "four-gray display refresh BUSY");
    }

    if (mode == EPD_UC8279C_GRAY4_FACTORY_B_GC2) {
        ESP_LOGI(TAG, "four-gray Factory B-GC2 cleanup drive");
        ESP_RETURN_ON_ERROR(send_command(0x12), TAG,
                            "four-gray cleanup refresh");
        err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                              "four-gray cleanup refresh");
        if (err != ESP_OK) {
            return abort_refresh(err, "four-gray cleanup refresh BUSY");
        }
    }

    /* Match upstream: restore mono DTM1 while the gray session remains active. */
    ESP_RETURN_ON_ERROR(send_command(0x10), TAG,
                        "four-gray mono DTM1 white command");
    ESP_RETURN_ON_ERROR(write_factory_white_seed(), TAG,
                        "four-gray mono DTM1 white seed");
    s_panel_ready = true;
    s_panel_session_mode = PANEL_SESSION_MONO;
    if (!s_power_hold_enabled) {
        ESP_RETURN_ON_ERROR(panel_power_off(), TAG,
                            "four-gray mono power off");
        s_panel_powered = false;
    }

    const uint32_t elapsed_ms =
        (uint32_t)((esp_timer_get_time() - refresh_start_us + 500) / 1000);
    ESP_LOGI(TAG, "refresh four-gray %s: total=%lums; DTM1 white re-synced",
             mode_name,
             (unsigned long)elapsed_ms);
    return ESP_OK;
}

esp_err_t epd_uc8279c_show_frame_gray4_retained(
    const uint8_t *dtm1_msb, const uint8_t *dtm2_lsb,
    size_t framebuffer_size, epd_uc8279c_gray4_mode_t mode)
{
    const bool previous_hold = s_power_hold_enabled;
    s_power_hold_enabled = true;
    const esp_err_t err = epd_uc8279c_show_frame_gray4(
        dtm1_msb, dtm2_lsb, framebuffer_size, mode);
    /* Restore policy only: set_power_hold(false) would send POWER_OFF now. */
    s_power_hold_enabled = previous_hold;
    return err;
}

esp_err_t epd_uc8279c_show_frame_gray4_a4(
    const uint8_t *dtm1_lsb, const uint8_t *dtm2_msb,
    size_t framebuffer_size)
{
    if (dtm1_lsb == NULL || dtm2_msb == NULL ||
        framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int64_t refresh_start_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "A4 gray49 begin: LSB->DTM1 MSB->DTM2 ready=%d powered=%d",
             s_panel_ready, s_panel_powered);

    /* FreeInk's A4 path has its own electrical recipe and PON-before-seed
     * ordering. Do not reuse panel_session_begin(true), whose 42-byte Factory
     * B assumptions would silently select the wrong LUT framing. */
    esp_err_t err = panel_session_begin_a4_gray49();
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 init");
    }

    err = send_command(0x10);
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 DTM1 command");
    }
    err = write_a4_gray_plane_continuous_cs(dtm1_lsb);
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 DTM1 frame");
    }

    err = send_command(0x13);
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 DTM2 command");
    }
    err = write_a4_gray_plane_continuous_cs(dtm2_msb);
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 DTM2 frame");
    }

    err = send_a4_gray49_lut();
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 LUT");
    }

    ESP_LOGI(TAG, "A4 gray49 trigger refresh");
    err = send_command(0x12);
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 display refresh");
    }
    err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS, "A4 gray49 refresh");
    if (err != ESP_OK) {
        return abort_refresh(err, "A4 gray49 display refresh BUSY");
    }

    /* Match upstream: restore mono DTM1 before returning to DU/GC. */
    ESP_RETURN_ON_ERROR(send_command(0x10), TAG,
                        "A4 gray49 mono DTM1 white command");
    ESP_RETURN_ON_ERROR(write_factory_white_seed(), TAG,
                        "A4 gray49 mono DTM1 white seed");
    s_panel_ready = true;
    s_panel_session_mode = PANEL_SESSION_MONO;
    ESP_RETURN_ON_ERROR(panel_power_off(), TAG,
                        "A4 gray49 mono power off");
    s_panel_powered = false;
    const uint32_t elapsed_ms = (uint32_t)(
        (esp_timer_get_time() - refresh_start_us + 500) / 1000);
    ESP_LOGI(TAG,
             "refresh A4 gray49 complete: total=%lums; DTM1 white re-synced",
             (unsigned long)elapsed_ms);
    return ESP_OK;
}

esp_err_t epd_uc8279c_show_frame_gray3_a4_transition_ex(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    size_t framebuffer_size,
    epd_uc8279c_gray3_refresh_reason_t reason,
    epd_uc8279c_gray3_refresh_result_t *result)
{
    /* 调用即消费，包括参数错误/重复帧提前返回；不能把唤醒资格留给
     * 后续不相干的页面。这里不改变任何光学历史或恢复波形选择。 */
    const reader_wake_black_handoff_t wake_black_handoff =
        s_reader_wake_black_handoff;
    s_reader_wake_black_handoff.pending = false;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (new_lsb == NULL || new_msb == NULL ||
        framebuffer_size != EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const bool old_valid = old_lsb != NULL || old_msb != NULL;
    if (old_valid && (old_lsb == NULL || old_msb == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool parallel_gray =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_PARALLEL_PROBE;
    const bool gray_bank_control =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_BANK_CONTROL;
    const bool gray_sync_probe =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC_PROBE;
    const bool gray_sync = gray_sync_probe ||
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC;
    const bool reader_recovery =
        reason == EPD_UC8279C_GRAY3_REFRESH_READER_RECOVERY;
    const bool gray_aligned_probe =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_ALIGNED_PROBE ||
        gray_sync_probe;
    const bool gray_aligned = gray_aligned_probe || gray_sync;
    const bool gray_return =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_RETURN_PROBE;
    const bool gray_hold =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_HOLD_PROBE;
    const bool timed_hold =
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_TIMED_HOLD_PROBE;
    const bool gray_reverse = gray_hold || timed_hold ||
        reason == EPD_UC8279C_GRAY3_REFRESH_GRAY_REVERSE_PROBE;
    const bool uncalibrated_output = gray_bank_control || gray_aligned_probe ||
        gray_return || gray_reverse;
    const gray3_timing_t gray_timing =
        timed_hold ? GRAY3_TIMING_REVERSE_TIMED_HOLD :
        gray_reverse ? GRAY3_TIMING_REVERSE :
        gray_return ? GRAY3_TIMING_RETURN :
        gray_aligned ? GRAY3_TIMING_ALIGNED :
        gray_bank_control ? GRAY3_TIMING_BLACK_CONTROL :
        reader_recovery ? GRAY3_TIMING_RECOVERY_ALIGNED :
        parallel_gray ? GRAY3_TIMING_ADVANCED : GRAY3_TIMING_BASELINE;
    const bool drive_held_black = parallel_gray || gray_sync || uncalibrated_output ||
        reason == EPD_UC8279C_GRAY3_REFRESH_BLACK_HOLD;
    const bool parallel_black = drive_held_black ||
        reason == EPD_UC8279C_GRAY3_REFRESH_PARALLEL_PROBE;
    if ((unsigned)reason > EPD_UC8279C_GRAY3_REFRESH_READER_RECOVERY ||
        ((gray_aligned || gray_return || reader_recovery) &&
         s_reader_gray3_black_drive_frames <
         (uc8279c_lut_gray4_reader_white_clear[2U * UC8279C_LUT_PHASE_BYTES + 8U] & 0x3fU)) ||
        (parallel_black && !old_valid) ||
        ((parallel_black || reader_recovery) && (
         UC8279C_GRAY3_RETENTION_EXPERIMENT != UC8279C_GRAY3_RETENTION_LEGACY ||
         UC8279C_GRAY3_EVS_EXPERIMENT != UC8279C_GRAY3_EVS_UNTOUCHED))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (UC8279C_GRAY3_EVS_EXPERIMENT != UC8279C_GRAY3_EVS_UNTOUCHED &&
        UC8279C_GRAY3_RETENTION_EXPERIMENT != UC8279C_GRAY3_RETENTION_LEGACY) {
        /* Never combine an unverified EVS experiment with the failed LUT
         * candidates: preserve a one-variable hardware comparison. */
        return ESP_ERR_INVALID_ARG;
    }

    gray3_transition_stats_t stats;
    collect_gray3_transition_stats(old_lsb, old_msb, new_lsb, new_msb,
                                   old_valid, reason, &stats);
    /* 此样张借用白清 bank；若同时清除旧文字/色块就会误用返回灰波形。
     * 只接受 W2G/B2G 和不变像素，且必须在任何 SPI 操作前拒绝其他迁移。 */
    if (gray_return && (stats.pair[0][2] != 0U || stats.pair[1][2] != 0U ||
                        stats.pair[1][0] != 0U || stats.pair[2][0] != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t driven = stats.clear_white + stats.drive_gray +
                            stats.drive_black;
    const unsigned retention_mode =
        reason == EPD_UC8279C_GRAY3_REFRESH_NORMAL && old_valid
            ? UC8279C_GRAY3_RETENTION_EXPERIMENT
            : UC8279C_GRAY3_RETENTION_LEGACY;
    /* B2B may now have an active selector, but that is not a content change.
     * A duplicate NORMAL request must still generate no activation. Use the
     * optical transition matrix, not the number of non-idle selectors. */
    const uint32_t content_changed =
        old_valid ? stats.pair[0][1] + stats.pair[0][2] +
                    stats.pair[1][0] + stats.pair[1][2] +
                    stats.pair[2][0] + stats.pair[2][1]
                  : EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES * 8U;
    if (content_changed == 0U &&
        (reason == EPD_UC8279C_GRAY3_REFRESH_NORMAL || parallel_black)) {
        ESP_LOGI(TAG,
                 "reader gray3 transition skipped: unchanged frame "
                 "pixels=%u retention=%u",
                 (unsigned)(EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES * 8U),
                 retention_mode);
        if (result != NULL) {
            result->skipped = true;
            result->history_committable = old_valid && !uncalibrated_output;
        }
        return ESP_OK;
    }

    const int64_t refresh_start_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "reader gray3 transition begin: reason=%u history=%s "
             "scan_psr0=0x%02x scan_pll=0x%02x ram_rows=%d "
             "idle=%lu white-clear=%lu gray=%lu black=%lu "
             "changed=%lu b2b_drive=%lu retention=%u",
             (unsigned)reason,
             old_valid ? "valid" : "unknown",
             s_scan_psr0, s_scan_pll,
             s_gray3_full_ram_mode ? UC8279C_RAM_HEIGHT
                                   : UC8279C_VISIBLE_HEIGHT,
             (unsigned long)stats.keep_white,
             (unsigned long)stats.clear_white,
             (unsigned long)stats.drive_gray,
             (unsigned long)stats.drive_black,
             (unsigned long)content_changed,
             (unsigned long)(drive_held_black || retention_mode ==
                 UC8279C_GRAY3_RETENTION_TARGET_BLACK ? stats.pair[0][0] : 0U),
             retention_mode);
    ESP_LOGI(TAG,
             "reader gray3 matrix: w2w=%lu w2g=%lu w2b=%lu "
             "g2w=%lu g2g=%lu g2b=%lu b2w=%lu b2g=%lu b2b=%lu; "
             "driven left/right=%lu/%lu top/bottom=%lu/%lu",
             (unsigned long)stats.pair[2][2],
             (unsigned long)stats.pair[2][1],
             (unsigned long)stats.pair[2][0],
             (unsigned long)stats.pair[1][2],
             (unsigned long)stats.pair[1][1],
             (unsigned long)stats.pair[1][0],
             (unsigned long)stats.pair[0][2],
             (unsigned long)stats.pair[0][1],
             (unsigned long)stats.pair[0][0],
             (unsigned long)stats.driven_left,
             (unsigned long)stats.driven_right,
             (unsigned long)stats.driven_top,
             (unsigned long)stats.driven_bottom);

    /* 保留的 GRAY3 可直接复用；MONO 仅允许紧接专用唤醒黑帧的恢复。
     * 两者基础寄存器一致，以下仍完整写入两块 RAM 与恢复 LUT。
     * 深睡、掉电、故障或配置变化仍必须重新初始化。 */
    const int64_t init_start_us = esp_timer_get_time();
    const bool scan_compatible =
        !s_scan_debug_override_active &&
        s_scan_psr0 == UC8279C_PSR_DEFAULT_0 &&
        s_scan_pll == UC8279C_PLL_DEFAULT;
    const bool wake_black_compatible =
        wake_black_handoff.pending && reader_recovery && !old_valid &&
        s_panel_powered && !s_controller_sleeping &&
        s_panel_session_mode == PANEL_SESSION_MONO &&
        wake_black_handoff.session_generation == s_panel_session_generation &&
        wake_black_handoff.profile_generation == s_live_profile_generation &&
        wake_black_handoff.refresh_count == s_display_refresh_count;
    const bool reused_session =
        s_panel_ready &&
        scan_compatible &&
        (s_panel_session_mode == PANEL_SESSION_GRAY3 || wake_black_compatible);
    if (reused_session && wake_black_compatible) {
        ESP_LOGI(TAG, "reader wake black handoff: reuse init; recovery LUT unchanged");
    }
    esp_err_t err = ESP_OK;
    if (!reused_session) {
        err = panel_session_begin(true);
        if (err != ESP_OK) {
            return abort_refresh(err, "reader gray3 transition init");
        }
    }
    const uint32_t init_ms = (uint32_t)(
        (esp_timer_get_time() - init_start_us + 500) / 1000);
    const int64_t power_start_us = esp_timer_get_time();
    err = panel_power_on();
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition power on");
    }
    const uint32_t power_ms = (uint32_t)(
        (esp_timer_get_time() - power_start_us + 500) / 1000);

    const int64_t plane_start_us = esp_timer_get_time();
    uint32_t dtm1_digest = 0;
    uint32_t dtm2_digest = 0;
    int plane_failure_row = -1;
    err = send_command(0x10);
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition DTM1 command");
    }
    err = write_gray3_transition_plane_continuous_cs(
        old_lsb, old_msb, new_lsb, new_msb, old_valid, reason,
        /* DTM1 carries the OLD/high selector bit (bit 1). */
        0x02U, &dtm1_digest, &plane_failure_row);
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition DTM1 frame");
    }

    err = send_command(0x13);
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition DTM2 command");
    }
    err = write_gray3_transition_plane_continuous_cs(
        old_lsb, old_msb, new_lsb, new_msb, old_valid, reason,
        /* DTM2 carries the NEW/low selector bit (bit 0). */
        0x01U, &dtm2_digest, &plane_failure_row);
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition DTM2 frame");
    }
    const uint32_t plane_ms = (uint32_t)(
        (esp_timer_get_time() - plane_start_us + 500) / 1000);
    ESP_LOGI(TAG,
             "reader gray3 planes: rows=%d dtm1=%08lx dtm2=%08lx fail_row=%d",
             s_gray3_full_ram_mode ? UC8279C_RAM_HEIGHT
                                   : UC8279C_VISIBLE_HEIGHT,
             (unsigned long)dtm1_digest, (unsigned long)dtm2_digest,
             plane_failure_row);

    const int64_t lut_start_us = esp_timer_get_time();
    /* Only a real old->white transition asks for the local 24-frame clear.
     * Unknown history keeps the recovered 18-frame baseline, so first paint,
     * wake recovery and any failed-history fallback cannot gain a new
     * full-screen white-clear behavior. */
    const bool boost_white_clear =
        (reason == EPD_UC8279C_GRAY3_REFRESH_NORMAL || parallel_black) && old_valid &&
        stats.clear_white != 0U;
    /* 黑保持与并行对照始终装载相同黑剂量，即使对照帧没有变化黑像素。
     * 保持样张只新增灰点；不能让 B2B 路由同时改变 LUT 而污染对照。 */
    const bool boost_black = stats.drive_black != 0U || parallel_black ||
                             reader_recovery;
    err = send_reader_gray3_transition_lut(boost_white_clear, boost_black,
                                           retention_mode, parallel_black,
                                           gray_timing);
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition LUT");
    }
    const uint32_t lut_ms = (uint32_t)(
        (esp_timer_get_time() - lut_start_us + 500) / 1000);
    const bool evs_lab_enabled =
        UC8279C_GRAY3_EVS_EXPERIMENT != UC8279C_GRAY3_EVS_UNTOUCHED;
    const bool evs_test_float =
        UC8279C_GRAY3_EVS_EXPERIMENT == UC8279C_GRAY3_EVS_FLOAT_0A &&
        reason == EPD_UC8279C_GRAY3_REFRESH_NORMAL && old_valid;
    if (evs_lab_enabled) {
        const uint8_t evs = evs_test_float ? 0x0aU : 0x02U;
        err = send_command_data(0x52, &evs, 1);
        if (err != ESP_OK) {
            return abort_refresh(err, "gray3 EVS lab set");
        }
        ESP_LOGI(TAG, "gray3 EVS LAB: assumed R52=0x%02x, mode=%u; "
                     "UC8279C compatibility unverified",
                 evs, (unsigned)UC8279C_GRAY3_EVS_EXPERIMENT);
    }
    err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                          "reader gray3 transition pre-refresh");
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition pre-refresh BUSY");
    }
    const int64_t wave_start_us = esp_timer_get_time();
    const uint32_t drf_before = s_display_refresh_count;
    err = send_command(0x12);
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition refresh");
    }
    err = wait_until_busy(UC8279C_BUSY_TIMEOUT_MS,
                          "reader gray3 transition refresh assert");
    if (err == ESP_OK) {
        err = wait_until_idle(UC8279C_BUSY_TIMEOUT_MS,
                              "reader gray3 transition refresh release");
    }
    if (err != ESP_OK) {
        return abort_refresh(err, "reader gray3 transition refresh BUSY");
    }
    if (evs_lab_enabled) {
        /* Do not alter EVS while BUSY is active. Restore only after the
         * completed cycle; failure invalidates the session and history. */
        const uint8_t evs_control = 0x02U;
        err = send_command_data(0x52, &evs_control, 1);
        if (err != ESP_OK) {
            return abort_refresh(err, "gray3 EVS lab restore");
        }
    }
    const uint32_t drf_after = s_display_refresh_count;
    if (result != NULL) {
        result->activated = true;
        result->gray_drive_advanced = parallel_gray;
        result->gray_uses_black_bank = gray_bank_control;
        result->gray_duration_aligned = gray_aligned || reader_recovery;
        result->gray_return_uses_white_bank = gray_return;
        result->gray_black_then_white = gray_reverse;
        result->gray_hold_repaint = gray_hold;
        result->gray_hold_timed = timed_hold;
        result->display_refresh_delta = drf_after - drf_before;
        result->applied_profile_id =
            (s_live_profile_generation << 8) |
            (uint32_t)((boost_white_clear ? 1U : 0U) |
                       (boost_black ? 2U : 0U) |
                       (retention_mode << 2) |
                       ((uint32_t)UC8279C_GRAY3_EVS_EXPERIMENT << 4) |
                       (parallel_black ? (1U << 6) : 0U) |
                       (drive_held_black ? (1U << 7) : 0U));
    }
    const uint32_t wave_ms = (uint32_t)(
        (esp_timer_get_time() - wave_start_us + 500) / 1000);

    /* POWER_OFF releases the analog rails but preserves the controller's
     * digital setup and RAM. Keep that GRAY3 session dormant so the next page
     * can PON and reload its session-local LUT without another hardware reset.
     * Any failed command still reaches abort_refresh(), which powers down
     * best-effort and invalidates the whole session. */
    esp_err_t power_off_err = ESP_OK;
    if (!s_power_hold_enabled) {
        power_off_err = panel_power_off();
    }
    if (power_off_err != ESP_OK) {
        return power_off_err;
    }
    s_panel_ready = true;
    s_panel_session_mode = PANEL_SESSION_GRAY3;
    if (result != NULL) {
        /* 对照将灰画黑或灰档尚未校准，不把目标灰图提交为光学历史。 */
        result->history_committable = !uncalibrated_output;
    }
    const uint32_t elapsed_ms = (uint32_t)(
        (esp_timer_get_time() - refresh_start_us + 500) / 1000);
    ESP_LOGI(TAG,
             "reader gray3 transition complete: driven=%lu total=%lu "
             "init=%lums power=%lums planes=%lums lut=%lums wave=%lums "
             "elapsed=%lums; session=gray3-retained reused=%d power=%s",
             (unsigned long)driven,
             (unsigned long)EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES * 8U,
             (unsigned long)init_ms,
             (unsigned long)power_ms,
             (unsigned long)plane_ms,
             (unsigned long)lut_ms,
             (unsigned long)wave_ms,
             (unsigned long)elapsed_ms,
             reused_session,
             s_panel_powered ? "held" : "off");
    return ESP_OK;
}

esp_err_t epd_uc8279c_show_frame_gray3_a4_transition(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    size_t framebuffer_size)
{
    return epd_uc8279c_show_frame_gray3_a4_transition_ex(
        old_lsb, old_msb, new_lsb, new_msb, framebuffer_size,
        EPD_UC8279C_GRAY3_REFRESH_NORMAL, NULL);
}

void epd_uc8279c_set_gray3_full_ram(bool enabled)
{
    s_gray3_full_ram_mode = enabled;
}

bool epd_uc8279c_gray3_full_ram(void)
{
    return s_gray3_full_ram_mode;
}

void epd_uc8279c_set_gray3_black_drive_frames(uint8_t frames)
{
    if (frames == 0U || frames > 0x3fU) {
        return;
    }
    s_reader_gray3_black_drive_frames = frames;
}

uint8_t epd_uc8279c_gray3_black_drive_frames(void)
{
    return s_reader_gray3_black_drive_frames;
}

void epd_uc8279c_set_gray3_reverse_white_frames(uint8_t frames)
{
    if (frames >= 1U && frames <= 18U) s_gray3_reverse_white_frames = frames;
}

uint8_t epd_uc8279c_gray3_reverse_white_frames(void)
{
    return s_gray3_reverse_white_frames;
}

esp_err_t epd_uc8279c_show_frame_gray3_a4(
    const uint8_t *lsb, const uint8_t *msb,
    size_t framebuffer_size)
{
    /* 保留当前阅读器的 00/01/11 主机编码和平面顺序，只切换到
     * d89fc904 后在本机 A4 使用过的完整白清 LUT 事务。Factory B
     * 仍保留为锁屏和诊断基线，不在此入口裁剪它的字段。 */
    ESP_LOGI(TAG, "reader gray3 white-clear: MSB->DTM1 LSB->DTM2");
    return epd_uc8279c_show_frame_gray4(
        msb, lsb, framebuffer_size,
        EPD_UC8279C_GRAY4_READER_WHITE_CLEAR);
}
