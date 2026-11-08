// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"

typedef enum { PET_OFFLINE, PET_IDLE, PET_WORKING, PET_WAITING, PET_COMPLETED, PET_FAILED } app_pet_state_t;
#define PET_TASKS 4
#define PET_TITLE_BYTES 97
/* All strings are owned UTF-8 copies. Epoch values are seconds, not milliseconds. */
typedef struct {
    app_pet_state_t state;
    uint32_t generated_at;
    uint32_t server_time;
    uint32_t reset_at;
    uint32_t done_at;
    unsigned active;
    unsigned done_count;
    char source_name[49];
    int remaining; /* -1 means unavailable; otherwise 0..100. */
    bool partial;
    bool source_ok;
    char quota_label[40];
    char tasks[PET_TASKS][PET_TITLE_BYTES];
    app_pet_state_t task_states[PET_TASKS];
    bool task_unread[PET_TASKS];
    char done_title[PET_TITLE_BYTES];
    char recent_title[PET_TITLE_BYTES];
} app_pet_snapshot_t;
typedef struct app_pet *app_pet_handle_t;
/* Source is polled on the UI loop and must be nonblocking. It writes only on true.
 * UI owns its copied snapshot; source_ctx must outlive the controller. */
typedef bool (*app_pet_source_t)(void *ctx, app_pet_snapshot_t *out);
esp_gsp_err_t app_pet_create(esp_gsp_handle_t ui, app_pet_source_t source,
                            void *source_ctx, app_pet_handle_t *out);
esp_gsp_err_t app_pet_destroy(app_pet_handle_t *handle);

/* Nonblocking local telemetry; returns -1 when unavailable. Register on UI startup. */
typedef int (*app_pet_battery_source_t)(void *ctx);
void app_pet_set_battery_source(app_pet_handle_t handle, app_pet_battery_source_t source, void *ctx);

typedef struct { unsigned volume,brightness,color;bool vibration,sound,wireless_ready,save_error;char version[16]; } app_pet_preferences_t;
typedef enum { PET_SET_VOLUME,PET_SET_BRIGHTNESS,PET_TOGGLE_VIBRATION,PET_PREVIEW,PET_SET_SOUND,PET_SET_COLOR } app_pet_action_t;
typedef bool (*app_pet_preferences_source_t)(void *ctx,app_pet_preferences_t *out);
typedef bool (*app_pet_action_handler_t)(void *ctx,app_pet_action_t action,unsigned value);
void app_pet_set_controls(app_pet_handle_t handle,app_pet_preferences_source_t source,
                          app_pet_action_handler_t handler,void *ctx);

#define PET_MANAGEMENT_ROWS 4
typedef struct {
    unsigned generation,mode,terminal_page,terminal_total,wifi_page,wifi_total;
    char terminal_ids[4][33],terminal_names[4][49];bool terminal_online[4];unsigned terminal_active[4];
    char selected_terminal[33],wifi_ssids[4][33],connected_ssid[33];
    bool portal_active;char portal_ssid[33],portal_password[17];
    char enrollment_code[16];unsigned enrollment_expires;
    char status[97];
    unsigned scan_count, join_result, join_revision;
    char scan_ssids[24][33];
} app_pet_management_t;
typedef enum {
 PET_MANAGE_SCREEN,PET_MANAGE_MODE,PET_MANAGE_TERMINAL,PET_MANAGE_TERMINAL_PAGE,
 PET_MANAGE_ENROLL,PET_MANAGE_WIFI_PAGE,PET_MANAGE_WIFI_CONNECT,PET_MANAGE_WIFI_PROMOTE,
 PET_MANAGE_WIFI_DELETE,PET_MANAGE_WIFI_ADD,PET_MANAGE_PORTAL_STOP,PET_MANAGE_SCAN
} app_pet_management_action_t;
typedef bool (*app_pet_management_source_t)(void *ctx,app_pet_management_t *out);
typedef bool (*app_pet_management_handler_t)(void *ctx,app_pet_management_action_t action,unsigned value);
void app_pet_set_management(app_pet_handle_t handle,app_pet_management_source_t source,app_pet_management_handler_t action,void *ctx);

typedef bool (*app_pet_join_handler_t)(void *ctx,const char *ssid,const char *password);
void app_pet_set_join(app_pet_handle_t handle,app_pet_join_handler_t handler);

/* Voice snapshots are owned copies; callbacks run on the UI task and never block. */
typedef struct { unsigned revision,level;bool recording,busy,connected;char state[20],name[49],text[1801],error[193]; } app_pet_voice_t;
typedef bool (*app_pet_voice_source_t)(void*,app_pet_voice_t*);
typedef bool (*app_pet_voice_action_t)(void*,const char*);
void app_pet_set_voice(app_pet_handle_t handle,app_pet_voice_source_t source,app_pet_voice_action_t action,void *ctx);
