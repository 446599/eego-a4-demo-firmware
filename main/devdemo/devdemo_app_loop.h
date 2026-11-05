#pragma once

#include <stdint.h>

#include "devdemo_app.h"

void devdemo_app_loop_run(const devdemo_app_t *first_app, uint8_t *ui_fb,
                          uint8_t *panel_fb);
