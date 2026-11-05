#pragma once

enum {
    UI_CANVAS_WIDTH_PX = 552,
    UI_CANVAS_HEIGHT_PX = 768,
    UI_FRAMEBUFFER_STRIDE_BYTES = UI_CANVAS_WIDTH_PX / 8,
    UI_FRAMEBUFFER_SIZE_BYTES =
        UI_FRAMEBUFFER_STRIDE_BYTES * UI_CANVAS_HEIGHT_PX,
};

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} a4_viewport_rect_t;
