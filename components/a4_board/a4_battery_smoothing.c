#include "a4_battery_smoothing.h"

#include <string.h>

void a4_battery_percent_filter_reset(a4_battery_percent_filter_t *filter)
{
    if (filter != NULL) memset(filter, 0, sizeof(*filter));
}

uint8_t a4_battery_percent_filter_update(a4_battery_percent_filter_t *filter,
                                         uint8_t raw_percent, bool charging)
{
    if (filter == NULL) return 0;
    if (raw_percent > 100U) raw_percent = 100U;
    if (!filter->initialized) {
        filter->initialized = true;
        filter->samples[0] = raw_percent;
        filter->samples[1] = raw_percent;
        filter->samples[2] = raw_percent;
        filter->count = 3;
        filter->next = 0;
        filter->displayed_percent = raw_percent;
        return raw_percent;
    }

    filter->samples[filter->next] = raw_percent;
    filter->next = (uint8_t)((filter->next + 1U) % 3U);
    if (filter->count < 3U) ++filter->count;

    uint8_t sorted[3];
    memcpy(sorted, filter->samples, filter->count);
    for (uint8_t i = 1; i < filter->count; ++i) {
        const uint8_t value = sorted[i];
        uint8_t j = i;
        while (j != 0 && sorted[j - 1U] > value) {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = value;
    }
    /* With an even sample count use the lower middle to avoid false increases. */
    const uint8_t candidate = sorted[(filter->count - 1U) / 2U];
    if (candidate < filter->displayed_percent) {
        filter->displayed_percent = candidate;
    } else if (candidate > filter->displayed_percent && charging) {
        ++filter->displayed_percent;
    }
    return filter->displayed_percent;
}

void a4_charging_debounce_reset(a4_charging_debounce_t *debounce)
{
    if (debounce != NULL) memset(debounce, 0, sizeof(*debounce));
}

bool a4_charging_debounce_update(a4_charging_debounce_t *debounce,
                                 bool raw_active, int64_t now_us)
{
    if (debounce == NULL) return false;
    if (!debounce->pending) {
        if (debounce->stable_active == raw_active) return debounce->stable_active;
        debounce->candidate_active = raw_active;
        debounce->candidate_since_us = now_us;
        debounce->pending = true;
        return debounce->stable_active;
    }

    if (debounce->candidate_active != raw_active ||
        now_us < debounce->candidate_since_us) {
        debounce->candidate_active = raw_active;
        debounce->candidate_since_us = now_us;
    }
    if (now_us - debounce->candidate_since_us >= A4_CHARGING_DEBOUNCE_INTERVAL_US) {
        debounce->stable_active = debounce->candidate_active;
        debounce->pending = false;
    }
    return debounce->stable_active;
}

bool a4_charging_debounce_pending(const a4_charging_debounce_t *debounce)
{
    return debounce != NULL && debounce->pending;
}
