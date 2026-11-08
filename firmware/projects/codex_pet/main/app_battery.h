#pragma once
#include "esp_err.h"
typedef struct app_battery *app_battery_handle_t;
/* Owned for the application's lifetime. Never changes the gauge calibration. */
esp_err_t app_battery_create(app_battery_handle_t *out);
int app_battery_poll(void *context);
