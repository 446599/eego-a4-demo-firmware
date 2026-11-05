#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lockscreen only: two half-duration GC drives of an all-black frame. */
esp_err_t epd_uc8279c_show_lockscreen_double_black(const uint8_t *framebuffer,
                                                size_t framebuffer_size);

/* 阅读锁屏唤醒专用：原强度单次 GC 全黑。成功且高压保持时，仅允许
 * 紧接着的未知历史 READER_RECOVERY 复用初始化；仍重写两平面及 LUT。
 * 资格不跨刷新、配置/会话代次变化、POF、深睡或错误。仅显示线程调用。 */
esp_err_t epd_uc8279c_show_reader_wake_black(const uint8_t *framebuffer,
                                            size_t framebuffer_size);

/* Final geometry confirmed on the physical A4 window.  UI rendering must be
 * clipped to the inner edge of the R56 / 8px calibration ring; the outer
 * pixels are partially covered by the housing. */
enum {
    EPD_UC8279C_WIDTH_PX = 768,
    EPD_UC8279C_VISIBLE_HEIGHT_PX = 552,
    EPD_UC8279C_FRAMEBUFFER_STRIDE_BYTES = EPD_UC8279C_WIDTH_PX / 8,
    EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES =
        EPD_UC8279C_FRAMEBUFFER_STRIDE_BYTES * EPD_UC8279C_VISIBLE_HEIGHT_PX,
    EPD_UC8279C_VISIBLE_CORNER_RADIUS_PX = 56,
    EPD_UC8279C_UI_CLIP_INSET_PX = 8,
    EPD_UC8279C_UI_CLIP_RADIUS_PX = 48,
    EPD_UC8279C_KEY_CONTENT_INSET_PX = 56,
};

typedef struct {
    spi_host_device_t host;
    int pin_sclk;
    int pin_mosi;
    int pin_cs;
    int pin_dc;
    int pin_reset;
    int pin_busy;
    int clock_hz;
} epd_uc8279c_config_t;

/* Called between BUSY samples while a frame refresh is in progress. The
 * callback must be short and must not submit another EPD operation. */
typedef void (*epd_uc8279c_busy_wait_hook_t)(void *context);

typedef enum {
    /* 旧版增量基线，保留给回归对照；相同帧可跳过。 */
    EPD_UC8279C_GRAY3_REFRESH_NORMAL = 0,
    /* First paint or a known-unknown optical state. Do not trust old RAM. */
    EPD_UC8279C_GRAY3_REFRESH_RECOVERY,
    /* Explicit maintenance pass. Identical frames must not be silently skipped. */
    EPD_UC8279C_GRAY3_REFRESH_MAINTENANCE,
    /* 仅诊断：将变化黑像素的驱黑段前移，与白清段重叠。
     * 必须提供可信旧帧，不能与 retention/EVS 实验叠加。
     * 不改变 B2B 的 hold 选择，不代表已修复黑保持或完成光学校准。 */
    EPD_UC8279C_GRAY3_REFRESH_PARALLEL_PROBE,
    /* 阅读黑保持：B2B 与新黑共用前移的黑向驱动，白/灰保持仍选 00。
     * 必须提供可信旧帧；未知历史由调用方选择 RECOVERY。
     * 相同帧跳过，不与 retention/EVS 实验叠加。值 4 与已验证样张一致。 */
    EPD_UC8279C_GRAY3_REFRESH_BLACK_HOLD,
    /* 仅诊断：保留 BLACK_HOLD，把灰分支的 5 帧黑向段前移。
     * 白/黑起点到灰的光学结果可能不同，尤其 B2G 尚未校准；阅读不得选用。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_PARALLEL_PROBE,
    /* 仅单步诊断：灰 bank 完整复制当前黑 bank，目标灰预期变黑。
     * 用于核对分支映射和显现先后；成功也不可提交目标帧为灰度历史。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_BANK_CONTROL,
    /* 仅单步诊断：从同波形对照减小灰驱动，在同记录补足代码 00 时长。
     * 字段时长及中灰响应仍待实测，成功也不可提交为可信灰度历史。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_ALIGNED_PROBE,
    /* 仅诊断：W2G 继续使用短灰 bank，B2G 改用独立白反向+短灰 bank。
     * 只用于验证旧黑转灰，不能接入阅读。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_RETURN_PROBE,
    /* 仅单步诊断：灰 bank 先完整驱黑，再执行短白反向段。
     * 默认计数 2 相对同波形黑对照只改变第二记录首槽电平；
     * 诊断剂量可配置，不提交光学历史。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_REVERSE_PROBE,
    /* 仅单步诊断：沿用先黑后短白波形，仅将 G2G 从 idle 改走灰 bank。
     * 用于隔离固定灰变浅的保持分支；不提交光学历史。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_HOLD_PROBE,
    /* 仅单步诊断：保持 reason=9 路由，仅将空 hold bank 换成与黑 bank
     * 等时的代码 00 记录；不假定零电场，不提交光学历史。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_TIMED_HOLD_PROBE,
    /* 黑灰同步对照：沿用已验证的黑灰同步选择器，便于逐步实测。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC_PROBE,
    /* 阅读生产用黑灰同步：需要可信旧光学帧，未知历史走 READER_RECOVERY。 */
    EPD_UC8279C_GRAY3_REFRESH_GRAY_SYNC,
    /* 阅读首帧独立恢复：不信任旧图，保留白清前缀；灰 bank 在驱黑后
     * 补齐与黑 bank 等长的代码 00 记录，不增加黑向剂量。
     * 原 RECOVERY 保留作基线对照；墨色一致性仍需面板实测。 */
    EPD_UC8279C_GRAY3_REFRESH_READER_RECOVERY,
} epd_uc8279c_gray3_refresh_reason_t;

typedef struct {
    bool activated;
    bool skipped;
    bool history_committable;
    bool gray_drive_advanced;
    bool gray_uses_black_bank;
    bool gray_duration_aligned;
    bool gray_return_uses_white_bank;
    bool gray_black_then_white;
    bool gray_hold_repaint;
    bool gray_hold_timed;
    uint32_t display_refresh_delta;
    uint32_t applied_profile_id;
} epd_uc8279c_gray3_refresh_result_t;

typedef enum {
    /* Names follow the three waveform modes found in this device's factory
     * app. Their visual cadence is intentionally not inferred. */
    EPD_UC8279C_GRAY4_FACTORY_A = 0,
    EPD_UC8279C_GRAY4_FACTORY_B,
    EPD_UC8279C_GRAY4_FACTORY_C,
    EPD_UC8279C_GRAY4_FACTORY_B_GC2,
    /* Public 5x42-byte four-gray waveform from Waveshare EPD_4in2.c. */
    EPD_UC8279C_GRAY4_WAVESHARE_4IN2,
    /* Recovered reader waveform: white clear followed by gray/black drive. */
    EPD_UC8279C_GRAY4_READER_WHITE_CLEAR,
} epd_uc8279c_gray4_mode_t;

/* UC8279C PSR gate-scan direction.  These names describe the controller's
 * logical Gate order only; the corresponding physical top/bottom or
 * left/right direction on the mounted A4 glass must be measured.  The
 * default production PSR is 0x3f (UD=1); REVERSE is the same value with
 * UD cleared (0x37). */
typedef enum {
    EPD_UC8279C_GATE_FORWARD = 0,
    EPD_UC8279C_GATE_REVERSE,
} epd_uc8279c_gate_direction_t;

typedef struct {
    epd_uc8279c_gate_direction_t direction;
    uint8_t pll;
    bool hold_power;
    /* Optional caller-owned LUT image.  Pass NULL to use the selected DU
     * profile.  The image is consumed synchronously before DISPLAY_REFRESH;
     * it is never read while BUSY is low. */
    const uint8_t *lut;
    size_t lut_size;
    size_t lut_bank_size; /* 42 or 49 bytes, five banks in hardware order */
    uint8_t lut_cdi;      /* register 0x50 value paired with the image */
} epd_uc8279c_gate_probe_config_t;

typedef struct {
    esp_err_t err;
    uint32_t display_refresh_delta;
    int64_t busy_wait_us;
    int64_t elapsed_us;
} epd_uc8279c_gate_probe_result_t;

/* Waveform selection held by the driver. Application persistence is opt-in and
 * restores a validated profile before the first normal panel refresh. */
typedef enum {
    EPD_UC8279C_GC_LUT_FACTORY_49 = 0,
    EPD_UC8279C_GC_LUT_CUSTOM_V1_49,
    EPD_UC8279C_GC_LUT_XIAODOUZI_49,
    EPD_UC8279C_GC_LUT_A4_FACTORY_42,
} epd_uc8279c_gc_lut_t;

typedef enum {
    EPD_UC8279C_DU_LUT_FACTORY_49 = 0,
    EPD_UC8279C_DU_LUT_XIAODOUZI_FAST_49,
    EPD_UC8279C_DU_LUT_XIAODOUZI_REFERENCE_49,
    EPD_UC8279C_DU_LUT_A4_FACTORY_42,
    EPD_UC8279C_DU_LUT_A4_NO_KK_42,
    EPD_UC8279C_DU_LUT_A4_BLACK_BOOST_42,
    EPD_UC8279C_DU_LUT_FACTORY_BLACK_DURATION_49,
    EPD_UC8279C_DU_LUT_FACTORY_BLACK_REPEAT_49,
} epd_uc8279c_du_lut_t;

typedef struct {
    uint8_t power_setting[5];       /* register 0x01, exactly five bytes */
    uint8_t booster[4];             /* register 0x06, exactly 3 or 4 bytes */
    size_t booster_len;
    uint8_t gc_cdi;                 /* register 0x50 paired with GC LUT */
    uint8_t du_cdi;                 /* register 0x50 paired with DU LUT */
    epd_uc8279c_gc_lut_t gc_lut;
    epd_uc8279c_du_lut_t du_lut;
} epd_uc8279c_live_profile_t;

typedef struct {
    esp_err_t err;
    int64_t busy_wait_us;
    int64_t elapsed_us;
    uint32_t generation;
} epd_uc8279c_live_profile_result_t;

esp_err_t epd_uc8279c_init(const epd_uc8279c_config_t *config);
void epd_uc8279c_live_profile_default(epd_uc8279c_live_profile_t *profile);
/* Select a validated production profile without touching panel hardware. */
esp_err_t epd_uc8279c_set_live_profile(const epd_uc8279c_live_profile_t *profile);
/* Apply a runtime profile after a controller reset. When old_framebuffer and
 * target_framebuffer are both supplied, the reset session is seeded with the
 * actual displayed old frame and performs that transition exactly once. Pass
 * both frame pointers as NULL for the legacy white calibration refresh.
 * hold_power applies only to this diagnostic operation and never mutates the
 * global production power-hold policy. */
esp_err_t epd_uc8279c_apply_live_profile(
    const epd_uc8279c_live_profile_t *profile, bool use_du,
    const uint8_t *old_framebuffer, const uint8_t *target_framebuffer,
    size_t framebuffer_size, bool hold_power,
    epd_uc8279c_live_profile_result_t *result);
esp_err_t epd_uc8279c_restore_stable_profile(
    epd_uc8279c_live_profile_result_t *result);
void epd_uc8279c_get_live_profile(epd_uc8279c_live_profile_t *profile);
uint32_t epd_uc8279c_live_profile_generation(void);
void epd_uc8279c_set_busy_wait_hook(epd_uc8279c_busy_wait_hook_t hook,
                                    void *context);
esp_err_t epd_uc8279c_show_test_pattern(void);

/* Number of DISPLAY_REFRESH (0x12) command transactions sent since driver
 * init.  This is a bus-level diagnostic counter: it counts real 0x12
 * submissions, including the two intentional activations in the gray4 GC2
 * diagnostic path. */
uint32_t epd_uc8279c_display_refresh_count(void);
void epd_uc8279c_reset_display_refresh_count(void);

/* Phase-1/2 diagnostics. These setters are deliberately session-local:
 * they only write a powered, idle controller and never alter the production
 * reset defaults used by the next normal session. */
esp_err_t epd_uc8279c_set_gate_direction(
    epd_uc8279c_gate_direction_t direction);
esp_err_t epd_uc8279c_set_pll(uint8_t pll);
esp_err_t epd_uc8279c_restore_scan_defaults(void);

/* Preload a caller-supplied five-bank LUT while the panel is idle. This is
 * the supported form of "实时传入 LUT": the host may stream a new waveform
 * immediately before a refresh, but the controller does not accept reliable
 * LUT writes during an active DRF/BUSY interval. The function sends no DRF. */
esp_err_t epd_uc8279c_load_custom_lut(const uint8_t *lut, size_t lut_size,
                                      size_t lut_bank_size, uint8_t lut_cdi);

/* Point a Gate-probe config at the timing-only single-frame waveform derived
 * from the device-proven factory DU table and pair it with the current live
 * DU CDI. The helper performs no I/O; the LUT is transmitted synchronously
 * by run_gate_probe() before its sole DRF.
 * Callers may instead fill the same four LUT fields with a runtime-provided
 * five-bank image when testing a host-generated waveform. */
esp_err_t epd_uc8279c_gate_probe_use_single_frame_lut(
    epd_uc8279c_gate_probe_config_t *config);

/* Execute one old->new transition with the selected PSR gate direction and
 * PLL. This is a diagnostic probe for physical direction/timing measurement,
 * not the final animation LUT: it uses the currently selected DU waveform,
 * sends exactly one 0x12, waits for BUSY, synchronizes both image planes
 * without another refresh, and then restores PSR/PLL defaults. */
esp_err_t epd_uc8279c_run_gate_probe(
    const uint8_t *old_framebuffer, const uint8_t *next_framebuffer,
    size_t framebuffer_size,
    const epd_uc8279c_gate_probe_config_t *config,
    epd_uc8279c_gate_probe_result_t *result);

/* Framebuffer rows are stored top-to-bottom.  Pixels are MSB-first within
 * each byte, 1=white and 0=black. */
esp_err_t epd_uc8279c_show_frame(const uint8_t *framebuffer, size_t framebuffer_size);

/* DU-waveform fast refresh: low flash, slight ghosting accumulation.
 * Interleave with periodic epd_uc8279c_show_frame() GC passes. */
esp_err_t epd_uc8279c_show_frame_fast(const uint8_t *framebuffer, size_t framebuffer_size);

/* DU partial refresh of a byte-aligned panel-raster rectangle. The full
 * framebuffer supplies the source rows; the controller's old-image plane is
 * updated for the same window after the waveform completes. */
esp_err_t epd_uc8279c_show_frame_fast_partial(
    const uint8_t *framebuffer, size_t framebuffer_size,
    int x, int y, int width, int height);

/* Factory-derived four-gray full refresh. Each argument is a normal 1bpp
 * panel framebuffer (MSB-first, 1=white) representing a DTM plane:
 * dtm1_msb is sent to 0x10 and dtm2_lsb is sent to 0x13. The two plane bits
 * encode black=00, dark gray=01, light gray=10, white=11. This operation
 * invalidates the 1bpp session after completion; the following regular UI
 * frame will reinitialize and use GC. */
esp_err_t epd_uc8279c_show_frame_gray4(
    const uint8_t *dtm1_msb, const uint8_t *dtm2_lsb,
    size_t framebuffer_size, epd_uc8279c_gray4_mode_t mode);

/* Preview transaction: retain HV after refresh without changing the global
 * retention policy. Serialized through display_manager like other refreshes;
 * explicit power-off, sleep and refresh-error cleanup still release rails. */
esp_err_t epd_uc8279c_show_frame_gray4_retained(
    const uint8_t *dtm1_msb, const uint8_t *dtm2_lsb,
    size_t framebuffer_size, epd_uc8279c_gray4_mode_t mode);

/* 阅读三灰入口，保留调用方 LSB、MSB 参数顺序，主机编码为
 * black=00, middle=01 and white=11。内部使用历史实机验证过的白清
 * 快速灰阶波形：MSB->DTM1、LSB->DTM2。保持电源开启时复用灰阶会话；
 * 单色历史仍由调用方失效，掉电/深睡/错误路径会强制重建会话。
 * 中灰亮度、字边和连续翻页残影仍需实机观察。 */
esp_err_t epd_uc8279c_show_frame_gray3_a4(
    const uint8_t *lsb, const uint8_t *msb,
    size_t framebuffer_size);

/* Three-gray reader transition with a software optical-history pair. The
 * caller supplies the previous successful reader frame and the new target;
 * the driver derives KEEP/WHITE/GRAY/BLACK transition classes and performs
 * one activation. Passing old_lsb/old_msb as NULL requests the full baseline
 * transaction used when optical history is unknown. */
esp_err_t epd_uc8279c_show_frame_gray3_a4_transition(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    size_t framebuffer_size);

/* Extended reader transition API. The old wrapper above is equivalent to
 * reason=NORMAL and result=NULL.  RECOVERY ignores the supplied old image;
 * MAINTENANCE also ignores it and must never be skipped as an identical
 * frame. READER_RECOVERY 同样忽略旧图并强制激活，但使用独立的首帧
 * 灰/黑时长对齐表，不改变原 RECOVERY 的基线字节。 */
esp_err_t epd_uc8279c_show_frame_gray3_a4_transition_ex(
    const uint8_t *old_lsb, const uint8_t *old_msb,
    const uint8_t *new_lsb, const uint8_t *new_msb,
    size_t framebuffer_size,
    epd_uc8279c_gray3_refresh_reason_t reason,
    epd_uc8279c_gray3_refresh_result_t *result);

/* Diagnostic RAM length control for the three-gray selector path.
 * false: 552 visible rows only (current production behavior).
 * true:  552 visible rows followed by 48 selector-00 tail rows. */
void epd_uc8279c_set_gray3_full_ram(bool enabled);
bool epd_uc8279c_gray3_full_ram(void);

/* Diagnostic black-drive duration for the three-gray BLACK selector bank.
 * The default is the production 18-frame value.  Values are raw low-six-bit
 * waveform fields, so callers must keep 1..63. */
void epd_uc8279c_set_gray3_black_drive_frames(uint8_t frames);
uint8_t epd_uc8279c_gray3_black_drive_frames(void);
/* 仅 reason=9 的白反向原始计数，1..18；记录总计数保持 18，默认 2。
 * 不改变 BLACK_HOLD、恢复或其他诊断的 LUT。 */
void epd_uc8279c_set_gray3_reverse_white_frames(uint8_t frames);
uint8_t epd_uc8279c_gray3_reverse_white_frames(void);

/* 原始 49 字节参考事务，仅供驱动对照，不用于阅读抗锯齿。
 * 参数和硬件顺序仍为 LSB->DTM1、MSB->DTM2。实机曾出现约 80ms
 * BUSY 且无可见更新；不能据函数成功返回宣称有效灰阶。 */
esp_err_t epd_uc8279c_show_frame_gray4_a4(
    const uint8_t *dtm1_lsb, const uint8_t *dtm2_msb,
    size_t framebuffer_size);

/* Panel-power retention policy. When enabled, a successful refresh leaves
 * the UC8279C high-voltage rails on instead of issuing POWER_OFF (0x02), so
 * the next refresh can reuse the powered session. Disabling the policy powers
 * the rails down immediately when they are currently on.
 *
 * Retaining power raises idle consumption, but can keep the visible panel
 * background stable between refreshes. Refresh-failure and deep-sleep paths
 * always power the rails down regardless of this setting. Call only when no
 * refresh or sleep call is active. */
esp_err_t epd_uc8279c_set_power_hold(bool enabled);
bool epd_uc8279c_power_hold_enabled(void);

/* Turn off only the UC8279C high-voltage rails with POF (0x02). The
 * controller session and register/SRAM state remain available for the next
 * refresh; this does not send deep sleep or change the retention policy. */
esp_err_t epd_uc8279c_power_off(void);

/* Deep-sleep the controller (loses old-image RAM; next refresh re-inits). */
esp_err_t epd_uc8279c_sleep(void);

/* Force a controller cold session after the rails have been powered down. */
esp_err_t epd_uc8279c_cold_reset(void);

#ifdef __cplusplus
}
#endif
