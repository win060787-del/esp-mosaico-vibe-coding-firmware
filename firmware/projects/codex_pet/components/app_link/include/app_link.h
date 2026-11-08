// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_pet.h"
#include "esp_err.h"
#include "app_notify.h"
typedef struct app_link *app_link_handle_t;
/* Owns one-slot mailbox. RPC runs on Iris; polling runs only on GSP's UI thread.
 * App lifetime is boot lifetime; start once after app_pet is ready to consume. */
esp_err_t app_link_create(app_notify_handle_t notify, app_link_handle_t *out);
bool app_link_poll(void *context, app_pet_snapshot_t *out);

/* Validate an external JSON snapshot and copy it into the UI mailbox. */
esp_err_t app_link_submit(app_link_handle_t link, const char *data, size_t size);
