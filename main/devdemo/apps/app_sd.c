#include <stdio.h>
#include <string.h>

#include "a4_sd.h"
#include "devdemo_app.h"
#include "devdemo_ui.h"

typedef struct {
    bool mounted;
    int root_entries;
    a4_sd_entry_t entries[3];
} sd_snapshot_t;

static sd_snapshot_t s_sd;

static void ascii_copy(char *destination, size_t size, const char *source)
{
    size_t written = 0;
    while (source != NULL && *source != '\0' && written + 1U < size) {
        const uint8_t value = (uint8_t)*source++;
        destination[written++] =
            value >= 32U && value <= 126U ? (char)value : '?';
    }
    destination[written] = '\0';
}

static void read_sd(void)
{
    memset(&s_sd, 0, sizeof(s_sd));
    s_sd.mounted = a4_sd_mount();
    if (s_sd.mounted) {
        (void)a4_sd_list("", 0, s_sd.entries, 3, &s_sd.root_entries);
    }
}

static void sd_enter(devdemo_ctx_t *ctx)
{
    (void)ctx;
    read_sd();
}

static void sd_render(devdemo_ctx_t *ctx)
{
    char line[80];
    devdemo_ui_clear(ctx->fb);
    devdemo_ui_header(ctx->fb, "microSD",
                      "SPI2 39/38/40/47, mount and read-only listing");
    int y = devdemo_ui_section(ctx->fb, DEVDEMO_UI_CONTENT_TOP, "CARD STATE");
    y = devdemo_ui_row(ctx->fb, y, "Mount",
                       s_sd.mounted ? "MOUNTED /sd" : "NOT MOUNTED");
    (void)snprintf(line, sizeof(line), "%d", s_sd.root_entries);
    y = devdemo_ui_row(ctx->fb, y, "Root entries", line);
    y = devdemo_ui_row(ctx->fb, y, "SPI host", "SPI2");
    y = devdemo_ui_row(ctx->fb, y, "CS", "GPIO47");

    y = devdemo_ui_section(ctx->fb, y + 18, "ROOT LISTING");
    for (int index = 0; index < 3; ++index) {
        if (s_sd.mounted && index < s_sd.root_entries &&
            s_sd.entries[index].name[0] != '\0') {
            char safe_name[64];
            ascii_copy(safe_name, sizeof(safe_name),
                       s_sd.entries[index].name);
            (void)snprintf(line, sizeof(line), "%s %s",
                           s_sd.entries[index].is_dir ? "[D]" : "[F]",
                           safe_name);
        } else {
            (void)snprintf(line, sizeof(line), "-");
        }
        y = devdemo_ui_row(ctx->fb, y, index == 0 ? "Entry 1" :
                                          index == 1 ? "Entry 2" : "Entry 3",
                           line);
    }
    y = devdemo_ui_row(ctx->fb, y, "Writes", "DISABLED");
    devdemo_ui_text_fit(
        ctx->fb, DEVDEMO_UI_MARGIN, y + 16,
        UI_CANVAS_WIDTH_PX - 2 * DEVDEMO_UI_MARGIN, 28,
        "Remount may briefly block while FatFs probes the card.", true);
    devdemo_ui_button(ctx->fb, devdemo_ui_bar_rect(0, 1), "REMOUNT", false);
}

static devdemo_redraw_t sd_touch(devdemo_ctx_t *ctx)
{
    if (devdemo_ui_rect_hit(devdemo_ui_bar_rect(0, 1), ctx->touch_x,
                            ctx->touch_y)) {
        read_sd();
        return DEVDEMO_REDRAW_PAGE;
    }
    return DEVDEMO_REDRAW_NONE;
}

const devdemo_app_t devdemo_app_sd = {
    .title = "microSD",
    .detail = "Mount, root listing and read-only policy",
    .render = sd_render,
    .on_enter = sd_enter,
    .on_touch = sd_touch,
};
