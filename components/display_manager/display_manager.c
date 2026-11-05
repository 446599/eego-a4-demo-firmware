#include "display_manager.h"

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_display_mux = portMUX_INITIALIZER_UNLOCKED;
static display_manager_snapshot_t s_snapshot;
static bool s_initialized;

esp_err_t display_manager_init(void)
{
    portENTER_CRITICAL(&s_display_mux);
    s_snapshot = (display_manager_snapshot_t){0};
    s_initialized = true;
    portEXIT_CRITICAL(&s_display_mux);
    return ESP_OK;
}

bool display_manager_begin(void)
{
    portENTER_CRITICAL(&s_display_mux);
    const bool accepted = s_initialized && !s_snapshot.active;
    if (accepted) {
        s_snapshot.active = true;
        s_snapshot.sequence++;
    }
    portEXIT_CRITICAL(&s_display_mux);
    return accepted;
}

void display_manager_finish(esp_err_t result)
{
    portENTER_CRITICAL(&s_display_mux);
    if (s_snapshot.active) {
        s_snapshot.active = false;
        s_snapshot.last_error = result;
        if (result == ESP_OK) {
            s_snapshot.completed++;
        } else {
            s_snapshot.failed++;
        }
    }
    portEXIT_CRITICAL(&s_display_mux);
}

esp_err_t display_manager_run(display_manager_operation_fn operation,
                              void *context)
{
    if (operation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_manager_begin()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = operation(context);
    display_manager_finish(result);
    return result;
}

bool display_manager_is_active(void)
{
    portENTER_CRITICAL(&s_display_mux);
    const bool active = s_snapshot.active;
    portEXIT_CRITICAL(&s_display_mux);
    return active;
}

void display_manager_snapshot(display_manager_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_display_mux);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_display_mux);
}
