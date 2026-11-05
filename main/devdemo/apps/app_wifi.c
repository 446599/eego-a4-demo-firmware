#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "devdemo_app.h"
#include "devdemo_ui.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "devdemo_wifi";

enum {
    WIFI_STARTED_BIT = BIT0,
    WIFI_SCAN_DONE_BIT = BIT1,
    WIFI_MAX_RESULTS = 32,
    WIFI_START_TIMEOUT_MS = 3000,
    WIFI_SCAN_TIMEOUT_MS = 25000,
};

typedef enum {
    WIFI_PAGE_IDLE = 0,
    WIFI_PAGE_STARTING,
    WIFI_PAGE_SCANNING,
    WIFI_PAGE_DONE,
    WIFI_PAGE_ERROR,
} wifi_page_state_t;

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static bool s_initialized;
static bool s_driver_started;
static wifi_page_state_t s_state;
static uint8_t s_scan_status;
static uint16_t s_network_count;
static char s_strongest[33];
static int8_t s_strongest_rssi;
static int64_t s_phase_started_ms;

static void wifi_event_handler(void *argument, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (base != WIFI_EVENT || s_events == NULL) {
        return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
        s_driver_started = true;
        xEventGroupSetBits(s_events, WIFI_STARTED_BIT);
    } else if (event_id == WIFI_EVENT_STA_STOP) {
        s_driver_started = false;
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *const scan = event_data;
        s_scan_status = scan != NULL ? scan->status : 0;
        xEventGroupSetBits(s_events, WIFI_SCAN_DONE_BIT);
    }
}

static bool wifi_init_driver(void)
{
    if (s_initialized) {
        return true;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return false;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return false;
    }
    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        return false;
    }
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
    (void)esp_wifi_set_country_code("CN", false);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        (void)esp_wifi_deinit();
        return false;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK) {
        (void)esp_wifi_deinit();
        return false;
    }
    s_initialized = true;
    return true;
}

static void wifi_stop_radio(void)
{
    if (!s_initialized) {
        return;
    }
    if (s_state == WIFI_PAGE_SCANNING) {
        (void)esp_wifi_scan_stop();
    }
    if (s_driver_started) {
        (void)esp_wifi_stop();
    }
    s_driver_started = false;
    if (s_events != NULL) {
        xEventGroupClearBits(s_events,
                             WIFI_STARTED_BIT | WIFI_SCAN_DONE_BIT);
    }
}

static void ascii_copy(char *destination, size_t size, const char *source)
{
    size_t written = 0;
    while (source != NULL && *source != '\0' && written + 1U < size) {
        const uint8_t value = (uint8_t)*source++;
        destination[written++] =
            value >= 32U && value <= 126U ? (char)value : '?';
    }
    destination[written] = '\0';
}

static void parse_results(void)
{
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) {
        s_scan_status = 1;
        return;
    }
    if (count > WIFI_MAX_RESULTS) {
        count = WIFI_MAX_RESULTS;
    }
    wifi_ap_record_t records[WIFI_MAX_RESULTS] = {0};
    if (count > 0 &&
        esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        s_scan_status = 1;
        return;
    }

    char seen[WIFI_MAX_RESULTS][33] = {{0}};
    uint16_t unique = 0;
    int8_t strongest_rssi = INT8_MIN;
    char strongest[33] = {0};
    for (uint16_t index = 0; index < count; ++index) {
        if (records[index].ssid[0] == '\0') {
            continue;
        }
        char ssid[33];
        ascii_copy(ssid, sizeof(ssid), (const char *)records[index].ssid);
        bool duplicate = false;
        for (uint16_t old = 0; old < unique; ++old) {
            if (strcmp(seen[old], ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        (void)snprintf(seen[unique], sizeof(seen[unique]), "%s", ssid);
        ++unique;
        if (records[index].rssi > strongest_rssi) {
            strongest_rssi = records[index].rssi;
            (void)snprintf(strongest, sizeof(strongest), "%s", ssid);
        }
    }
    s_network_count = unique;
    s_strongest_rssi = strongest_rssi;
    (void)snprintf(s_strongest, sizeof(s_strongest), "%s", strongest);
    ESP_LOGI(TAG, "scan networks=%u strongest=%s rssi=%d",
             (unsigned)unique, strongest, (int)strongest_rssi);
}

static void wifi_start_scan(devdemo_ctx_t *ctx)
{
    if (s_state == WIFI_PAGE_STARTING || s_state == WIFI_PAGE_SCANNING) {
        return;
    }
    if (!wifi_init_driver()) {
        s_state = WIFI_PAGE_ERROR;
        s_scan_status = 1;
        return;
    }
    xEventGroupClearBits(s_events, WIFI_STARTED_BIT | WIFI_SCAN_DONE_BIT);
    s_state = WIFI_PAGE_STARTING;
    s_scan_status = 0;
    s_network_count = 0;
    s_strongest[0] = '\0';
    s_strongest_rssi = INT8_MIN;
    s_phase_started_ms = ctx->now_ms;
    const esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        s_driver_started = true;
    } else if (err == ESP_ERR_WIFI_STATE) {
        s_driver_started = true;
        xEventGroupSetBits(s_events, WIFI_STARTED_BIT);
    } else {
        s_state = WIFI_PAGE_ERROR;
        s_scan_status = 1;
    }
    ESP_LOGI(TAG, "Wi-Fi scan requested: %s", esp_err_to_name(err));
}

static void wifi_cancel_scan(void)
{
    if (s_state == WIFI_PAGE_STARTING || s_state == WIFI_PAGE_SCANNING) {
        wifi_stop_radio();
    }
    s_state = WIFI_PAGE_IDLE;
}

static void wifi_enter(devdemo_ctx_t *ctx)
{
    (void)ctx;
    s_state = WIFI_PAGE_IDLE;
}

static void wifi_exit(devdemo_ctx_t *ctx)
{
    (void)ctx;
    wifi_cancel_scan();
}

static const char *state_text(void)
{
    switch (s_state) {
    case WIFI_PAGE_STARTING: return "RADIO STARTING";
    case WIFI_PAGE_SCANNING: return "ACTIVE SCAN";
    case WIFI_PAGE_DONE: return "COMPLETE / RADIO OFF";
    case WIFI_PAGE_ERROR: return "ERROR / RADIO OFF";
    case WIFI_PAGE_IDLE:
    default: return "IDLE / RADIO OFF";
    }
}

static void wifi_render(devdemo_ctx_t *ctx)
{
    char line[96];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Wi-Fi Scan",
                      "Scan-only, RAM storage, no station or credentials");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "SCAN STATE");
    y = devdemo_ui_row(ctx->fb, y, "State", state_text());
    (void)snprintf(line, sizeof(line), "%u", (unsigned)s_network_count);
    y = devdemo_ui_row(ctx->fb, y, "Networks", line);
    if (s_strongest[0] != '\0') {
        (void)snprintf(line, sizeof(line), "%s %d dBm", s_strongest,
                       (int)s_strongest_rssi);
    } else {
        (void)snprintf(line, sizeof(line), "-");
    }
    y = devdemo_ui_row(ctx->fb, y, "Strongest", line);

    y = devdemo_ui_section(ctx->fb, y + 18, "RADIO POLICY");
    y = devdemo_ui_row(ctx->fb, y, "Storage", "WIFI_STORAGE_RAM");
    y = devdemo_ui_row(ctx->fb, y, "Country", "CN / channels 1-13");
    y = devdemo_ui_row(ctx->fb, y, "Connection", "DISABLED");
    y = devdemo_ui_row(ctx->fb, y, "Credentials", "NOT READ / NOT WRITTEN");
    y = devdemo_ui_row(ctx->fb, y, "On page exit", "stop scan and radio");
    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, y + 18,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "Tap SCAN or press UP. POWER opens the menu; leaving this page stops Wi-Fi.",
        true);
    devdemo_ui_button(
        ctx->fb, devdemo_ui_bar_rect(0, 1),
        s_state == WIFI_PAGE_STARTING || s_state == WIFI_PAGE_SCANNING
            ? "SCANNING"
            : "SCAN",
        s_state == WIFI_PAGE_STARTING || s_state == WIFI_PAGE_SCANNING);
}

static devdemo_redraw_t wifi_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        wifi_start_scan(ctx);
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t wifi_key(devdemo_ctx_t *ctx, int key)
{
    if (key == DEVDEMO_KEY_UP) {
        wifi_start_scan(ctx);
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

static devdemo_redraw_t wifi_tick(devdemo_ctx_t *ctx)
{
    if (s_state == WIFI_PAGE_STARTING) {
        const EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & WIFI_STARTED_BIT) != 0) {
            wifi_scan_config_t config = {0};
            config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            config.show_hidden = true;
            const esp_err_t err = esp_wifi_scan_start(&config, false);
            if (err == ESP_OK) {
                s_state = WIFI_PAGE_SCANNING;
                s_phase_started_ms = ctx->now_ms;
                return DEVDEMO_REDRAW_PAGE;
            }
            if (err != ESP_ERR_NOT_FINISHED) {
                s_state = WIFI_PAGE_ERROR;
                s_scan_status = 1;
                wifi_stop_radio();
                return DEVDEMO_REDRAW_PAGE;
            }
        } else if (ctx->now_ms - s_phase_started_ms >=
                   WIFI_START_TIMEOUT_MS) {
            s_state = WIFI_PAGE_ERROR;
            s_scan_status = 1;
            wifi_stop_radio();
            return DEVDEMO_REDRAW_PAGE;
        }
    } else if (s_state == WIFI_PAGE_SCANNING) {
        const EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & WIFI_SCAN_DONE_BIT) != 0) {
            if (s_scan_status == 0) {
                parse_results();
            }
            wifi_stop_radio();
            s_state = s_scan_status == 0 ? WIFI_PAGE_DONE : WIFI_PAGE_ERROR;
            return DEVDEMO_REDRAW_PAGE;
        }
        if (ctx->now_ms - s_phase_started_ms >= WIFI_SCAN_TIMEOUT_MS) {
            s_state = WIFI_PAGE_ERROR;
            s_scan_status = 1;
            wifi_stop_radio();
            return DEVDEMO_REDRAW_PAGE;
        }
    }
    return DEVDEMO_REDRAW_NONE;
}

const devdemo_app_t devdemo_app_wifi = {
    .title = "Wi-Fi Scan",
    .detail = "Scan-only radio demonstration, no connection",
    .render = wifi_render,
    .on_enter = wifi_enter,
    .on_exit = wifi_exit,
    .on_touch = wifi_touch,
    .on_key = wifi_key,
    .on_tick = wifi_tick,
};
