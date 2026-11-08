#pragma once
#include "app_pet.h"
#include "app_notify.h"
#include "esp_err.h"
typedef struct app_voice *app_voice_handle_t;
esp_err_t app_voice_create(app_notify_handle_t notify,app_voice_handle_t *out);
bool app_voice_poll(void *ctx,app_pet_voice_t *out);
bool app_voice_action(void *ctx,const char *action);
