#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "a4_display_geometry.h"

enum {
    DEVDEMO_UI_MARGIN = 20,
    DEVDEMO_UI_GAP = 8,
    DEVDEMO_UI_HEADER_RULE_Y = 92,
    DEVDEMO_UI_CONTENT_TOP = 104,
    DEVDEMO_UI_CONTENT_BOTTOM = 690,
    DEVDEMO_UI_BAR_TOP = 704,
    DEVDEMO_UI_BAR_HEIGHT = 48,
    DEVDEMO_UI_MENU_BUTTON = 48,
    DEVDEMO_UI_ROW_HEIGHT = 34,
};

typedef struct {
    int x;
    int y;
    int width;
    int height;
} devdemo_rect_t;

void devdemo_ui_clear(uint8_t *fb);
void devdemo_ui_fill_rect(uint8_t *fb, devdemo_rect_t rect, bool black);
void devdemo_ui_stroke_rect(uint8_t *fb, devdemo_rect_t rect, int width,
                            bool black);
void devdemo_ui_line(uint8_t *fb, int x0, int y0, int x1, int y1,
                     bool black);

int devdemo_ui_text_width(int font_size, const char *text);
void devdemo_ui_text(uint8_t *fb, int x, int y, int font_size,
                     const char *text, bool black);
void devdemo_ui_text_fit(uint8_t *fb, int x, int y, int max_width,
                         int font_size, const char *text, bool black);
void devdemo_ui_text_center(uint8_t *fb, devdemo_rect_t rect, int font_size,
                            const char *text, bool black);

void devdemo_ui_header(uint8_t *fb, const char *title, const char *detail);
int devdemo_ui_section(uint8_t *fb, int y, const char *title);
int devdemo_ui_row(uint8_t *fb, int y, const char *label, const char *value);
int devdemo_ui_row2(uint8_t *fb, int y, const char *label1, const char *value1,
                    const char *label2, const char *value2);
void devdemo_ui_button(uint8_t *fb, devdemo_rect_t rect, const char *label,
                       bool selected);
devdemo_rect_t devdemo_ui_slider_track(devdemo_rect_t band);
int devdemo_ui_slider_percent_at(devdemo_rect_t band, int x);
void devdemo_ui_slider(uint8_t *fb, devdemo_rect_t band, const char *label,
                       int percent, bool active);
void devdemo_ui_menu_handle(uint8_t *fb, bool menu_open);
void devdemo_ui_checker(uint8_t *fb, devdemo_rect_t rect, int cell_size);

bool devdemo_ui_rect_hit(devdemo_rect_t rect, int x, int y);
devdemo_rect_t devdemo_ui_grid_rect(int column, int columns, int row, int y,
                                    int height);
devdemo_rect_t devdemo_ui_bar_rect(int column, int columns);
devdemo_rect_t devdemo_ui_menu_handle_rect(void);
