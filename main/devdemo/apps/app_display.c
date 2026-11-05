#include <inttypes.h>
#include <stdio.h>

#include "devdemo_app.h"
#include "devdemo_ui.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "devdemo_display";

enum {
    DISPLAY_ACTION_GC = 0,
    DISPLAY_ACTION_FAST,
    DISPLAY_ACTION_PATTERN,
    DISPLAY_ACTION_COUNT,
};

static uint32_t s_timing_ms[DISPLAY_ACTION_COUNT];
static bool s_pattern;

static devdemo_rect_t action_rect(int action)
{
    return devdemo_ui_bar_rect(action, DISPLAY_ACTION_COUNT);
}

static void draw_pattern(devdemo_ctx_t *ctx)
{
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Display Pattern",
                      "GC test pattern; touch anywhere to return");
    devdemo_ui_checker(
        ctx->fb,
        (devdemo_rect_t){DEVDEMO_UI_MARGIN, 118,
                         UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 220},
        10);
    int y = 360;
    for (int index = 0; index < 16; ++index) {
        const int x = DEVDEMO_UI_MARGIN + index * 32;
        devdemo_ui_fill_rect(
            ctx->fb,
            (devdemo_rect_t){x, y, 32, 120}, (index & 1) == 0);
    }
    y += 140;
    for (int index = 0; index < 20; ++index) {
        devdemo_ui_fill_rect(
            ctx->fb,
            (devdemo_rect_t){DEVDEMO_UI_MARGIN, y + index * 6,
                             UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 1},
            true);
    }
    devdemo_ui_text(ctx->fb, DEVDEMO_UI_MARGIN, 566, 28,
                    "0123456789 TEXT SHARPNESS 0123456789", true);
}

static void display_exit(devdemo_ctx_t *ctx)
{
    (void)ctx;
    s_pattern = false;
}

static void display_render(devdemo_ctx_t *ctx)
{
    if (s_pattern) {
        draw_pattern(ctx);
        return;
    }

    char line[64];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "EPD Refresh",
                      "GC, DU fast refresh and measured timing");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "REFRESH STATE");
    (void)snprintf(line, sizeof(line), "%" PRIu32, ctx->gc_refresh_count);
    y = devdemo_ui_row(ctx->fb, y, "GC count", line);
    (void)snprintf(line, sizeof(line), "%" PRIu32,
                   ctx->fast_refresh_count);
    y = devdemo_ui_row(ctx->fb, y, "Fast count", line);
    (void)snprintf(line, sizeof(line), "%" PRIu32 " ms",
                   ctx->last_refresh_ms);
    y = devdemo_ui_row(ctx->fb, y, "Last refresh", line);

    y = devdemo_ui_section(ctx->fb, y + 18, "BENCH TIMING");
    (void)snprintf(line, sizeof(line), "%" PRIu32 " ms", s_timing_ms[0]);
    y = devdemo_ui_row(ctx->fb, y, "Full GC", line);
    (void)snprintf(line, sizeof(line), "%" PRIu32 " ms", s_timing_ms[1]);
    y = devdemo_ui_row(ctx->fb, y, "DU fast", line);
    y = devdemo_ui_row(ctx->fb, y, "Panel session",
                       "power retained between frames");

    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, y + 16,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "GC driver: full-pixel maintenance. FAST: DU low-flash update.",
        true);
    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, y + 54,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "The pattern remains until the next touch.", true);

    devdemo_ui_button(ctx->fb, action_rect(0), "GC", false);
    devdemo_ui_button(ctx->fb, action_rect(1), "FAST", false);
    devdemo_ui_button(ctx->fb, action_rect(2), "PATTERN", false);
}

static devdemo_redraw_t display_touch(devdemo_ctx_t *ctx)
{
    if (s_pattern) {
        s_pattern = false;
        return DEVDEMO_REDRAW_PAGE;
    }
    for (int action = 0; action < DISPLAY_ACTION_COUNT; ++action) {
        if (!devdemo_ui_rect_hit(action_rect(action), ctx->touch_x,
                                 ctx->touch_y)) {
            continue;
        }
        if (action == DISPLAY_ACTION_PATTERN) {
            s_pattern = true;
            return DEVDEMO_REDRAW_FULL;
        }
        const bool full = action == DISPLAY_ACTION_GC;
        const int64_t started_us = esp_timer_get_time();
        devdemo_present(ctx, ctx->current,
                        full ? DEVDEMO_REDRAW_FULL : DEVDEMO_REDRAW_FAST);
        s_timing_ms[action] =
            (uint32_t)((esp_timer_get_time() - started_us + 500) / 1000);
        ESP_LOGI(TAG, "%s action took %" PRIu32 " ms",
                 full ? "GC" : "FAST", s_timing_ms[action]);
        return DEVDEMO_REDRAW_DONE;
    }
    return DEVDEMO_REDRAW_NONE;
}

const devdemo_app_t devdemo_app_display = {
    .title = "Display / Refresh",
    .detail = "GC, DU, timing and checker pattern",
    .enter_full = true,
    .on_exit = display_exit,
    .render = display_render,
    .on_touch = display_touch,
};
