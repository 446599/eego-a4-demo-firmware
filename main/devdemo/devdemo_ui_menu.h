#pragma once

#include <stdbool.h>

#include "devdemo_app.h"

#define DEVDEMO_MENU_ITEMS_PER_PAGE 8
#define DEVDEMO_MENU_HIT_NONE (-1)
#define DEVDEMO_MENU_HIT_PREV (-2)
#define DEVDEMO_MENU_HIT_NEXT (-3)

int devdemo_menu_leaf_count(void);
int devdemo_menu_leaf_for_app(const devdemo_app_t *app);
void devdemo_menu_draw(uint8_t *fb, const devdemo_app_t *current, int leaf,
                       int highlight);
int devdemo_menu_hit_test(int x, int y, int leaf);
bool devdemo_menu_handle_hit_test(int x, int y);
