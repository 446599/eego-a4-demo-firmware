#include <stdio.h>
#include <string.h>

#include "a4_board.h"
#include "a4_sd.h"
#include "devdemo_app.h"
#include "devdemo_ui.h"
#include "esp_app_desc.h"
#include "esp_mac.h"

typedef struct {
    bool frontlight;
    bool switched_power;
    bool battery_present;
    uint8_t battery_percent;
    bool charging;
    bool rtc_valid;
    bool sd_mounted;
    int sd_root_entries;
} overview_snapshot_t;

static overview_snapshot_t s_snapshot;
static int64_t s_last_poll_ms;

static overview_snapshot_t take_snapshot(void)
{
    overview_snapshot_t snapshot = {
        .frontlight = a4_board_has_frontlight(),
        .switched_power = a4_board_sd_power_can_suspend(),
        .battery_present = a4_board_battery_present(),
        .battery_percent = a4_board_read_battery_percent(),
        .charging = a4_board_is_charging(),
        .sd_mounted = a4_sd_is_mounted(),
    };
    a4_datetime_t datetime = {0};
    snapshot.rtc_valid = a4_board_read_datetime(&datetime);
    if (snapshot.sd_mounted) {
        a4_sd_entry_t entry = {0};
        (void)a4_sd_list("", 0, &entry, 0, &snapshot.sd_root_entries);
    }
    snapshot.battery_present = a4_board_battery_present();
    return snapshot;
}

static bool snapshot_equal(const overview_snapshot_t *left,
                           const overview_snapshot_t *right)
{
    return left->frontlight == right->frontlight &&
           left->switched_power == right->switched_power &&
           left->battery_present == right->battery_present &&
           left->battery_percent == right->battery_percent &&
           left->charging == right->charging &&
           left->rtc_valid == right->rtc_valid &&
           left->sd_mounted == right->sd_mounted &&
           left->sd_root_entries == right->sd_root_entries;
}

static void refresh_snapshot(void)
{
    s_snapshot = take_snapshot();
}

static void overview_enter(devdemo_ctx_t *ctx)
{
    refresh_snapshot();
    s_last_poll_ms = ctx->now_ms;
}

static void overview_render(devdemo_ctx_t *ctx)
{
    char line[96];
    uint8_t mac[6] = {0};
    const esp_app_desc_t *const app = esp_app_get_description();
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Overview",
                      "Hardware identity, firmware and live services");

    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP,
                               "HARDWARE PROFILE");
    y = devdemo_ui_row(ctx->fb, y, "Model",
                       s_snapshot.frontlight ? "A4" : "A4 Lite");
    y = devdemo_ui_row(ctx->fb, y, "Power board",
                       s_snapshot.switched_power ? "CW2015 / switched"
                                                 : "ADC / legacy");
    y = devdemo_ui_row(ctx->fb, y, "Frontlight",
                       s_snapshot.frontlight ? "LM3630A @ 0x36"
                                             : "Not fitted");
    y = devdemo_ui_row(ctx->fb, y, "Display",
                       "UC8279C 768x552 SPI3");

    y = devdemo_ui_section(ctx->fb, y + 8, "POWER");
    if (s_snapshot.battery_present) {
        (void)snprintf(line, sizeof(line), "%u%% %s",
                       (unsigned)s_snapshot.battery_percent,
                       s_snapshot.charging ? "CHARGING" : "DISCHARGE");
    } else {
        (void)snprintf(line, sizeof(line), "NO BATTERY");
    }
    y = devdemo_ui_row(ctx->fb, y, "Battery", line);
    y = devdemo_ui_row(ctx->fb, y, "RTC",
                       s_snapshot.rtc_valid ? "PCF8563 ready"
                                            : "PCF8563 no time");
    (void)snprintf(line, sizeof(line), "%s / %d root entries",
                   s_snapshot.sd_mounted ? "MOUNTED" : "NOT MOUNTED",
                   s_snapshot.sd_root_entries);
    y = devdemo_ui_row(ctx->fb, y, "Storage", line);

    y = devdemo_ui_section(ctx->fb, y + 8, "FIRMWARE");
    y = devdemo_ui_row(ctx->fb, y, "Version",
                       app != NULL ? app->version : "UNKNOWN");
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        (void)snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        (void)snprintf(line, sizeof(line), "unavailable");
    }
    y = devdemo_ui_row(ctx->fb, y, "Base MAC", line);
    (void)snprintf(line, sizeof(line), "GC %lu  FAST %lu  LAST %lu ms",
                   (unsigned long)ctx->gc_refresh_count,
                   (unsigned long)ctx->fast_refresh_count,
                   (unsigned long)ctx->last_refresh_ms);
    (void)devdemo_ui_row(ctx->fb, y, "Refresh", line);

    devdemo_ui_button(ctx->fb, devdemo_ui_bar_rect(0, 1), "READ AGAIN", false);
}

static devdemo_redraw_t overview_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        refresh_snapshot();
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t overview_tick(devdemo_ctx_t *ctx)
{
    if (ctx->now_ms - s_last_poll_ms < 2000) {
        return DEVDEMO_REDRAW_NONE;
    }
    s_last_poll_ms = ctx->now_ms;
    const overview_snapshot_t next = take_snapshot();
    if (snapshot_equal(&next, &s_snapshot)) {
        return DEVDEMO_REDRAW_NONE;
    }
    s_snapshot = next;
    return DEVDEMO_REDRAW_PAGE;
}

const devdemo_app_t devdemo_app_overview = {
    .title = "Overview",
    .detail = "Model, power profile, battery, firmware",
    .enter_full = true,
    .on_enter = overview_enter,
    .render = overview_render,
    .on_touch = overview_touch,
    .on_tick = overview_tick,
};
