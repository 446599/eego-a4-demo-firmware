#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    A4_FRONTLIGHT_CONTROL_SLEEP_COMMAND = 0x80,
    A4_FRONTLIGHT_CONTROL_SLEEP_STATUS = 0x40,
    A4_FRONTLIGHT_CONTROL_WARM_ENABLE = 0x04,
    A4_FRONTLIGHT_CONTROL_COLD_ENABLE = 0x02,
    A4_FRONTLIGHT_CONTROL_CHANNEL_ENABLE_MASK =
        A4_FRONTLIGHT_CONTROL_WARM_ENABLE |
        A4_FRONTLIGHT_CONTROL_COLD_ENABLE,
    /* Every writable Control-register bit. Bit 6 is SLEEP_STATUS (read-only). */
    A4_FRONTLIGHT_CONTROL_WRITABLE_MASK = 0x9f,
};

static inline uint8_t a4_frontlight_channel_enable_mask(bool warm_enabled,
                                                          bool cold_enabled)
{
    return (uint8_t)((warm_enabled ? A4_FRONTLIGHT_CONTROL_WARM_ENABLE : 0) |
                     (cold_enabled ? A4_FRONTLIGHT_CONTROL_COLD_ENABLE : 0));
}
