#include "devdemo_ui.h"

#include <stdio.h>
#include <string.h>

#include "demo_fonts.h"

extern const ui_font_t ui_font_title;
extern const ui_font_t ui_font_body;
extern const ui_font_t ui_font_small;

enum {
    FONT_SMALL = 28,
    FONT_BODY = 38,
    FONT_TITLE = 43,
};

static const ui_font_t *font_for_size(int font_size)
{
    if (font_size >= FONT_TITLE) {
        return &ui_font_title;
    }
    if (font_size >= FONT_BODY) {
        return &ui_font_body;
    }
    return &ui_font_small;
}

static void set_pixel(uint8_t *fb, int x, int y, bool black)
{
    if (fb == NULL || x < 0 || x >= UI_CANVAS_WIDTH_PX ||
        y < 0 || y >= UI_CANVAS_HEIGHT_PX) {
        return;
    }
    uint8_t *const byte =
        &fb[(size_t)y * UI_FRAMEBUFFER_STRIDE_BYTES + (x >> 3)];
    const uint8_t mask = (uint8_t)(0x80U >> (x & 7));
    if (black) {
        *byte &= (uint8_t)~mask;
    } else {
        *byte |= mask;
    }
}

static const ui_glyph_t *font_glyph(const ui_font_t *font, uint32_t cp)
{
    if (font == NULL || font->glyphs == NULL || font->codepoints == NULL ||
        cp < 32U || cp > 126U || font->glyph_count <= cp - 32U ||
        font->codepoints[0] != 32U) {
        return NULL;
    }
    return &font->glyphs[cp - 32U];
}

static void draw_glyph(uint8_t *fb, const ui_font_t *font,
                       const ui_glyph_t *glyph, int x, int y, bool black)
{
    if (font == NULL || glyph == NULL) {
        return;
    }
    const bool coverage_font = font->bits_per_pixel == 2U;
    const int stride =
        coverage_font ? (glyph->width + 3) / 4 : (glyph->width + 7) / 8;
    const uint8_t *const bits = font->bitmap + glyph->offset;
    for (int row = 0; row < font->height; ++row) {
        const uint8_t *const source = bits + (size_t)row * stride;
        for (int column = 0; column < glyph->width; ++column) {
            bool ink;
            if (coverage_font) {
                const uint8_t coverage =
                    (source[column >> 2] >> (6 - 2 * (column & 3))) & 3U;
                ink = coverage >= 2U;
            } else {
                ink = (source[column >> 3] &
                       (uint8_t)(0x80U >> (column & 7))) != 0;
            }
            if (ink) {
                set_pixel(fb, x + column, y + row, black);
            }
        }
    }
}

void devdemo_ui_clear(uint8_t *fb)
{
    if (fb != NULL) {
        memset(fb, 0xff, UI_FRAMEBUFFER_SIZE_BYTES);
    }
}

void devdemo_ui_fill_rect(uint8_t *fb, devdemo_rect_t rect, bool black)
{
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }
    const int x0 = rect.x < 0 ? 0 : rect.x;
    const int y0 = rect.y < 0 ? 0 : rect.y;
    const int x1 = rect.x + rect.width > UI_CANVAS_WIDTH_PX
                       ? UI_CANVAS_WIDTH_PX
                       : rect.x + rect.width;
    const int y1 = rect.y + rect.height > UI_CANVAS_HEIGHT_PX
                       ? UI_CANVAS_HEIGHT_PX
                       : rect.y + rect.height;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            set_pixel(fb, x, y, black);
        }
    }
}

void devdemo_ui_stroke_rect(uint8_t *fb, devdemo_rect_t rect, int width,
                            bool black)
{
    if (width <= 0 || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    devdemo_ui_fill_rect(
        fb, (devdemo_rect_t){rect.x, rect.y, rect.width, width}, black);
    devdemo_ui_fill_rect(
        fb,
        (devdemo_rect_t){rect.x, rect.y + rect.height - width, rect.width,
                         width},
        black);
    devdemo_ui_fill_rect(
        fb, (devdemo_rect_t){rect.x, rect.y, width, rect.height}, black);
    devdemo_ui_fill_rect(
        fb,
        (devdemo_rect_t){rect.x + rect.width - width, rect.y, width,
                         rect.height},
        black);
}

void devdemo_ui_line(uint8_t *fb, int x0, int y0, int x1, int y1,
                     bool black)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        set_pixel(fb, x0, y0, black);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

int devdemo_ui_text_width(int font_size, const char *text)
{
    const ui_font_t *const font = font_for_size(font_size);
    int width = 0;
    if (text == NULL) {
        return width;
    }
    while (*text != '\0') {
        const uint32_t cp = (uint8_t)*text++;
        const ui_glyph_t *const glyph = font_glyph(font, cp);
        if (glyph != NULL) {
            width += glyph->width;
        }
    }
    return width;
}

void devdemo_ui_text(uint8_t *fb, int x, int y, int font_size,
                     const char *text, bool black)
{
    const ui_font_t *const font = font_for_size(font_size);
    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        const uint32_t cp = (uint8_t)*text++;
        const ui_glyph_t *const glyph = font_glyph(font, cp);
        if (glyph == NULL) {
            continue;
        }
        draw_glyph(fb, font, glyph, x, y, black);
        x += glyph->width;
    }
}

void devdemo_ui_text_fit(uint8_t *fb, int x, int y, int max_width,
                         int font_size, const char *text, bool black)
{
    if (text == NULL || max_width <= 0) {
        return;
    }
    if (devdemo_ui_text_width(font_size, text) <= max_width) {
        devdemo_ui_text(fb, x, y, font_size, text, black);
        return;
    }

    char fitted[96];
    size_t length = strlen(text);
    if (length >= sizeof(fitted)) {
        length = sizeof(fitted) - 1U;
    }
    memcpy(fitted, text, length);
    fitted[length] = '\0';
    while (length > 0U) {
        fitted[length] = '\0';
        (void)snprintf(fitted + length, sizeof(fitted) - length, "...");
        if (devdemo_ui_text_width(font_size, fitted) <= max_width) {
            devdemo_ui_text(fb, x, y, font_size, fitted, black);
            return;
        }
        --length;
    }
}

void devdemo_ui_text_center(uint8_t *fb, devdemo_rect_t rect, int font_size,
                            const char *text, bool black)
{
    const int width = devdemo_ui_text_width(font_size, text);
    const ui_font_t *const font = font_for_size(font_size);
    const int x = rect.x + (rect.width - width) / 2;
    const int y = rect.y + (rect.height - font->height) / 2;
    devdemo_ui_text_fit(fb, x, y, rect.width, font_size, text, black);
}

void devdemo_ui_header(uint8_t *fb, const char *title, const char *detail)
{
    devdemo_ui_text_fit(fb, DEVDEMO_UI_MARGIN, 10,
                        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN,
                        FONT_TITLE, title, true);
    devdemo_ui_text_fit(fb, DEVDEMO_UI_MARGIN, 57,
                        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN,
                        FONT_SMALL, detail, true);
    devdemo_ui_fill_rect(
        fb,
        (devdemo_rect_t){DEVDEMO_UI_MARGIN, DEVDEMO_UI_HEADER_RULE_Y,
                         UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 1},
        true);
}

int devdemo_ui_section(uint8_t *fb, int y, const char *title)
{
    devdemo_ui_text_fit(fb, DEVDEMO_UI_MARGIN, y,
                        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN,
                        FONT_SMALL, title, true);
    const int line_y = y + FONT_SMALL + 2;
    devdemo_ui_fill_rect(
        fb,
        (devdemo_rect_t){DEVDEMO_UI_MARGIN, line_y,
                         UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 1},
        true);
    return line_y + 7;
}

int devdemo_ui_row(uint8_t *fb, int y, const char *label, const char *value)
{
    devdemo_ui_text_fit(fb, DEVDEMO_UI_MARGIN, y, 230, FONT_SMALL, label,
                        true);
    const int value_width = devdemo_ui_text_width(FONT_SMALL, value);
    const int value_x = UI_CANVAS_WIDTH_PX - DEVDEMO_UI_MARGIN - value_width;
    devdemo_ui_text_fit(fb, value_x, y, 260, FONT_SMALL, value, true);
    const int bottom = y + DEVDEMO_UI_ROW_HEIGHT - 1;
    devdemo_ui_fill_rect(
        fb,
        (devdemo_rect_t){DEVDEMO_UI_MARGIN, bottom,
                         UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 1},
        true);
    return y + DEVDEMO_UI_ROW_HEIGHT;
}

int devdemo_ui_row2(uint8_t *fb, int y, const char *label1,
                    const char *value1, const char *label2,
                    const char *value2)
{
    char left[96];
    char right[96];
    (void)snprintf(left, sizeof(left), "%s %s",
                   label1 != NULL ? label1 : "",
                   value1 != NULL ? value1 : "-");
    (void)snprintf(right, sizeof(right), "%s %s",
                   label2 != NULL ? label2 : "",
                   value2 != NULL ? value2 : "-");
    devdemo_ui_text_fit(fb, DEVDEMO_UI_MARGIN, y, 244, FONT_SMALL, left,
                        true);
    devdemo_ui_text_fit(fb, 282, y, 250, FONT_SMALL, right, true);
    const int bottom = y + DEVDEMO_UI_ROW_HEIGHT - 1;
    devdemo_ui_fill_rect(
        fb,
        (devdemo_rect_t){DEVDEMO_UI_MARGIN, bottom,
                         UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 1},
        true);
    return y + DEVDEMO_UI_ROW_HEIGHT;
}

void devdemo_ui_button(uint8_t *fb, devdemo_rect_t rect, const char *label,
                       bool selected)
{
    if (selected) {
        devdemo_ui_fill_rect(fb, rect, true);
        devdemo_ui_text_center(fb, rect, FONT_SMALL, label, false);
    } else {
        devdemo_ui_stroke_rect(fb, rect, 1, true);
        devdemo_ui_text_center(fb, rect, FONT_SMALL, label, true);
    }
}

devdemo_rect_t devdemo_ui_slider_track(devdemo_rect_t band)
{
    return (devdemo_rect_t){
        .x = band.x + 4,
        .y = band.y + 48,
        .width = band.width - 8,
        .height = 18,
    };
}

int devdemo_ui_slider_percent_at(devdemo_rect_t band, int x)
{
    const devdemo_rect_t track = devdemo_ui_slider_track(band);
    int percent = (x - track.x) * 100 / (track.width - 1);
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    return percent;
}

void devdemo_ui_slider(uint8_t *fb, devdemo_rect_t band, const char *label,
                       int percent, bool active)
{
    char value[8];
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    (void)snprintf(value, sizeof(value), "%d%%", percent);
    devdemo_ui_text_fit(fb, band.x, band.y, band.width - 90, FONT_SMALL,
                        label, true);
    const int value_width = devdemo_ui_text_width(FONT_SMALL, value);
    devdemo_ui_text(fb, band.x + band.width - value_width, band.y, FONT_SMALL,
                    value, true);

    const devdemo_rect_t track = devdemo_ui_slider_track(band);
    const int fill_width = track.width * percent / 100;
    if (fill_width > 0) {
        devdemo_ui_fill_rect(
            fb,
            (devdemo_rect_t){track.x, track.y + 5, fill_width, 8}, true);
    }
    devdemo_ui_stroke_rect(fb, track, active ? 2 : 1, true);

    for (int tick = 1; tick < 4; ++tick) {
        const int x = track.x + track.width * tick / 4;
        devdemo_ui_line(fb, x, track.y + 1, x, track.y + 5, true);
        devdemo_ui_line(fb, x, track.y + track.height - 6, x,
                        track.y + track.height - 2, true);
    }

    const int knob_x =
        track.x + fill_width - (percent == 100 ? 11 : 9);
    const int knob_y = track.y - 6;
    devdemo_ui_fill_rect(
        fb, (devdemo_rect_t){knob_x, knob_y, 18, 30}, true);
    devdemo_ui_fill_rect(
        fb, (devdemo_rect_t){knob_x + 4, knob_y + 5, 10, 20}, false);
    devdemo_ui_fill_rect(
        fb, (devdemo_rect_t){knob_x + 7, knob_y + 9, 4, 12}, true);
}

devdemo_rect_t devdemo_ui_menu_handle_rect(void)
{
    return (devdemo_rect_t){
        .x = UI_CANVAS_WIDTH_PX - DEVDEMO_UI_MARGIN -
             DEVDEMO_UI_MENU_BUTTON,
        .y = DEVDEMO_UI_BAR_TOP,
        .width = DEVDEMO_UI_MENU_BUTTON,
        .height = DEVDEMO_UI_BAR_HEIGHT,
    };
}

void devdemo_ui_menu_handle(uint8_t *fb, bool menu_open)
{
    const devdemo_rect_t rect = devdemo_ui_menu_handle_rect();
    devdemo_ui_stroke_rect(fb, rect, 1, true);
    const int center_x = rect.x + rect.width / 2;
    const int center_y = rect.y + rect.height / 2;
    if (menu_open) {
        devdemo_ui_line(fb, center_x - 9, center_y - 4, center_x, center_y + 5,
                        true);
        devdemo_ui_line(fb, center_x, center_y + 5, center_x + 9,
                        center_y - 4, true);
    } else {
        devdemo_ui_line(fb, center_x - 9, center_y + 5, center_x,
                        center_y - 4, true);
        devdemo_ui_line(fb, center_x, center_y - 4, center_x + 9,
                        center_y + 5, true);
    }
}

void devdemo_ui_checker(uint8_t *fb, devdemo_rect_t rect, int cell_size)
{
    if (cell_size <= 0) {
        cell_size = 8;
    }
    for (int row = 0; row < rect.height; ++row) {
        for (int column = 0; column < rect.width; ++column) {
            const bool black =
                (((column / cell_size) + (row / cell_size)) & 1) == 0;
            set_pixel(fb, rect.x + column, rect.y + row, black);
        }
    }
    devdemo_ui_stroke_rect(fb, rect, 1, true);
}

bool devdemo_ui_rect_hit(devdemo_rect_t rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
           y < rect.y + rect.height;
}

devdemo_rect_t devdemo_ui_grid_rect(int column, int columns, int row, int y,
                                    int height)
{
    const int available =
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN -
        (columns - 1) * DEVDEMO_UI_GAP;
    const int width = available / columns;
    return (devdemo_rect_t){
        .x = DEVDEMO_UI_MARGIN +
             column * (width + DEVDEMO_UI_GAP),
        .y = y + row * (height + DEVDEMO_UI_GAP),
        .width = width,
        .height = height,
    };
}

devdemo_rect_t devdemo_ui_bar_rect(int column, int columns)
{
    const int available =
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN -
        DEVDEMO_UI_MENU_BUTTON - DEVDEMO_UI_GAP;
    const int width =
        (available - (columns - 1) * DEVDEMO_UI_GAP) / columns;
    return (devdemo_rect_t){
        .x = DEVDEMO_UI_MARGIN +
             column * (width + DEVDEMO_UI_GAP),
        .y = DEVDEMO_UI_BAR_TOP,
        .width = width,
        .height = DEVDEMO_UI_BAR_HEIGHT,
    };
}
