#include "devdemo_app_loop.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a4_board.h"
#include "devdemo_app_registry.h"
#include "devdemo_ui.h"
#include "devdemo_ui_menu.h"
#include "display_manager.h"
#include "epd_uc8279c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "frame_rotation.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch_calibration.h"

static const char *TAG = "devdemo_loop";

enum {
    LOOP_TICK_MS = 20,
    BUTTON_DEBOUNCE_SAMPLES = 5,
    TOUCH_INVALID_RAW_X = 920,
    FULL_REFRESH_EVERY = 8,
};

typedef struct {
    bool stable;
    bool candidate;
    uint8_t samples;
} button_state_t;

typedef struct {
    bool full;
    bool partial;
    int x;
    int y;
    int width;
    int height;
} refresh_request_t;

static uint8_t *s_ui_fb;
static uint8_t *s_panel_fb;
static bool s_panel_valid;
static bool s_force_full;
static uint8_t s_soft_since_full;
static int64_t s_last_refresh_us;

static bool s_menu_open;
static int s_menu_leaf;
static int s_menu_highlight;

static esp_err_t refresh_operation(void *context)
{
    const refresh_request_t *const request = context;
    if (request == NULL || s_panel_fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->partial) {
        return epd_uc8279c_show_frame_fast_partial(
            s_panel_fb, EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES, request->x,
            request->y, request->width, request->height);
    }
    return request->full
               ? epd_uc8279c_show_frame(
                     s_panel_fb, EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES)
               : epd_uc8279c_show_frame_fast(
                     s_panel_fb, EPD_UC8279C_FRAMEBUFFER_SIZE_BYTES);
}

void devdemo_present_framebuffer(devdemo_ctx_t *ctx, bool full, bool force_fast)
{
    full = full || !s_panel_valid || s_force_full ||
           (!force_fast &&
            s_soft_since_full + 1U >= FULL_REFRESH_EVERY);
    a4_rotate_to_panel(s_ui_fb, s_panel_fb, false);
    refresh_request_t request = {.full = full};
    const int64_t started_us = esp_timer_get_time();
    const esp_err_t err = display_manager_run(refresh_operation, &request);
    s_last_refresh_us = esp_timer_get_time();

    ctx->last_refresh_ms =
        (uint32_t)((s_last_refresh_us - started_us + 500) / 1000);
    if (err != ESP_OK) {
        s_force_full = true;
        ESP_LOGE(TAG, "display refresh failed: %s", esp_err_to_name(err));
        return;
    }
    s_panel_valid = true;
    s_force_full = false;
    if (full) {
        ++ctx->gc_refresh_count;
        s_soft_since_full = 0;
    } else {
        ++ctx->fast_refresh_count;
        ++s_soft_since_full;
    }
    ESP_LOGI(TAG, "refresh=%s gc=%" PRIu32 " fast=%" PRIu32
                  " elapsed=%" PRIu32 "ms",
             full ? "GC" : "fast", ctx->gc_refresh_count,
             ctx->fast_refresh_count, ctx->last_refresh_ms);
}

static void present_area(devdemo_ctx_t *ctx, devdemo_area_t area)
{
    if (!s_panel_valid || s_force_full) {
        devdemo_present_framebuffer(ctx, true, false);
        return;
    }
    const a4_viewport_rect_t portrait = {
        .x = (int16_t)area.x,
        .y = (int16_t)area.y,
        .width = (int16_t)area.width,
        .height = (int16_t)area.height,
    };
    const a4_dirty_rect_t panel =
        a4_rotate_portrait_dirty_to_panel(portrait, false);
    if (panel.width <= 0 || panel.height <= 0) {
        devdemo_present_framebuffer(ctx, false, false);
        return;
    }

    a4_rotate_to_panel(s_ui_fb, s_panel_fb, false);
    refresh_request_t request = {
        .partial = true,
        .x = panel.x,
        .y = panel.y,
        .width = panel.width,
        .height = panel.height,
    };
    const int64_t started_us = esp_timer_get_time();
    const esp_err_t err = display_manager_run(refresh_operation, &request);
    s_last_refresh_us = esp_timer_get_time();
    ctx->last_refresh_ms =
        (uint32_t)((s_last_refresh_us - started_us + 500) / 1000);
    if (err != ESP_OK) {
        s_force_full = true;
        ESP_LOGE(TAG, "partial refresh failed: %s", esp_err_to_name(err));
        return;
    }
    ++ctx->fast_refresh_count;
    ++s_soft_since_full;
    ESP_LOGI(TAG, "refresh=AREA x=%d y=%d w=%d h=%d fast=%" PRIu32
                  " elapsed=%" PRIu32 "ms",
             panel.x, panel.y, panel.width, panel.height,
             ctx->fast_refresh_count, ctx->last_refresh_ms);
}

void devdemo_present(devdemo_ctx_t *ctx, const devdemo_app_t *app,
                     devdemo_redraw_t redraw)
{
    if (ctx == NULL || app == NULL || s_menu_open ||
        redraw == DEVDEMO_REDRAW_NONE || redraw == DEVDEMO_REDRAW_DONE) {
        return;
    }
    if (app->render != NULL) {
        app->render(ctx);
    }
    devdemo_ui_menu_handle(ctx->fb, false);
    if (redraw == DEVDEMO_REDRAW_AREA && app->area_hint != NULL) {
        present_area(ctx, app->area_hint(ctx));
        return;
    }
    devdemo_present_framebuffer(ctx, redraw == DEVDEMO_REDRAW_FULL,
                                redraw == DEVDEMO_REDRAW_FAST);
}

static void draw_menu(devdemo_ctx_t *ctx, bool force_full)
{
    devdemo_menu_draw(ctx->fb, ctx->current, s_menu_leaf, s_menu_highlight);
    devdemo_present_framebuffer(ctx, force_full, false);
}

static void open_menu(devdemo_ctx_t *ctx)
{
    const int current_index = devdemo_app_index_of(ctx->current);
    if (current_index < 0) {
        return;
    }
    s_menu_open = true;
    s_menu_leaf = current_index / DEVDEMO_MENU_ITEMS_PER_PAGE;
    s_menu_highlight = current_index;
    draw_menu(ctx, true);
}

static void switch_page(devdemo_ctx_t *ctx, const devdemo_app_t *next)
{
    if (ctx == NULL || next == NULL || next == ctx->current) {
        return;
    }
    if (ctx->current != NULL && ctx->current->on_exit != NULL) {
        ctx->current->on_exit(ctx);
    }
    ctx->current = next;
    ctx->request_app = NULL;
    ctx->request_menu = false;
    s_menu_open = false;
    if (next->on_enter != NULL) {
        next->on_enter(ctx);
    }
    devdemo_present(ctx, next,
                    next->enter_full ? DEVDEMO_REDRAW_FULL
                                     : DEVDEMO_REDRAW_PAGE);
    ESP_LOGI(TAG, "page -> %s", next->title);
}

static void select_menu_index(devdemo_ctx_t *ctx, int index)
{
    const devdemo_app_t *const next = devdemo_app_at(index);
    if (next == ctx->current) {
        s_menu_open = false;
        devdemo_present(ctx, ctx->current, DEVDEMO_REDRAW_FULL);
    } else if (next != NULL) {
        switch_page(ctx, next);
    }
}

static void menu_move(devdemo_ctx_t *ctx, int direction)
{
    const int count = devdemo_app_count();
    if (count <= 0) {
        return;
    }
    s_menu_highlight =
        (s_menu_highlight + direction + count) % count;
    s_menu_leaf =
        s_menu_highlight / DEVDEMO_MENU_ITEMS_PER_PAGE;
    draw_menu(ctx, false);
}

static void handle_menu_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_menu_handle_hit_test(ctx->touch_x, ctx->touch_y)) {
        s_menu_open = false;
        devdemo_present(ctx, ctx->current, DEVDEMO_REDRAW_FULL);
        return;
    }
    const int hit =
        devdemo_menu_hit_test(ctx->touch_x, ctx->touch_y, s_menu_leaf);
    if (hit == DEVDEMO_MENU_HIT_PREV && s_menu_leaf > 0) {
        --s_menu_leaf;
        s_menu_highlight =
            s_menu_leaf * DEVDEMO_MENU_ITEMS_PER_PAGE;
        draw_menu(ctx, false);
    } else if (hit == DEVDEMO_MENU_HIT_NEXT &&
               s_menu_leaf + 1 < devdemo_menu_leaf_count()) {
        ++s_menu_leaf;
        s_menu_highlight =
            s_menu_leaf * DEVDEMO_MENU_ITEMS_PER_PAGE;
        draw_menu(ctx, false);
    } else if (hit >= 0) {
        s_menu_highlight = hit;
        select_menu_index(ctx, hit);
    }
}

static void dispatch_result(devdemo_ctx_t *ctx, devdemo_redraw_t redraw)
{
    if (ctx->request_app != NULL) {
        const devdemo_app_t *const next = ctx->request_app;
        ctx->request_app = NULL;
        switch_page(ctx, next);
        return;
    }
    if (ctx->request_menu) {
        ctx->request_menu = false;
        open_menu(ctx);
        return;
    }
    if (s_menu_open) {
        return;
    }
    devdemo_present(ctx, ctx->current, redraw);
}

static void poll_touch(devdemo_ctx_t *ctx, bool *was_touching)
{
    uint8_t fingers = 0;
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    const bool touching =
        a4_board_read_touch(&fingers, &raw_x, &raw_y) && fingers > 0U &&
        raw_x < TOUCH_INVALID_RAW_X;

    ctx->touching = touching;
    ctx->pressed = touching && !*was_touching;
    ctx->released = !touching && *was_touching;
    ctx->fingers = touching ? fingers : 0;
    if (touching) {
        touch_calibration_map(raw_x, raw_y, &ctx->touch_x, &ctx->touch_y);
        ctx->raw_x = raw_x;
        ctx->raw_y = raw_y;
        if (ctx->pressed) {
            ++ctx->touch_taps;
        }
    }
    *was_touching = touching;
}

static void initialize_buttons(button_state_t states[DEVDEMO_KEY_COUNT])
{
    static const a4_button_t buttons[DEVDEMO_KEY_COUNT] = {
        A4_BUTTON_UP,
        A4_BUTTON_DOWN,
        A4_BUTTON_POWER,
    };
    for (int index = 0; index < DEVDEMO_KEY_COUNT; ++index) {
        const bool pressed = a4_board_button_pressed(buttons[index]);
        states[index].stable = pressed;
        states[index].candidate = pressed;
        states[index].samples = BUTTON_DEBOUNCE_SAMPLES;
    }
}

static void poll_buttons(devdemo_ctx_t *ctx,
                         button_state_t states[DEVDEMO_KEY_COUNT])
{
    static const a4_button_t buttons[DEVDEMO_KEY_COUNT] = {
        A4_BUTTON_UP,
        A4_BUTTON_DOWN,
        A4_BUTTON_POWER,
    };
    ctx->key_down_mask = 0;
    ctx->key_pressed_mask = 0;
    ctx->key_released_mask = 0;
    for (int index = 0; index < DEVDEMO_KEY_COUNT; ++index) {
        const bool pressed = a4_board_button_pressed(buttons[index]);
        button_state_t *const state = &states[index];
        if (pressed != state->candidate) {
            state->candidate = pressed;
            state->samples = 0;
        }
        if (state->samples < BUTTON_DEBOUNCE_SAMPLES) {
            ++state->samples;
        }
        if (state->samples >= BUTTON_DEBOUNCE_SAMPLES &&
            state->stable != state->candidate) {
            state->stable = state->candidate;
            const uint8_t bit = (uint8_t)(1U << index);
            if (state->stable) {
                ctx->key_pressed_mask |= bit;
                ++ctx->key_press_count[index];
            } else {
                ctx->key_released_mask |= bit;
            }
        }
        if (state->stable) {
            ctx->key_down_mask |= (uint8_t)(1U << index);
        }
    }
}

static void handle_keys(devdemo_ctx_t *ctx)
{
    if (s_menu_open) {
        if ((ctx->key_pressed_mask & (1U << DEVDEMO_KEY_POWER)) != 0) {
            select_menu_index(ctx, s_menu_highlight);
        } else if ((ctx->key_pressed_mask & (1U << DEVDEMO_KEY_UP)) != 0) {
            menu_move(ctx, -1);
        } else if ((ctx->key_pressed_mask &
                    (1U << DEVDEMO_KEY_DOWN)) != 0) {
            menu_move(ctx, 1);
        }
        return;
    }

    if (ctx->current->owns_keys) {
        if ((ctx->key_pressed_mask != 0 || ctx->key_released_mask != 0) &&
            ctx->current->on_key != NULL) {
            int changed_key = DEVDEMO_KEY_UP;
            for (int key = 0; key < DEVDEMO_KEY_COUNT; ++key) {
                const uint8_t bit = (uint8_t)(1U << key);
                if ((ctx->key_pressed_mask & bit) != 0 ||
                    (ctx->key_released_mask & bit) != 0) {
                    changed_key = key;
                    break;
                }
            }
            dispatch_result(ctx,
                            ctx->current->on_key(ctx, changed_key));
        }
        return;
    }

    if ((ctx->key_pressed_mask & (1U << DEVDEMO_KEY_POWER)) != 0) {
        open_menu(ctx);
        return;
    }
    if ((ctx->key_pressed_mask & (1U << DEVDEMO_KEY_DOWN)) != 0) {
        devdemo_present(ctx, ctx->current, DEVDEMO_REDRAW_FULL);
        return;
    }
    if ((ctx->key_pressed_mask & (1U << DEVDEMO_KEY_UP)) != 0 &&
        ctx->current->on_key != NULL) {
        dispatch_result(ctx,
                        ctx->current->on_key(ctx, DEVDEMO_KEY_UP));
    }
}

static void handle_touch(devdemo_ctx_t *ctx)
{
    if (!ctx->pressed && !ctx->released) {
        return;
    }
    if (s_menu_open) {
        if (ctx->pressed) {
            handle_menu_touch(ctx);
        }
        return;
    }
    if (ctx->pressed &&
        devdemo_menu_handle_hit_test(ctx->touch_x, ctx->touch_y)) {
        open_menu(ctx);
        return;
    }
    if (ctx->current->on_touch != NULL) {
        dispatch_result(ctx, ctx->current->on_touch(ctx));
    }
}

void devdemo_app_loop_run(const devdemo_app_t *first_app, uint8_t *ui_fb,
                          uint8_t *panel_fb)
{
    if (ui_fb == NULL || panel_fb == NULL) {
        ESP_LOGE(TAG, "framebuffers are required");
        return;
    }
    s_ui_fb = ui_fb;
    s_panel_fb = panel_fb;
    s_panel_valid = true;

    devdemo_ctx_t ctx = {
        .fb = s_ui_fb,
        .now_ms = esp_timer_get_time() / 1000,
        .current = first_app != NULL ? first_app : devdemo_overview_page(),
    };
    bool was_touching = false;
    button_state_t button_states[DEVDEMO_KEY_COUNT] = {0};
    initialize_buttons(button_states);

    if (ctx.current->on_enter != NULL) {
        ctx.current->on_enter(&ctx);
    }
    devdemo_present(&ctx, ctx.current, DEVDEMO_REDRAW_FULL);
    ESP_LOGI(TAG, "UI ready on %s", ctx.current->title);

    while (true) {
        ctx.now_ms = esp_timer_get_time() / 1000;
        ctx.request_app = NULL;
        ctx.request_menu = false;
        poll_buttons(&ctx, button_states);
        poll_touch(&ctx, &was_touching);

        handle_keys(&ctx);
        handle_touch(&ctx);
        if (ctx.current->on_tick != NULL) {
            dispatch_result(&ctx, ctx.current->on_tick(&ctx));
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_refresh_us > 8000000LL && s_force_full &&
            !s_menu_open) {
            devdemo_present(&ctx, ctx.current, DEVDEMO_REDRAW_FULL);
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
