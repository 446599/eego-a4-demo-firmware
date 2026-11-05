#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    A4_CHARGING_DEBOUNCE_INTERVAL_US = 200000,
};

typedef struct {
    uint8_t samples[3];
    uint8_t count;
    uint8_t next;
    uint8_t displayed_percent;
    bool initialized;
} a4_battery_percent_filter_t;

typedef struct {
    bool stable_active;
    bool candidate_active;
    bool pending;
    int64_t candidate_since_us;
} a4_charging_debounce_t;

void a4_battery_percent_filter_reset(a4_battery_percent_filter_t *filter);
uint8_t a4_battery_percent_filter_update(a4_battery_percent_filter_t *filter,
                                         uint8_t raw_percent, bool charging);
void a4_charging_debounce_reset(a4_charging_debounce_t *debounce);
bool a4_charging_debounce_update(a4_charging_debounce_t *debounce,
                                  bool raw_active, int64_t now_us);
bool a4_charging_debounce_pending(const a4_charging_debounce_t *debounce);
