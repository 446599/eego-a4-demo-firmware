#include "fatfs_status.h"

#include "ff.h"
#include "vfs_fat_internal.h"

#if FF_FS_EXFAT != 1
#error "local fatfs must be built with FF_FS_EXFAT enabled"
#endif

uint8_t fatfs_sdcard_filesystem_type(const sdmmc_card_t *card)
{
    const vfs_fat_sd_ctx_t *const context = get_vfs_fat_get_sd_ctx(card);
    return context != NULL && context->fs != NULL ? context->fs->fs_type : 0;
}

const char *fatfs_filesystem_name(uint8_t filesystem_type)
{
    switch (filesystem_type) {
    case FS_FAT12:
        return "FAT12";
    case FS_FAT16:
        return "FAT16";
    case FS_FAT32:
        return "FAT32";
    case FS_EXFAT:
        return "exFAT";
    default:
        return "unknown";
    }
}
