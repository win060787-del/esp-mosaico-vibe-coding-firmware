#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
typedef struct app_notify *app_notify_handle_t;
/* Boot lifetime, one worker owns the physical actuators; observation is thread safe. */
esp_err_t app_notify_create(app_notify_handle_t *out);
void app_notify_observe(app_notify_handle_t handle,const cJSON *snapshot);

void app_notify_set_options(app_notify_handle_t handle,unsigned volume,bool vibration,bool sound);
esp_err_t app_notify_preview(app_notify_handle_t handle);

bool app_notify_audio_acquire(app_notify_handle_t handle);
void app_notify_audio_release(app_notify_handle_t handle);
