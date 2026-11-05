#pragma once

#include <stdbool.h>

/* GPIO11 is the board's active-high cable/charging-present sense. */
static inline bool a4_charging_status_is_active(bool level_high)
{
    return level_high;
}
