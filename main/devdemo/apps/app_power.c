#include <stdio.h>

#include "a4_board.h"
#include "devdemo_app.h"
#include "devdemo_ui.h"

typedef struct {
    bool present;
    bool charging;
    bool switched;
    uint8_t percent;
} power_snapshot_t;

static power_snapshot_t s_power;
static int64_t s_last_poll_ms;

static power_snapshot_t take_power(void)
{
    power_snapshot_t snapshot = {
        .switched = a4_board_sd_power_can_suspend(),
        .percent = a4_board_read_battery_percent(),
        .present = a4_board_battery_present(),
    };
    snapshot.charging =
        snapshot.present ? a4_board_is_charging() : false;
    snapshot.present = a4_board_battery_present();
    return snapshot;
}

static bool power_equal(const power_snapshot_t *left,
                        const power_snapshot_t *right)
{
    return left->present == right->present &&
           left->charging == right->charging &&
           left->switched == right->switched &&
           left->percent == right->percent;
}

static void power_enter(devdemo_ctx_t *ctx)
{
    s_power = take_power();
    s_last_poll_ms = ctx->now_ms;
}

static void power_render(devdemo_ctx_t *ctx)
{
    char line[64];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Power / Battery",
                      "Battery source, charge detect and board profile");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "LIVE POWER");
    (void)snprintf(line, sizeof(line), "%u%%",
                   (unsigned)s_power.percent);
    y = devdemo_ui_row(ctx->fb, y, "Battery",
                       s_power.present ? line : "NO BATTERY");
    y = devdemo_ui_row(ctx->fb, y, "Charging",
                       s_power.charging ? "CHARGING" : "IDLE");
    y = devdemo_ui_row(ctx->fb, y, "Source",
                       s_power.switched ? "CW2015 @ 0x62"
                                        : "ADC GPIO10");
    y = devdemo_ui_row(ctx->fb, y, "Profile",
                       s_power.switched ? "CW2015 / switched"
                                        : "Legacy / ADC");

    y = devdemo_ui_section(ctx->fb, y + 18, "BOARD SIGNALS");
    y = devdemo_ui_row(ctx->fb, y, "Charge detect", "GPIO11 active-high");
    y = devdemo_ui_row(ctx->fb, y, "MCU latch", "GPIO4");
    y = devdemo_ui_row(ctx->fb, y, "SD rail", "GPIO6");
    (void)devdemo_ui_row(ctx->fb, y, "Power source",
                         s_power.switched ? "GPIO6 LOW enables"
                                          : "GPIO6 HIGH required");
    devdemo_ui_button(ctx->fb, devdemo_ui_bar_rect(0, 1), "READ", false);
}

static devdemo_redraw_t power_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        s_power = take_power();
        s_last_poll_ms = ctx->now_ms;
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t power_tick(devdemo_ctx_t *ctx)
{
    if (ctx->now_ms - s_last_poll_ms < 1000) {
        return DEVDEMO_REDRAW_NONE;
    }
    s_last_poll_ms = ctx->now_ms;
    const power_snapshot_t next = take_power();
    if (power_equal(&next, &s_power)) {
        return DEVDEMO_REDRAW_NONE;
    }
    s_power = next;
    return DEVDEMO_REDRAW_PAGE;
}

const devdemo_app_t devdemo_app_power = {
    .title = "Power / Battery",
    .detail = "CW2015 or ADC, charging and GPIO profile",
    .render = power_render,
    .on_enter = power_enter,
    .on_touch = power_touch,
    .on_tick = power_tick,
};
