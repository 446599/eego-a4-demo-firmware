#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct devdemo_app_s devdemo_app_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} devdemo_area_t;

typedef enum {
    DEVDEMO_REDRAW_NONE = 0,
    DEVDEMO_REDRAW_AREA,
    DEVDEMO_REDRAW_PAGE,
    DEVDEMO_REDRAW_FULL,
    DEVDEMO_REDRAW_FAST,
    DEVDEMO_REDRAW_DONE,
} devdemo_redraw_t;

enum {
    DEVDEMO_KEY_UP = 0,
    DEVDEMO_KEY_DOWN,
    DEVDEMO_KEY_POWER,
    DEVDEMO_KEY_COUNT,
};

typedef struct {
    uint8_t *fb;
    int64_t now_ms;

    bool touching;
    bool pressed;
    bool released;
    uint8_t fingers;
    uint16_t raw_x;
    uint16_t raw_y;
    int16_t touch_x;
    int16_t touch_y;
    uint32_t touch_taps;

    uint8_t key_down_mask;
    uint8_t key_pressed_mask;
    uint8_t key_released_mask;
    uint32_t key_press_count[DEVDEMO_KEY_COUNT];

    uint32_t gc_refresh_count;
    uint32_t fast_refresh_count;
    uint32_t last_refresh_ms;

    const devdemo_app_t *current;
    const devdemo_app_t *request_app;
    bool request_menu;
} devdemo_ctx_t;

struct devdemo_app_s {
    const char *title;
    const char *detail;
    bool enter_full;
    bool owns_keys;
    void (*on_enter)(devdemo_ctx_t *ctx);
    void (*on_exit)(devdemo_ctx_t *ctx);
    void (*render)(devdemo_ctx_t *ctx);
    devdemo_area_t (*area_hint)(devdemo_ctx_t *ctx);
    devdemo_redraw_t (*on_touch)(devdemo_ctx_t *ctx);
    devdemo_redraw_t (*on_key)(devdemo_ctx_t *ctx, int key);
    devdemo_redraw_t (*on_tick)(devdemo_ctx_t *ctx);
};

/* Draw and present the requested page. Pages may call this from on_touch for
 * custom refresh bench operations, then return DEVDEMO_REDRAW_DONE. */
void devdemo_present(devdemo_ctx_t *ctx, const devdemo_app_t *app,
                     devdemo_redraw_t redraw);
void devdemo_present_framebuffer(devdemo_ctx_t *ctx, bool full, bool force_fast);
