#include <stdio.h>
#include <string.h>

#include "a4_board.h"
#include "devdemo_app.h"
#include "devdemo_ui.h"

static a4_datetime_t s_datetime;
static bool s_valid;
static int64_t s_last_poll_ms;

static void read_rtc(void)
{
    a4_datetime_t next = {0};
    s_valid = a4_board_read_datetime(&next);
    if (s_valid) {
        s_datetime = next;
    }
}

static void rtc_enter(devdemo_ctx_t *ctx)
{
    read_rtc();
    s_last_poll_ms = ctx->now_ms;
}

static void rtc_render(devdemo_ctx_t *ctx)
{
    char line[64];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "RTC",
                      "PCF8563 @ I2C 0x51, read-only demonstration");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "CURRENT TIME");
    if (!s_valid) {
        y = devdemo_ui_row(ctx->fb, y, "Status", "READ FAILED");
        y = devdemo_ui_row(ctx->fb, y, "Address", "0x51");
        y = devdemo_ui_row(ctx->fb, y, "Bus", "I2C0, 100 kHz");
    } else {
        (void)snprintf(line, sizeof(line), "%04u-%02u-%02u",
                       (unsigned)s_datetime.year,
                       (unsigned)s_datetime.month,
                       (unsigned)s_datetime.day);
        y = devdemo_ui_row(ctx->fb, y, "Date", line);
        (void)snprintf(line, sizeof(line), "%02u:%02u:%02u",
                       (unsigned)s_datetime.hour,
                       (unsigned)s_datetime.minute,
                       (unsigned)s_datetime.second);
        y = devdemo_ui_row(ctx->fb, y, "Time", line);
        (void)snprintf(line, sizeof(line), "%u",
                       (unsigned)s_datetime.weekday);
        y = devdemo_ui_row(ctx->fb, y, "Weekday", line);
        y = devdemo_ui_row(ctx->fb, y, "Status", "READ OK");
    }
    y = devdemo_ui_section(ctx->fb, y + 18, "SAFETY");
    y = devdemo_ui_row(ctx->fb, y, "Write RTC", "DISABLED");
    y = devdemo_ui_row(ctx->fb, y, "Clear RTC", "DISABLED");
    (void)devdemo_ui_row(ctx->fb, y, "Purpose", "read and display only");
    devdemo_ui_button(ctx->fb, devdemo_ui_bar_rect(0, 1), "READ", false);
}

static devdemo_redraw_t rtc_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        read_rtc();
        s_last_poll_ms = ctx->now_ms;
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t rtc_tick(devdemo_ctx_t *ctx)
{
    if (ctx->now_ms - s_last_poll_ms < 1000) {
        return DEVDEMO_REDRAW_NONE;
    }
    s_last_poll_ms = ctx->now_ms;
    a4_datetime_t previous = s_datetime;
    const bool previous_valid = s_valid;
    read_rtc();
    if (previous_valid != s_valid ||
        (s_valid && memcmp(&previous, &s_datetime, sizeof(s_datetime)) != 0)) {
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

const devdemo_app_t devdemo_app_rtc = {
    .title = "RTC",
    .detail = "PCF8563 read-only date and time",
    .render = rtc_render,
    .on_enter = rtc_enter,
    .on_touch = rtc_touch,
    .on_tick = rtc_tick,
};
