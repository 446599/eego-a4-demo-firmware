#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* ESP-IDF's dirent.d_name holds up to 255 UTF-8 bytes plus NUL.  Keep the
     * complete on-card name here; the UI owns its smaller display copy. */
    A4_SD_NAME_MAX = 256,
    /* Includes the terminating NUL. `/sd/` leaves 251 relative-path bytes. */
    A4_SD_PATH_MAX = 512,
    /* Relative path capacity after the `/sd/` mount prefix. */
    A4_SD_RELATIVE_PATH_MAX = A4_SD_PATH_MAX - 5,
};
typedef struct {
    char name[A4_SD_NAME_MAX];
    bool is_dir;
    uint32_t size_bytes;
} a4_sd_entry_t;

/* Mounts the card on the dedicated HSPI bus (SCK39/MOSI38/MISO40/CS47).
 * Safe to call repeatedly; remounts if the card was swapped. */
bool a4_sd_mount(void);
bool a4_sd_is_mounted(void);
void a4_sd_unmount(void);

/* Lists one page of directory entries sorted directories-first.  `path` is
 * relative to the card root ("" or "books/x").  Returns the entry count
 * written to `out` and stores the directory's total in *total_entries. */
int a4_sd_list(const char *path, int offset, a4_sd_entry_t *out, int max_out,
               int *total_entries);

#ifdef __cplusplus
}
#endif
