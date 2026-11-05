#include <stdio.h>

#include "devdemo_app.h"
#include "devdemo_ui.h"

static const char *const s_labels[DEVDEMO_KEY_COUNT] = {
    "UP GPIO5",
    "DOWN GPIO7",
    "POWER GPIO8",
};

static devdemo_area_t keys_area_hint(devdemo_ctx_t *ctx)
{
    (void)ctx;
    return (devdemo_area_t){
        .x = DEVDEMO_UI_MARGIN - 4,
        .y = DEVDEMO_UI_CONTENT_TOP - 4,
        .width = UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN + 8,
        .height = 5 * DEVDEMO_UI_ROW_HEIGHT + 46,
    };
}

static void keys_render(devdemo_ctx_t *ctx)
{
    char line[64];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "Hardware Keys",
                      "UP=page action, DOWN=full refresh, POWER=menu");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "LIVE STATE");
    for (int key = 0; key < DEVDEMO_KEY_COUNT; ++key) {
        (void)snprintf(line, sizeof(line), "%s  count=%lu",
                       (ctx->key_down_mask & (1U << key)) != 0 ? "DOWN" : "UP",
                       (unsigned long)ctx->key_press_count[key]);
        y = devdemo_ui_row(ctx->fb, y, s_labels[key], line);
    }

    y = devdemo_ui_section(ctx->fb, y + 18, "INPUT MAP");
    y = devdemo_ui_row(ctx->fb, y, "UP", "page-specific action");
    y = devdemo_ui_row(ctx->fb, y, "DOWN", "force full GC refresh");
    y = devdemo_ui_row(ctx->fb, y, "POWER", "open/close menu");
    y = devdemo_ui_row(ctx->fb, y, "Menu", "UP/DOWN move, POWER select");
    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, y + 20,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "This page owns all three keys and redraws on press and release.",
        true);
}

static devdemo_redraw_t keys_key(devdemo_ctx_t *ctx, int key)
{
    (void)ctx;
    (void)key;
    return DEVDEMO_REDRAW_AREA;
}

const devdemo_app_t devdemo_app_keys = {
    .title = "Hardware Keys",
    .detail = "GPIO levels, counts and global actions",
    .owns_keys = true,
    .render = keys_render,
    .area_hint = keys_area_hint,
    .on_key = keys_key,
};
