#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t reg;
    uint32_t value;
} gsl_fw_pair_t;

extern const gsl_fw_pair_t gsl1680_fw[];
extern const size_t gsl1680_fw_count;
