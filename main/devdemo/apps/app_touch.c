#include <stdbool.h>
#include <stdio.h>

#include "devdemo_app.h"
#include "devdemo_ui.h"

enum {
    TOUCH_TRAIL_MAX = 24,
    TOUCH_MOTION_INTERVAL_MS = 80,
};

static const devdemo_rect_t s_map = {
    .x = 144,
    .y = 320,
    .width = 264,
    .height = 368,
};

static int16_t s_trail_x[TOUCH_TRAIL_MAX];
static int16_t s_trail_y[TOUCH_TRAIL_MAX];
static uint8_t s_trail_count;
static uint8_t s_trail_next;
static uint16_t s_last_raw_x;
static uint16_t s_last_raw_y;
static int16_t s_last_touch_x;
static int16_t s_last_touch_y;
static int64_t s_last_motion_ms;

static void reset_trail(void)
{
    s_trail_count = 0;
    s_trail_next = 0;
}

static void append_trail(devdemo_ctx_t *ctx)
{
    if (s_trail_count > 0) {
        const uint8_t last =
            (uint8_t)((s_trail_next + TOUCH_TRAIL_MAX - 1U) %
                      TOUCH_TRAIL_MAX);
        if (s_trail_x[last] == ctx->touch_x &&
            s_trail_y[last] == ctx->touch_y) {
            return;
        }
    }
    s_trail_x[s_trail_next] = ctx->touch_x;
    s_trail_y[s_trail_next] = ctx->touch_y;
    s_trail_next =
        (uint8_t)((s_trail_next + 1U) % TOUCH_TRAIL_MAX);
    if (s_trail_count < TOUCH_TRAIL_MAX) {
        ++s_trail_count;
    }
}

static void map_point(uint16_t x, uint16_t y, int *out_x, int *out_y)
{
    if (x >= UI_CANVAS_WIDTH_PX) {
        x = UI_CANVAS_WIDTH_PX - 1;
    }
    if (y >= UI_CANVAS_HEIGHT_PX) {
        y = UI_CANVAS_HEIGHT_PX - 1;
    }
    *out_x =
        s_map.x + (int)x * (s_map.width - 1) /
                      (UI_CANVAS_WIDTH_PX - 1);
    *out_y =
        s_map.y + (int)y * (s_map.height - 1) /
                      (UI_CANVAS_HEIGHT_PX - 1);
}

static void draw_touch_map(devdemo_ctx_t *ctx)
{
    devdemo_ui_stroke_rect(ctx->fb, s_map, 2, true);
    for (int grid = 1; grid < 4; ++grid) {
        const int x = s_map.x + s_map.width * grid / 4;
        const int y = s_map.y + s_map.height * grid / 4;
        devdemo_ui_line(ctx->fb, x, s_map.y, x,
                        s_map.y + s_map.height, true);
        devdemo_ui_line(ctx->fb, s_map.x, y,
                        s_map.x + s_map.width, y, true);
    }

    int previous_x = 0;
    int previous_y = 0;
    for (uint8_t index = 0; index < s_trail_count; ++index) {
        const uint8_t slot =
            (uint8_t)((s_trail_next + TOUCH_TRAIL_MAX - s_trail_count +
                       index) %
                      TOUCH_TRAIL_MAX);
        int x;
        int y;
        map_point((uint16_t)s_trail_x[slot],
                  (uint16_t)s_trail_y[slot], &x, &y);
        if (index > 0) {
            devdemo_ui_line(ctx->fb, previous_x, previous_y, x, y, true);
        }
        devdemo_ui_fill_rect(
            ctx->fb, (devdemo_rect_t){x - 2, y - 2, 5, 5}, true);
        previous_x = x;
        previous_y = y;
    }

    if (ctx->touching) {
        int x;
        int y;
        map_point(ctx->raw_x, ctx->raw_y, &x, &y);
        devdemo_ui_line(ctx->fb, x - 12, y, x + 12, y, true);
        devdemo_ui_line(ctx->fb, x, y - 12, x, y + 12, true);
    }
}

static void touch_render(devdemo_ctx_t *ctx)
{
    char line[64];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Touch",
                      "GSL1680 @ 0x40, click-and-drag for local DU update");

    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "LIVE SAMPLE");
    (void)snprintf(line, sizeof(line), "%u", (unsigned)ctx->fingers);
    y = devdemo_ui_row(ctx->fb, y, "Fingers", line);
    (void)snprintf(line, sizeof(line), "%u, %u", (unsigned)ctx->raw_x,
                   (unsigned)ctx->raw_y);
    y = devdemo_ui_row(ctx->fb, y, "Raw", line);
    (void)snprintf(line, sizeof(line), "%d, %d", (int)ctx->touch_x,
                   (int)ctx->touch_y);
    y = devdemo_ui_row(ctx->fb, y, "Mapped", line);
    (void)snprintf(line, sizeof(line), "%lu",
                   (unsigned long)ctx->touch_taps);
    y = devdemo_ui_row(ctx->fb, y, "Tap count", line);
    (void)devdemo_ui_row(ctx->fb, y, "State",
                         ctx->touching ? "TOUCHING" : "RELEASED");

    draw_touch_map(ctx);
    devdemo_ui_button(ctx->fb, devdemo_ui_bar_rect(0, 1), "CLEAR TRAIL", false);
}

static devdemo_area_t touch_area_hint(devdemo_ctx_t *ctx)
{
    (void)ctx;
    return (devdemo_area_t){
        .x = s_map.x - 2,
        .y = s_map.y - 2,
        .width = s_map.width + 4,
        .height = s_map.height + 4,
    };
}

static devdemo_redraw_t touch_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        reset_trail();
        return DEVDEMO_REDRAW_PAGE;
    }
    if (ctx->pressed) {
        reset_trail();
        append_trail(ctx);
        s_last_raw_x = ctx->raw_x;
        s_last_raw_y = ctx->raw_y;
        s_last_touch_x = ctx->touch_x;
        s_last_touch_y = ctx->touch_y;
        s_last_motion_ms = ctx->now_ms;
        return DEVDEMO_REDRAW_PAGE;
    }
    if (ctx->released) {
        append_trail(ctx);
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t touch_tick(devdemo_ctx_t *ctx)
{
    if (!ctx->touching) {
        return DEVDEMO_REDRAW_NONE;
    }
    if (ctx->raw_x == s_last_raw_x && ctx->raw_y == s_last_raw_y &&
        ctx->touch_x == s_last_touch_x && ctx->touch_y == s_last_touch_y) {
        return DEVDEMO_REDRAW_NONE;
    }
    if (ctx->now_ms - s_last_motion_ms < TOUCH_MOTION_INTERVAL_MS) {
        return DEVDEMO_REDRAW_NONE;
    }
    s_last_raw_x = ctx->raw_x;
    s_last_raw_y = ctx->raw_y;
    s_last_touch_x = ctx->touch_x;
    s_last_touch_y = ctx->touch_y;
    s_last_motion_ms = ctx->now_ms;
    append_trail(ctx);
    return DEVDEMO_REDRAW_AREA;
}

const devdemo_app_t devdemo_app_touch = {
    .title = "Touch",
    .detail = "Live trail with partial DU refresh",
    .render = touch_render,
    .area_hint = touch_area_hint,
    .on_touch = touch_touch,
    .on_tick = touch_tick,
};
