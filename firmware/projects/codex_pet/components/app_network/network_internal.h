#pragma once
#include "app_network.h"
#include "wifi_profiles.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

typedef struct {unsigned kind,value;struct pet_wifi_profile profile;} net_command_t;
#define NET_ADD_PROFILE 100U
#define NET_PROFILE_CONNECT 101U
#define NET_TRY_PROFILE 102U

typedef struct {
    char url[192],read[80],manage[80],legacy_ssid[33],legacy_password[65];
    char authorization[96],target[384],response[2049],viewer[33],fixed[33];
    char current_ssid[33],current_password[65];
    wifi_config_t wifi;
    pet_wifi_profiles_t profiles;
    app_pet_management_t view;
} network_data_t;
struct app_network {
    app_link_handle_t link;network_data_t *data;QueueHandle_t commands;SemaphoreHandle_t view_lock;
    esp_http_client_handle_t client;httpd_handle_t portal;esp_netif_t *ap;
    atomic_uint connections,pulls,last_ts,revision,disconnect_reason;
    atomic_bool connected,configured;
    unsigned menu,mode,terminal_page,wifi_page,tried,visible;
    bool joining;
    int join_previous;
    bool initialized,started,had_connected,attempting,profiles_synced;
    TickType_t attempt_tick,retry_tick,portal_until,roster_tick;
};
typedef struct app_network network_t;
void network_status_message(network_t *net,const char *message);
esp_err_t network_portal_start(network_t *net);
void network_portal_stop(network_t *net);
