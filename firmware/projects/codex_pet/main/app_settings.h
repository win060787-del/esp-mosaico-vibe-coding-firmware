#pragma once
#include "app_pet.h"
#include "app_notify.h"
#include "esp_err.h"
typedef struct app_settings *app_settings_handle_t;
esp_err_t app_settings_create(app_notify_handle_t notify,app_settings_handle_t *out);
bool app_settings_poll(void *ctx,app_pet_preferences_t *out);
bool app_settings_action(void *ctx,app_pet_action_t action,unsigned value);
