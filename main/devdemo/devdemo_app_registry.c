#include "devdemo_app_registry.h"

#include <stddef.h>

extern const devdemo_app_t devdemo_app_overview;
extern const devdemo_app_t devdemo_app_display;
extern const devdemo_app_t devdemo_app_touch;
extern const devdemo_app_t devdemo_app_keys;
extern const devdemo_app_t devdemo_app_frontlight;
extern const devdemo_app_t devdemo_app_power;
extern const devdemo_app_t devdemo_app_rtc;
extern const devdemo_app_t devdemo_app_sd;
extern const devdemo_app_t devdemo_app_wifi;
extern const devdemo_app_t devdemo_app_ble;

static const devdemo_app_t *const s_apps[] = {
    &devdemo_app_overview,
    &devdemo_app_display,
    &devdemo_app_touch,
    &devdemo_app_keys,
    &devdemo_app_frontlight,
    &devdemo_app_power,
    &devdemo_app_rtc,
    &devdemo_app_sd,
    &devdemo_app_wifi,
    &devdemo_app_ble,
};

int devdemo_app_count(void)
{
    return (int)(sizeof(s_apps) / sizeof(s_apps[0]));
}

const devdemo_app_t *devdemo_app_at(int index)
{
    if (index < 0 || index >= devdemo_app_count()) {
        return NULL;
    }
    return s_apps[index];
}

int devdemo_app_index_of(const devdemo_app_t *app)
{
    for (int index = 0; index < devdemo_app_count(); ++index) {
        if (s_apps[index] == app) {
            return index;
        }
    }
    return -1;
}

const devdemo_app_t *devdemo_overview_page(void)
{
    return &devdemo_app_overview;
}
