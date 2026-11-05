#pragma once

#include <stdint.h>

enum {
    TOUCH_CALIBRATION_OUTPUT_WIDTH_PX = 552,
    TOUCH_CALIBRATION_OUTPUT_HEIGHT_PX = 768,
};

typedef struct {
    int32_t x;
    int32_t y;
} touch_calibration_point_t;

typedef struct {
    touch_calibration_point_t raw;
    touch_calibration_point_t mapped;
} touch_calibration_pair_t;

static inline int64_t touch_calibration_orient(
    touch_calibration_point_t a,
    touch_calibration_point_t b,
    touch_calibration_point_t c)
{
    return (int64_t)(b.x - a.x) * (c.y - a.y) -
           (int64_t)(b.y - a.y) * (c.x - a.x);
}

static inline int32_t touch_calibration_round_div(int64_t numerator,
                                                  int64_t denominator)
{
    if (numerator >= 0) {
        return (int32_t)((numerator + denominator / 2) / denominator);
    }
    return (int32_t)-((-numerator + denominator / 2) / denominator);
}

static inline int32_t touch_calibration_clamp(int32_t value,
                                              int32_t low,
                                              int32_t high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/* Five-point piecewise-affine calibration.
 *
 * A single least-squares affine transform leaves 7-11 px of error on these
 * samples because the mounted GSL sensor has a small trapezoidal/non-linear
 * distortion.  Dividing the calibrated quadrilateral into four triangles
 * around its measured centre maps all five samples exactly while remaining
 * continuous across every shared centre-to-corner edge.
 *
 * The four centre-to-corner rays also divide samples outside the calibrated
 * quadrilateral.  Adjacent affine maps agree along the full shared ray, so
 * extrapolation stays continuous instead of jumping near a screen edge.  The
 * final result is clipped to the portrait 552x768 canvas.  All calculations
 * use int64_t barycentric arithmetic, avoiding floating-point touch work. */
static inline void touch_calibration_map(uint16_t raw_x,
                                         uint16_t raw_y,
                                         int16_t *out_x,
                                         int16_t *out_y)
{
    enum {
        CAL_TL = 0,
        CAL_TR,
        CAL_BR,
        CAL_BL,
        CAL_CENTER,
    };
    static const touch_calibration_pair_t calibration[] = {
        [CAL_TL] = {{720, 94}, {80, 168}},
        [CAL_TR] = {{694, 560}, {472, 168}},
        [CAL_BR] = {{392, 558}, {472, 444}},
        [CAL_BL] = {{374, 97}, {80, 444}},
        [CAL_CENTER] = {{549, 337}, {276, 306}},
    };
    static const uint8_t triangle_corners[][2] = {
        {CAL_TL, CAL_TR},
        {CAL_TR, CAL_BR},
        {CAL_BR, CAL_BL},
        {CAL_BL, CAL_TL},
    };

    const touch_calibration_point_t sample = {
        .x = raw_x,
        .y = raw_y,
    };
    unsigned selected =
        (unsigned)(sizeof(triangle_corners) / sizeof(triangle_corners[0]) - 1);

    for (unsigned triangle = 0;
         triangle < sizeof(triangle_corners) / sizeof(triangle_corners[0]);
         ++triangle) {
        const touch_calibration_point_t p0 = calibration[CAL_CENTER].raw;
        const touch_calibration_point_t p1 =
            calibration[triangle_corners[triangle][0]].raw;
        const touch_calibration_point_t p2 =
            calibration[triangle_corners[triangle][1]].raw;
        /* Corner vectors are counter-clockwise and every wedge is below
         * 180 degrees.  Extending those wedges beyond the measured quad
         * gives continuous extrapolation along their shared boundary rays. */
        if (touch_calibration_orient(p0, p1, sample) >= 0 &&
            touch_calibration_orient(p0, sample, p2) >= 0) {
            selected = triangle;
            break;
        }
    }

    const touch_calibration_point_t raw[3] = {
        calibration[CAL_CENTER].raw,
        calibration[triangle_corners[selected][0]].raw,
        calibration[triangle_corners[selected][1]].raw,
    };
    const touch_calibration_point_t mapped[3] = {
        calibration[CAL_CENTER].mapped,
        calibration[triangle_corners[selected][0]].mapped,
        calibration[triangle_corners[selected][1]].mapped,
    };
    const int64_t selected_denominator =
        touch_calibration_orient(raw[0], raw[1], raw[2]);
    const int64_t selected_weights[3] = {
        touch_calibration_orient(raw[1], raw[2], sample),
        touch_calibration_orient(raw[2], raw[0], sample),
        touch_calibration_orient(raw[0], raw[1], sample),
    };
    const int64_t mapped_x_numerator =
        selected_weights[0] * mapped[0].x +
        selected_weights[1] * mapped[1].x +
        selected_weights[2] * mapped[2].x;
    const int64_t mapped_y_numerator =
        selected_weights[0] * mapped[0].y +
        selected_weights[1] * mapped[1].y +
        selected_weights[2] * mapped[2].y;
    int32_t mapped_x = touch_calibration_round_div(mapped_x_numerator,
                                                   selected_denominator);
    int32_t mapped_y = touch_calibration_round_div(mapped_y_numerator,
                                                   selected_denominator);
    mapped_x = touch_calibration_clamp(
        mapped_x, 0, TOUCH_CALIBRATION_OUTPUT_WIDTH_PX - 1);
    mapped_y = touch_calibration_clamp(
        mapped_y, 0, TOUCH_CALIBRATION_OUTPUT_HEIGHT_PX - 1);

    if (out_x != 0) {
        *out_x = (int16_t)mapped_x;
    }
    if (out_y != 0) {
        *out_y = (int16_t)mapped_y;
    }
}
