#pragma once
#include "app_link.h"
typedef struct app_network *app_network_handle_t;
/* App lifetime is boot lifetime. All network work runs outside the UI task. */
esp_err_t app_network_start(app_link_handle_t link,app_network_handle_t *out);
bool app_network_management_poll(void *ctx,app_pet_management_t *out);
bool app_network_management_action(void *ctx,app_pet_management_action_t action,unsigned value);

bool app_network_join(void *ctx,const char *ssid,const char *password);
