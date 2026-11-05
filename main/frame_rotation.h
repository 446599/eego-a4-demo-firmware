#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a4_display_geometry.h"

enum {
    A4_PANEL_WIDTH_PX = UI_CANVAS_HEIGHT_PX,
    A4_PANEL_HEIGHT_PX = UI_CANVAS_WIDTH_PX,
    A4_PANEL_STRIDE_BYTES = A4_PANEL_WIDTH_PX / 8,
};

typedef struct {
    int x;
    int y;
    int width;
    int height;
} a4_dirty_rect_t;

typedef struct {
    uint32_t ww;
    uint32_t kw;
    uint32_t wk;
    uint32_t kk;
} a4_transition_stats_t;

/* Convert a portrait half-open UI rectangle to the byte-aligned panel dirty
 * rectangle used by the existing full-frame DU submission.  This is only a
 * coordinate helper; it does not enable controller partial refresh. */
static inline a4_dirty_rect_t a4_rotate_portrait_dirty_to_panel(
    a4_viewport_rect_t portrait, bool clockwise)
{
    int x0 = portrait.x, y0 = portrait.y;
    int x1 = portrait.x + portrait.width, y1 = portrait.y + portrait.height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > UI_CANVAS_WIDTH_PX) x1 = UI_CANVAS_WIDTH_PX;
    if (y1 > UI_CANVAS_HEIGHT_PX) y1 = UI_CANVAS_HEIGHT_PX;
    if (x1 <= x0 || y1 <= y0) return (a4_dirty_rect_t){0, 0, 0, 0};
    int px0, px1, py0, py1;
    if (clockwise) {
        px0 = UI_CANVAS_HEIGHT_PX - y1;
        px1 = UI_CANVAS_HEIGHT_PX - y0;
        py0 = x0;
        py1 = x1;
    } else {
        px0 = y0;
        px1 = y1;
        py0 = UI_CANVAS_WIDTH_PX - x1;
        py1 = UI_CANVAS_WIDTH_PX - x0;
    }
    /* Panel dirty rectangles are expressed in pixels, with x aligned to
     * panel bytes just like a4_rotate_to_panel_dirty(). */
    px0 &= ~7;
    px1 = (px1 + 7) & ~7;
    if (px0 < 0) px0 = 0;
    if (px1 > A4_PANEL_WIDTH_PX) px1 = A4_PANEL_WIDTH_PX;
    return (a4_dirty_rect_t){px0, py0, px1 - px0, py1 - py0};
}

/* Transpose one MSB-first 8x8 bitmap tile. The returned word contains the
 * eight output bytes from most-significant to least-significant. */
static inline uint64_t a4_transpose_8x8(uint64_t bits)
{
    uint64_t swap =
        (bits ^ (bits >> 7)) & UINT64_C(0x00aa00aa00aa00aa);
    bits ^= swap ^ (swap << 7);
    swap = (bits ^ (bits >> 14)) & UINT64_C(0x0000cccc0000cccc);
    bits ^= swap ^ (swap << 14);
    swap = (bits ^ (bits >> 28)) & UINT64_C(0x00000000f0f0f0f0);
    return bits ^ swap ^ (swap << 28);
}
/* Rotate the portrait 552x768 UI bitmap into the panel's 768x552 raster.
 * Processing 8x8 tiles replaces the previous 424k per-pixel coordinate and
 * bit operations while producing byte-identical output. */
static inline void a4_rotate_to_panel(const uint8_t *ui,
                                      uint8_t *panel,
                                      bool clockwise)
{
    for (int source_byte_x = 0;
         source_byte_x < UI_FRAMEBUFFER_STRIDE_BYTES;
         ++source_byte_x) {
        for (int source_tile_y = 0;
             source_tile_y < UI_CANVAS_HEIGHT_PX / 8;
             ++source_tile_y) {
            uint64_t rows = 0;
            for (int row = 0; row < 8; ++row) {
                const int source_row =
                    source_tile_y * 8 + (clockwise ? 7 - row : row);
                rows = (rows << 8) |
                       ui[(size_t)source_row * UI_FRAMEBUFFER_STRIDE_BYTES +
                          source_byte_x];
            }
            const uint64_t columns = a4_transpose_8x8(rows);
            for (int column = 0; column < 8; ++column) {
                const int panel_row = clockwise
                                          ? source_byte_x * 8 + column
                                          : A4_PANEL_HEIGHT_PX - 1 -
                                                (source_byte_x * 8 + column);
                const int panel_byte_x = clockwise
                                             ? A4_PANEL_STRIDE_BYTES - 1 -
                                                   source_tile_y
                                             : source_tile_y;
                panel[(size_t)panel_row * A4_PANEL_STRIDE_BYTES +
                      panel_byte_x] =
                    (uint8_t)(columns >> (56 - column * 8));
            }
        }
    }
}

/* Rotate and compare each completed panel byte against the previous frame.
 * The destination is updated exactly like a4_rotate_to_panel(); the returned
 * rectangle is byte-aligned in panel coordinates and is empty when unchanged.
 * This avoids a second full-size framebuffer solely for dirty tracking. */
static inline bool a4_rotate_to_panel_dirty(const uint8_t *ui,
                                           uint8_t *panel,
                                           bool clockwise,
                                           a4_dirty_rect_t *out_rect,
                                           a4_transition_stats_t *out_stats)
{
    if (out_rect != NULL) {
        *out_rect = (a4_dirty_rect_t){0, 0, 0, 0};
    }
    if (out_stats != NULL) {
        *out_stats = (a4_transition_stats_t){0, 0, 0, 0};
    }
    int min_x = A4_PANEL_STRIDE_BYTES;
    int max_x = -1;
    int min_y = A4_PANEL_HEIGHT_PX;
    int max_y = -1;
    for (int source_byte_x = 0;
         source_byte_x < UI_FRAMEBUFFER_STRIDE_BYTES;
         ++source_byte_x) {
        for (int source_tile_y = 0;
             source_tile_y < UI_CANVAS_HEIGHT_PX / 8;
             ++source_tile_y) {
            uint64_t rows = 0;
            for (int row = 0; row < 8; ++row) {
                const int source_row =
                    source_tile_y * 8 + (clockwise ? 7 - row : row);
                rows = (rows << 8) |
                       ui[(size_t)source_row * UI_FRAMEBUFFER_STRIDE_BYTES +
                          source_byte_x];
            }
            const uint64_t columns = a4_transpose_8x8(rows);
            for (int column = 0; column < 8; ++column) {
                const int panel_row = clockwise
                                          ? source_byte_x * 8 + column
                                          : A4_PANEL_HEIGHT_PX - 1 -
                                                (source_byte_x * 8 + column);
                const int panel_byte_x = clockwise
                                             ? A4_PANEL_STRIDE_BYTES - 1 -
                                                   source_tile_y
                                             : source_tile_y;
                const uint8_t value =
                    (uint8_t)(columns >> (56 - column * 8));
                uint8_t *const destination =
                    &panel[(size_t)panel_row * A4_PANEL_STRIDE_BYTES +
                           panel_byte_x];
                const uint8_t previous = *destination;
                if (out_stats != NULL) {
                    out_stats->ww +=
                        (uint32_t)__builtin_popcount((unsigned)(previous & value));
                    out_stats->kw +=
                        (uint32_t)__builtin_popcount(
                            (unsigned)((uint8_t)~previous & value));
                    out_stats->wk +=
                        (uint32_t)__builtin_popcount(
                            (unsigned)(previous & (uint8_t)~value));
                    out_stats->kk +=
                        (uint32_t)__builtin_popcount(
                            (unsigned)((uint8_t)~previous & (uint8_t)~value));
                }
                if (*destination != value) {
                    *destination = value;
                    if (panel_byte_x < min_x) min_x = panel_byte_x;
                    if (panel_byte_x > max_x) max_x = panel_byte_x;
                    if (panel_row < min_y) min_y = panel_row;
                    if (panel_row > max_y) max_y = panel_row;
                }
            }
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return false;
    }
    if (out_rect != NULL) {
        out_rect->x = min_x * 8;
        out_rect->y = min_y;
        out_rect->width = (max_x - min_x + 1) * 8;
        out_rect->height = max_y - min_y + 1;
    }
    return true;
}
