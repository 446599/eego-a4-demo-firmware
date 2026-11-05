#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void epd_frame_history_reset(uint8_t *history, size_t size)
{
    memset(history, 0xff, size);
}

static inline bool epd_frame_history_commit(uint8_t *history,
                                            const uint8_t *frame,
                                            size_t size)
{
    if (history == NULL || frame == NULL) {
        return false;
    }
    memcpy(history, frame, size);
    return true;
}
