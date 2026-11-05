#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Discharge curve imported from the attached 505060-2000 cycle workbook.
 * The source contains 300 discharge cycles. Each point below is the median
 * cell voltage at the corresponding remaining-capacity percentile across all
 * cycles. The values are the median voltage at each remaining-capacity
 * percentile, rounded to the nearest millivolt. Keeping the curve in
 * millivolts makes the conversion deterministic and avoids depending on a
 * proprietary CW2015 BATINFO generator.
 */
static inline uint8_t a4_battery_percent_from_mv(int battery_mv)
{
    static const struct {
        int mv;
        int percent;
    } curve[] = {
        {2999, 0}, {3239, 5},  {3318, 10}, {3377, 15}, {3432, 20},
        {3482, 25}, {3534, 30}, {3598, 35}, {3659, 40}, {3706, 45},
        {3738, 50}, {3764, 55}, {3787, 60}, {3810, 65}, {3835, 70},
        {3864, 75}, {3895, 80}, {3923, 85}, {3946, 90}, {3968, 95},
        {4104, 100},
    };

    if (battery_mv <= curve[0].mv) {
        return 0;
    }
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (battery_mv <= curve[i].mv) {
            const int span_mv = curve[i].mv - curve[i - 1].mv;
            const int span_percent = curve[i].percent - curve[i - 1].percent;
            return (uint8_t)(curve[i - 1].percent +
                             ((battery_mv - curve[i - 1].mv) * span_percent) /
                                 span_mv);
        }
    }
    return 100;
}

/* The CW2015 VCELL measurement is the input to the imported discharge curve. */
static inline uint8_t a4_battery_percent_from_cw2015_voltage(int battery_mv)
{
    return a4_battery_percent_from_mv(battery_mv);
}

/* 老主板 GPIO10：恢复原有分压换算，不改变 CW2015 曲线。 */
static inline int a4_legacy_battery_mv_from_adc(int adc_mv)
{
    if (adc_mv < 0 || adc_mv > 4500) return 0;
    const int battery_mv = adc_mv * 1559 / 1000;
    /* 悬空或超量程输入不能被夹成一个看似有效的电池电压。 */
    return battery_mv >= 2500 && battery_mv <= 4500 ? battery_mv : 0;
}

static inline uint8_t a4_legacy_battery_percent_from_mv(int battery_mv)
{
    static const int curve[][2] = {
        {3300, 0}, {3500, 8}, {3600, 15}, {3700, 35}, {3800, 55},
        {3900, 70}, {4000, 85}, {4100, 95}, {4180, 100},
    };
    if (battery_mv <= curve[0][0]) return 0;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (battery_mv <= curve[i][0]) {
            const int span = curve[i][0] - curve[i - 1][0];
            return (uint8_t)(curve[i - 1][1] +
                ((battery_mv - curve[i - 1][0]) *
                 (curve[i][1] - curve[i - 1][1])) / span);
        }
    }
    return 100;
}
