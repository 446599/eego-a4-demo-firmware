#pragma once

#include <stdint.h>
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FatFs filesystem type for an already mounted SD card, or zero on error. */
uint8_t fatfs_sdcard_filesystem_type(const sdmmc_card_t *card);

/* Stable diagnostic label for the mounted filesystem. */
const char *fatfs_filesystem_name(uint8_t filesystem_type);

#ifdef __cplusplus
}
#endif
