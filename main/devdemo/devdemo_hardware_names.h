#pragma once

#include <stdbool.h>

static inline const char *devdemo_product_name(bool frontlight_present,
                                               bool a4_pro_power_board)
{
    if (a4_pro_power_board) {
        return "A4 Pro";
    }
    return frontlight_present ? "A4 Frontlight" : "A4";
}
