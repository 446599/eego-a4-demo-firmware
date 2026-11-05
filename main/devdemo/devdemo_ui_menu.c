#include "devdemo_ui_menu.h"

#include <stdio.h>

#include "devdemo_app_registry.h"
#include "devdemo_ui.h"

int devdemo_menu_leaf_count(void)
{
    const int count = devdemo_app_count();
    return (count + DEVDEMO_MENU_ITEMS_PER_PAGE - 1) /
           DEVDEMO_MENU_ITEMS_PER_PAGE;
}

int devdemo_menu_leaf_for_app(const devdemo_app_t *app)
{
    const int index = devdemo_app_index_of(app);
    return index >= 0 ? index / DEVDEMO_MENU_ITEMS_PER_PAGE : 0;
}

static int leaf_first(int leaf)
{
    return leaf * DEVDEMO_MENU_ITEMS_PER_PAGE;
}

static int leaf_count(int leaf)
{
    const int remaining = devdemo_app_count() - leaf_first(leaf);
    if (remaining < 0) {
        return 0;
    }
    return remaining > DEVDEMO_MENU_ITEMS_PER_PAGE
               ? DEVDEMO_MENU_ITEMS_PER_PAGE
               : remaining;
}

static devdemo_rect_t menu_row_rect(int row)
{
    const int top = DEVDEMO_UI_CONTENT_TOP;
    const int height =
        (DEVDEMO_UI_CONTENT_BOTTOM - top) /
        DEVDEMO_MENU_ITEMS_PER_PAGE;
    return (devdemo_rect_t){
        .x = DEVDEMO_UI_MARGIN,
        .y = top + row * height,
        .width = UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN,
        .height = height - DEVDEMO_UI_GAP,
    };
}

static devdemo_rect_t nav_rect(bool next)
{
    return devdemo_ui_bar_rect(next ? 1 : 0, 2);
}

void devdemo_menu_draw(uint8_t *fb, const devdemo_app_t *current, int leaf,
                       int highlight)
{
    char line[96];
    devdemo_ui_clear(fb);
    (void)snprintf(line, sizeof(line), "Page %d / %d  |  UP/DOWN then POWER",
                   leaf + 1, devdemo_menu_leaf_count());
    devdemo_ui_header(fb, "EEGO A4 DEVELOPER DEMO", line);

    const int first = leaf_first(leaf);
    const int count = leaf_count(leaf);
    for (int row = 0; row < count; ++row) {
        const int index = first + row;
        const devdemo_app_t *const app = devdemo_app_at(index);
        const devdemo_rect_t rect = menu_row_rect(row);
        const bool selected = index == highlight;
        const bool current_app = app == current;
        devdemo_ui_stroke_rect(fb, rect, selected ? 2 : 1, true);
        (void)snprintf(line, sizeof(line), "%d", index + 1);
        devdemo_ui_text(fb, rect.x + 14, rect.y + 7, 28, line, true);
        devdemo_ui_text_fit(fb, rect.x + 64, rect.y + 4, rect.width - 76,
                            28, app->title, true);
        devdemo_ui_text_fit(fb, rect.x + 64, rect.y + 33, rect.width - 76,
                            28, app->detail, true);
        if (current_app) {
            devdemo_ui_fill_rect(
                fb,
                (devdemo_rect_t){rect.x + rect.width - 18, rect.y + 10, 8, 8},
                true);
        }
    }

    devdemo_ui_button(fb, nav_rect(false), "PREV", leaf > 0);
    devdemo_ui_button(fb, nav_rect(true), "NEXT",
                      leaf + 1 < devdemo_menu_leaf_count());
    devdemo_ui_menu_handle(fb, true);
}

int devdemo_menu_hit_test(int x, int y, int leaf)
{
    if (devdemo_ui_rect_hit(nav_rect(false), x, y)) {
        return leaf > 0 ? DEVDEMO_MENU_HIT_PREV : DEVDEMO_MENU_HIT_NONE;
    }
    if (devdemo_ui_rect_hit(nav_rect(true), x, y)) {
        return leaf + 1 < devdemo_menu_leaf_count()
                   ? DEVDEMO_MENU_HIT_NEXT
                   : DEVDEMO_MENU_HIT_NONE;
    }
    const int count = leaf_count(leaf);
    for (int row = 0; row < count; ++row) {
        if (devdemo_ui_rect_hit(menu_row_rect(row), x, y)) {
            return leaf_first(leaf) + row;
        }
    }
    return DEVDEMO_MENU_HIT_NONE;
}

bool devdemo_menu_handle_hit_test(int x, int y)
{
    return devdemo_ui_rect_hit(devdemo_ui_menu_handle_rect(), x, y);
}
