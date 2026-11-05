#pragma once

#include "devdemo_app.h"

int devdemo_app_count(void);
const devdemo_app_t *devdemo_app_at(int index);
int devdemo_app_index_of(const devdemo_app_t *app);
const devdemo_app_t *devdemo_overview_page(void);
