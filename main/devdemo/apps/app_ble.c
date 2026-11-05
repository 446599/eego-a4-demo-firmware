#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "devdemo_app.h"
#include "devdemo_ui.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "devdemo_ble";

enum {
    BLE_MAX_SEEN = 64,
    BLE_START_TIMEOUT_MS = 4000,
    BLE_SCAN_TIMEOUT_MS = 7000,
    BLE_SCAN_DURATION_MS = 4000,
};

typedef enum {
    BLE_PAGE_IDLE = 0,
    BLE_PAGE_STARTING,
    BLE_PAGE_SCANNING,
    BLE_PAGE_DONE,
    BLE_PAGE_ERROR,
} ble_page_state_t;

typedef struct {
    ble_addr_t address;
    int8_t rssi;
    char name[32];
} ble_seen_t;

static ble_page_state_t s_state;
static bool s_initialized;
static bool s_host_started;
static bool s_scan_requested;
static volatile bool s_scanning;
static volatile bool s_scan_started;
static volatile bool s_scan_done;
static volatile bool s_scan_error;
static uint8_t s_own_addr_type;
static int64_t s_phase_started_ms;
static esp_pm_lock_handle_t s_pm_lock;

static uint16_t s_seen_count;
static uint16_t s_unique_count;
static int8_t s_strongest_rssi;
static char s_strongest_name[32];
static ble_seen_t s_seen[BLE_MAX_SEEN];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void ble_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void record_advertisement(const struct ble_gap_disc_desc *disc)
{
    if (disc == NULL) {
        return;
    }
    struct ble_hs_adv_fields fields = {0};
    const bool parsed =
        ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0;
    char name[32] = {0};
    if (parsed && fields.name != NULL && fields.name_len > 0U) {
        const size_t length =
            fields.name_len < sizeof(name) - 1U
                ? fields.name_len
                : sizeof(name) - 1U;
        for (size_t index = 0; index < length; ++index) {
            const uint8_t value = fields.name[index];
            name[index] =
                value >= 32U && value <= 126U ? (char)value : '?';
        }
        name[length] = '\0';
    }
    char address[32];
    (void)snprintf(address, sizeof(address),
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   disc->addr.val[5], disc->addr.val[4],
                   disc->addr.val[3], disc->addr.val[2],
                   disc->addr.val[1], disc->addr.val[0]);
    const char *const label = name[0] != '\0' ? name : address;
    const size_t label_length = strlen(label);

    portENTER_CRITICAL(&s_lock);
    int existing = -1;
    for (uint16_t index = 0; index < s_seen_count; ++index) {
        if (memcmp(s_seen[index].address.val, disc->addr.val,
                   sizeof(disc->addr.val)) == 0 &&
            s_seen[index].address.type == disc->addr.type) {
            existing = index;
            break;
        }
    }
    if (existing < 0 && s_seen_count < BLE_MAX_SEEN) {
        existing = s_seen_count++;
        memset(&s_seen[existing], 0, sizeof(s_seen[existing]));
        s_seen[existing].address = disc->addr;
        ++s_unique_count;
    }
    if (existing >= 0) {
        ble_seen_t *const seen = &s_seen[existing];
        seen->rssi = disc->rssi;
        if (name[0] != '\0') {
            memcpy(seen->name, name, label_length + 1U);
        }
        if (seen->rssi > s_strongest_rssi) {
            s_strongest_rssi = seen->rssi;
            memcpy(s_strongest_name, label, label_length + 1U);
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event == NULL) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC) {
        record_advertisement(&event->disc);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scanning = false;
        s_scan_done = true;
        ESP_LOGI(TAG, "scan complete reason=%d",
                 event->disc_complete.reason);
    }
    return 0;
}

static void start_observer_scan(void)
{
    if (!s_scan_requested) {
        return;
    }
    const int address_status =
        ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (address_status != 0) {
        s_scan_error = true;
        s_scan_done = true;
        return;
    }
    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;
    params.passive = 0;
    params.itvl = 160;
    params.window = 80;
    const int status =
        ble_gap_disc(s_own_addr_type, BLE_SCAN_DURATION_MS, &params,
                     gap_event, NULL);
    if (status != 0) {
        s_scan_error = true;
        s_scan_done = true;
        return;
    }
    s_scanning = true;
    s_scan_started = true;
    ESP_LOGI(TAG, "observer scan submitted");
}

static void ble_reset(int reason)
{
    (void)reason;
    s_scan_error = true;
    s_scan_done = true;
}

static void ble_sync(void)
{
    start_observer_scan();
}

static void release_pm_lock(void)
{
    if (s_pm_lock != NULL) {
        (void)esp_pm_lock_release(s_pm_lock);
        (void)esp_pm_lock_delete(s_pm_lock);
        s_pm_lock = NULL;
    }
}

static void stop_stack(void)
{
    if (s_scanning) {
        (void)ble_gap_disc_cancel();
    }
    s_scanning = false;
    if (s_host_started) {
        (void)nimble_port_stop();
    }
    if (s_initialized) {
        (void)nimble_port_deinit();
    }
    s_host_started = false;
    s_initialized = false;
    s_scan_requested = false;
    s_scan_started = false;
    release_pm_lock();
}

static void start_ble(devdemo_ctx_t *ctx)
{
    if (s_state == BLE_PAGE_STARTING || s_state == BLE_PAGE_SCANNING) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_seen_count = 0;
    s_unique_count = 0;
    s_strongest_rssi = INT8_MIN;
    s_strongest_name[0] = '\0';
    memset(s_seen, 0, sizeof(s_seen));
    portEXIT_CRITICAL(&s_lock);
    s_scan_done = false;
    s_scan_started = false;
    s_scan_error = false;
    s_scanning = false;
    s_scan_requested = true;
    s_state = BLE_PAGE_STARTING;
    s_phase_started_ms = ctx->now_ms;

    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "devdemo_ble",
                           &s_pm_lock) != ESP_OK ||
        s_pm_lock == NULL ||
        esp_pm_lock_acquire(s_pm_lock) != ESP_OK) {
        release_pm_lock();
        s_scan_requested = false;
        s_state = BLE_PAGE_ERROR;
        return;
    }
    if (nimble_port_init() != ESP_OK) {
        s_scan_requested = false;
        release_pm_lock();
        s_state = BLE_PAGE_ERROR;
        return;
    }
    s_initialized = true;
    ble_hs_cfg.reset_cb = ble_reset;
    ble_hs_cfg.sync_cb = ble_sync;
    nimble_port_freertos_init(ble_host_task);
    s_host_started = true;
}

static void cancel_scan(void)
{
    if (s_state == BLE_PAGE_STARTING || s_state == BLE_PAGE_SCANNING) {
        stop_stack();
    }
    s_state = BLE_PAGE_IDLE;
}

static void ble_enter(devdemo_ctx_t *ctx)
{
    (void)ctx;
    s_state = BLE_PAGE_IDLE;
}

static void ble_exit(devdemo_ctx_t *ctx)
{
    (void)ctx;
    cancel_scan();
}

static const char *state_text(void)
{
    switch (s_state) {
    case BLE_PAGE_STARTING: return "HOST STARTING";
    case BLE_PAGE_SCANNING: return "OBSERVER SCAN";
    case BLE_PAGE_DONE: return "COMPLETE / RADIO OFF";
    case BLE_PAGE_ERROR: return "ERROR / RADIO OFF";
    case BLE_PAGE_IDLE:
    default: return "IDLE / RADIO OFF";
    }
}

static void ble_render(devdemo_ctx_t *ctx)
{
    char line[96];
    uint16_t count;
    int8_t strongest;
    char strongest_name[32];
    portENTER_CRITICAL(&s_lock);
    count = s_unique_count;
    strongest = s_strongest_rssi;
    (void)snprintf(strongest_name, sizeof(strongest_name), "%s",
                   s_strongest_name);
    portEXIT_CRITICAL(&s_lock);

    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "BLE Observer",
                      "NimBLE discovery only, no GATT, bond or connection");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "SCAN STATE");
    y = devdemo_ui_row(ctx->fb, y, "State", state_text());
    (void)snprintf(line, sizeof(line), "%u", (unsigned)count);
    y = devdemo_ui_row(ctx->fb, y, "Devices", line);
    if (strongest_name[0] != '\0') {
        (void)snprintf(line, sizeof(line), "%s %d dBm", strongest_name,
                       (int)strongest);
    } else {
        (void)snprintf(line, sizeof(line), "-");
    }
    y = devdemo_ui_row(ctx->fb, y, "Strongest", line);

    y = devdemo_ui_section(ctx->fb, y + 18, "RADIO POLICY");
    y = devdemo_ui_row(ctx->fb, y, "Role", "OBSERVER");
    y = devdemo_ui_row(ctx->fb, y, "GATT service", "NOT CREATED");
    y = devdemo_ui_row(ctx->fb, y, "Pairing / bond", "DISABLED");
    y = devdemo_ui_row(ctx->fb, y, "Identity store", "NOT INITIALIZED");
    y = devdemo_ui_row(ctx->fb, y, "On completion", "deinit host/controller");
    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, y + 18,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "Tap SCAN or press UP. Leaving this page cancels and releases BLE.",
        true);
    devdemo_ui_button(
        ctx->fb, devdemo_ui_bar_rect(0, 1),
        s_state == BLE_PAGE_STARTING || s_state == BLE_PAGE_SCANNING
            ? "SCANNING"
            : "SCAN",
        s_state == BLE_PAGE_STARTING || s_state == BLE_PAGE_SCANNING);
}

static devdemo_redraw_t ble_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        start_ble(ctx);
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t ble_key(devdemo_ctx_t *ctx, int key)
{
    if (key == DEVDEMO_KEY_UP) {
        start_ble(ctx);
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t ble_tick(devdemo_ctx_t *ctx)
{
    if (s_state == BLE_PAGE_STARTING) {
        if (s_scan_done && s_scan_error) {
            stop_stack();
            s_state = BLE_PAGE_ERROR;
            return DEVDEMO_REDRAW_PAGE;
        }
        if (s_scan_started) {
            s_state = BLE_PAGE_SCANNING;
            s_phase_started_ms = ctx->now_ms;
            return DEVDEMO_REDRAW_PAGE;
        }
        if (ctx->now_ms - s_phase_started_ms >= BLE_START_TIMEOUT_MS) {
            stop_stack();
            s_state = BLE_PAGE_ERROR;
            return DEVDEMO_REDRAW_PAGE;
        }
    }
    if (s_state == BLE_PAGE_SCANNING) {
        if (s_scan_done) {
            stop_stack();
            s_state = s_scan_error ? BLE_PAGE_ERROR : BLE_PAGE_DONE;
            return DEVDEMO_REDRAW_PAGE;
        }
        if (ctx->now_ms - s_phase_started_ms >= BLE_SCAN_TIMEOUT_MS) {
            stop_stack();
            s_state = BLE_PAGE_ERROR;
            return DEVDEMO_REDRAW_PAGE;
        }
    }
    return DEVDEMO_REDRAW_NONE;
}

const devdemo_app_t devdemo_app_ble = {
    .title = "BLE Observer",
    .detail = "NimBLE scan-only, no connection or bond",
    .render = ble_render,
    .on_enter = ble_enter,
    .on_exit = ble_exit,
    .on_touch = ble_touch,
    .on_key = ble_key,
    .on_tick = ble_tick,
};
