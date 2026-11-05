#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    uint32_t sequence;
    uint32_t completed;
    uint32_t failed;
    esp_err_t last_error;
} display_manager_snapshot_t;

typedef esp_err_t (*display_manager_operation_fn)(void *context);

esp_err_t display_manager_init(void);
bool display_manager_begin(void);
void display_manager_finish(esp_err_t result);
esp_err_t display_manager_run(display_manager_operation_fn operation,
                              void *context);
bool display_manager_is_active(void);
void display_manager_snapshot(display_manager_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
