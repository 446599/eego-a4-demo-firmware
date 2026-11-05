#include <stdbool.h>
#include <stdio.h>

#include "a4_board.h"
#include "devdemo_app.h"
#include "devdemo_hardware_names.h"
#include "devdemo_ui.h"

enum {
    FRONTLIGHT_COLD = 0,
    FRONTLIGHT_WARM,
    FRONTLIGHT_CHANNEL_COUNT,
    FRONTLIGHT_APPLY_INTERVAL_MS = 100,
};

static bool s_available;
static bool s_a4_pro_power_board;
static uint8_t s_level[FRONTLIGHT_CHANNEL_COUNT];
static int8_t s_active_channel = -1;
static bool s_write_ok = true;
static int64_t s_last_apply_ms;

static devdemo_rect_t slider_band(int channel)
{
    return (devdemo_rect_t){
        .x = DEVDEMO_UI_MARGIN,
        .y = channel == FRONTLIGHT_COLD ? 250 : 390,
        .width = UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN,
        .height = 112,
    };
}

static devdemo_area_t slider_area(int channel)
{
    const devdemo_rect_t band = slider_band(channel);
    return (devdemo_area_t){
        .x = band.x - 4,
        .y = band.y - 4,
        .width = band.width + 8,
        .height = band.height + 8,
    };
}

static void apply_channels(void)
{
    if (!s_available) {
        return;
    }
    s_write_ok =
        a4_board_set_frontlight(s_level[FRONTLIGHT_COLD],
                                s_level[FRONTLIGHT_WARM]);
}

static void set_channel_from_x(devdemo_ctx_t *ctx, int channel)
{
    const int percent = devdemo_ui_slider_percent_at(
        slider_band(channel), ctx->touch_x);
    if (s_level[channel] == percent) {
        return;
    }
    s_level[channel] = (uint8_t)percent;
    apply_channels();
}

static void frontlight_enter(devdemo_ctx_t *ctx)
{
    (void)ctx;
    s_available = a4_board_has_frontlight();
    s_a4_pro_power_board = a4_board_sd_power_can_suspend();
    s_active_channel = -1;
    s_write_ok = true;
}

static void frontlight_exit(devdemo_ctx_t *ctx)
{
    (void)ctx;
    s_active_channel = -1;
}

static void frontlight_render(devdemo_ctx_t *ctx)
{
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Frontlight",
                      "LM3630A dual-channel control @ I2C 0x36");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "HARDWARE");
    y = devdemo_ui_row(
        ctx->fb, y, "Model",
        devdemo_product_name(s_available, s_a4_pro_power_board));
    y = devdemo_ui_row(ctx->fb, y, "Controller",
                       s_available ? "PRESENT" : "NOT FITTED");
    (void)devdemo_ui_row(ctx->fb, y, "Last write",
                         !s_available ? "DISABLED"
                                      : (s_write_ok ? "OK" : "FAILED"));

    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, 222,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        s_available
            ? "Drag each track independently; values are sent live."
            : "No LM3630A controller answered at address 0x36.",
        true);

    devdemo_ui_slider(ctx->fb, slider_band(FRONTLIGHT_COLD), "COLD",
                      s_level[FRONTLIGHT_COLD],
                      s_active_channel == FRONTLIGHT_COLD);
    devdemo_ui_slider(ctx->fb, slider_band(FRONTLIGHT_WARM), "WARM",
                      s_level[FRONTLIGHT_WARM],
                      s_active_channel == FRONTLIGHT_WARM);

    char line[64];
    (void)snprintf(line, sizeof(line), "CALL cold=%u warm=%u",
                   (unsigned)s_level[FRONTLIGHT_COLD],
                   (unsigned)s_level[FRONTLIGHT_WARM]);
    devdemo_ui_text_fit(ctx->fb, DEVDEMO_UI_MARGIN, 520,
                        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
                        line, true);
    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, 558,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "Drag updates about every 100 ms; lift settles the full page.", true);
}

static devdemo_area_t frontlight_area_hint(devdemo_ctx_t *ctx)
{
    (void)ctx;
    return slider_area(s_active_channel >= 0 ? s_active_channel
                                             : FRONTLIGHT_COLD);
}

static devdemo_redraw_t frontlight_touch(devdemo_ctx_t *ctx)
{
    if (!s_available) {
        return DEVDEMO_REDRAW_NONE;
    }
    if (ctx->pressed) {
        for (int channel = 0; channel < FRONTLIGHT_CHANNEL_COUNT; ++channel) {
            const devdemo_rect_t band = slider_band(channel);
            if (devdemo_ui_rect_hit(band, ctx->touch_x, ctx->touch_y)) {
                s_active_channel = (int8_t)channel;
                set_channel_from_x(ctx, channel);
                s_last_apply_ms = ctx->now_ms;
                return DEVDEMO_REDRAW_PAGE;
            }
        }
    } else if (ctx->released && s_active_channel >= 0) {
        set_channel_from_x(ctx, s_active_channel);
        s_active_channel = -1;
        s_last_apply_ms = 0;
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t frontlight_tick(devdemo_ctx_t *ctx)
{
    if (!s_available || !ctx->touching || s_active_channel < 0) {
        return DEVDEMO_REDRAW_NONE;
    }
    const int percent = devdemo_ui_slider_percent_at(
        slider_band(s_active_channel), ctx->touch_x);
    if (s_level[s_active_channel] == percent ||
        ctx->now_ms - s_last_apply_ms < FRONTLIGHT_APPLY_INTERVAL_MS) {
        return DEVDEMO_REDRAW_NONE;
    }
    set_channel_from_x(ctx, s_active_channel);
    s_last_apply_ms = ctx->now_ms;
    return DEVDEMO_REDRAW_AREA;
}

const devdemo_app_t devdemo_app_frontlight = {
    .title = "Frontlight",
    .detail = "Dual cold/warm channel sliders",
    .render = frontlight_render,
    .area_hint = frontlight_area_hint,
    .on_enter = frontlight_enter,
    .on_exit = frontlight_exit,
    .on_touch = frontlight_touch,
    .on_tick = frontlight_tick,
};
