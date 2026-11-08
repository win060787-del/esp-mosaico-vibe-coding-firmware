// SPDX-License-Identifier: Apache-2.0
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/task.h"
#include "mdns.h"
#include "network_internal.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const char server_cert[] asm("_binary_server_crt_start");
#define API "https://43.138.181.110/codex-pet/v1"
#define FETCH_INTERVAL_MS 500U
static const char *TAG = "pet_network";
static const char *const modes[] = {"auto", "fixed", "all"};
static bool get_string(const cJSON *root, const char *key, char *out,
                       size_t cap) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(value) || strlen(value->valuestring) >= cap)
    return false;
  strcpy(out, value->valuestring);
  return true;
}
void network_status_message(network_t *net, const char *message) {
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  snprintf(net->data->view.status, sizeof(net->data->view.status), "%s",
           message);
  ++net->data->view.generation;
  xSemaphoreGive(net->view_lock);
}
static void refresh_view(network_t *net) {
  network_data_t *d = net->data;
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  d->view.mode = net->mode;
  d->view.wifi_page = net->wifi_page;
  d->view.wifi_total = d->profiles.count;
  d->view.terminal_page = net->terminal_page;
  snprintf(d->view.connected_ssid, sizeof(d->view.connected_ssid), "%s",
           atomic_load(&net->connected) ? d->current_ssid : "");
  for (unsigned row = 0; row < 4; ++row) {
    unsigned index = net->wifi_page * 4 + row;
    snprintf(d->view.wifi_ssids[row], sizeof(d->view.wifi_ssids[row]), "%s",
             index < d->profiles.count ? d->profiles.profiles[index].ssid : "");
  }
  ++d->view.generation;
  xSemaphoreGive(net->view_lock);
}
static esp_err_t save_profiles(const pet_wifi_profiles_t *profiles) {
  if (!pet_wifi_valid(profiles))
    return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t err = nvs_open("pet_wifi", NVS_READWRITE, &h);
  if (err != ESP_OK)
    return err;
  err = nvs_set_blob(h, "profiles", profiles, sizeof(*profiles));
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err;
}
static void load_profiles(network_t *net) {
  network_data_t *d = net->data;
  nvs_handle_t h;
  size_t bytes = sizeof(d->profiles);
  bool loaded = false;
  if (nvs_open("pet_wifi", NVS_READONLY, &h) == ESP_OK) {
    loaded = nvs_get_blob(h, "profiles", &d->profiles, &bytes) == ESP_OK &&
             bytes == sizeof(d->profiles) && pet_wifi_valid(&d->profiles);
    nvs_close(h);
  }
  if (!loaded) {
    memset(&d->profiles, 0, sizeof(d->profiles));
    d->profiles.version = 1;
    if (d->legacy_ssid[0] &&
        pet_wifi_put(&d->profiles, d->legacy_ssid, d->legacy_password))
      (void)save_profiles(&d->profiles);
  }
  net->wifi_page = 0;
  refresh_view(net);
}
static bool load_cloud(network_t *net) {
  network_data_t *d = net->data;
  nvs_handle_t h;
  size_t n;
  uint8_t recovery_wifi = 0;
  if (nvs_open("pet_network", NVS_READONLY, &h) != ESP_OK)
    return false;
  n = sizeof(d->url);
  esp_err_t err = nvs_get_str(h, "url", d->url, &n);
  n = sizeof(d->read);
  if (err == ESP_OK)
    err = nvs_get_str(h, "token", d->read, &n);
  n = sizeof(d->manage);
  (void)nvs_get_str(h, "manage", d->manage, &n);
  (void)nvs_get_u8(h, "recovery_wifi", &recovery_wifi);
  if (!recovery_wifi) {
    n = sizeof(d->legacy_ssid);
    (void)nvs_get_str(h, "ssid", d->legacy_ssid, &n);
    n = sizeof(d->legacy_password);
    (void)nvs_get_str(h, "password", d->legacy_password, &n);
  }
  nvs_close(h);
  if (!d->legacy_ssid[0] &&
      nvs_open_from_partition("sysmeta", "wifi", NVS_READONLY, &h) == ESP_OK) {
    n = sizeof(d->legacy_ssid);
    (void)nvs_get_str(h, "ssid", d->legacy_ssid, &n);
    n = sizeof(d->legacy_password);
    (void)nvs_get_str(h, "password", d->legacy_password, &n);
    nvs_close(h);
  }
  return err == ESP_OK && strcmp(d->url, API "/state") == 0 && d->read[0];
}
static void load_selection(network_t *net) {
  nvs_handle_t h;
  uint8_t mode = 0;
  size_t bytes = sizeof(net->data->fixed);
  if (nvs_open("pet_view", NVS_READONLY, &h) == ESP_OK) {
    (void)nvs_get_u8(h, "mode", &mode);
    (void)nvs_get_str(h, "terminal", net->data->fixed, &bytes);
    nvs_close(h);
  }
  net->mode = mode < 3 ? mode : 0;
}
static void save_selection(network_t *net) {
  nvs_handle_t h;
  esp_err_t err = nvs_open("pet_view", NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_set_u8(h, "mode", net->mode);
    if (err == ESP_OK)
      err = nvs_set_str(h, "terminal", net->data->fixed);
    if (err == ESP_OK)
      err = nvs_commit(h);
    nvs_close(h);
  }
  if (err != ESP_OK)
    network_status_message(net, "选择未保存，请重试");
}
static void close_client(network_t *net) {
  if (net->client) {
    esp_http_client_close(net->client);
    esp_http_client_cleanup(net->client);
    net->client = NULL;
  }
}
static esp_err_t event_http(esp_http_client_event_t *event) {
  network_t *net = event->user_data;
  if (event->event_id == HTTP_EVENT_ON_CONNECTED)
    atomic_fetch_add(&net->connections, 1);
  return ESP_OK;
}
static esp_err_t request(network_t *net, const char *path, const char *body,
                         bool manage) {
  network_data_t *d = net->data;
  snprintf(d->target, sizeof(d->target), "%s%s", API, path);
  if (!net->client) {
    const esp_http_client_config_t config = {.url = d->target,
                                             .cert_pem = server_cert,
                                             .timeout_ms = 10000,
                                             .buffer_size = 1024,
                                             .buffer_size_tx = 512,
                                             .disable_auto_redirect = true,
                                             .keep_alive_enable = true,
                                             .event_handler = event_http,
                                             .user_data = net};
    net->client = esp_http_client_init(&config);
  }
  if (!net->client)
    return ESP_ERR_NO_MEM;
  snprintf(d->authorization, sizeof(d->authorization), "Bearer %s",
           manage ? d->manage : d->read);
  esp_err_t err = esp_http_client_set_url(net->client, d->target);
  if (err == ESP_OK)
    err = esp_http_client_set_method(net->client,
                                     body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
  if (err == ESP_OK)
    err = esp_http_client_set_header(net->client, "Authorization",
                                     d->authorization);
  if (err == ESP_OK)
    err = esp_http_client_set_header(net->client, "Content-Type",
                                     "application/json");
  size_t bytes = body ? strlen(body) : 0;
  if (err == ESP_OK)
    err = esp_http_client_open(net->client, bytes);
  if (err == ESP_OK && bytes &&
      esp_http_client_write(net->client, body, bytes) != (int)bytes)
    err = ESP_FAIL;
  if (err == ESP_OK) {
    if (esp_http_client_fetch_headers(net->client) < 0 ||
        esp_http_client_get_status_code(net->client) != 200)
      err = ESP_FAIL;
    else {
      int used = 0, n = 0;
      while (used < 2048 &&
             (n = esp_http_client_read(net->client, d->response + used,
                                       2048 - used)) > 0)
        used += n;
      if (n < 0 || used == 2048 ||
          !esp_http_client_is_complete_data_received(net->client))
        err = ESP_ERR_INVALID_SIZE;
      else
        d->response[used] = 0;
    }
  }
  if (err != ESP_OK)
    close_client(net);
  return err;
}
static esp_err_t fetch_state(network_t *net) {
  char path[128];
  snprintf(path, sizeof(path), "/state?mode=%s&terminal=%.32s&viewer=%.32s",
           modes[net->mode], net->data->fixed, net->data->viewer);
  esp_err_t err = request(net, path, NULL, false);
  if (err == ESP_OK) {
    cJSON *json = cJSON_Parse(net->data->response);
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(json, "ts");
    if (!cJSON_IsNumber(ts) || ts->valuedouble < time(NULL) - 90 ||
        ts->valuedouble > time(NULL) + 30)
      err = ESP_ERR_INVALID_ARG;
    else {
      err = app_link_submit(net->link, net->data->response,
                            strlen(net->data->response));
      if (err == ESP_OK) {
        atomic_fetch_add(&net->pulls, 1);
        atomic_store(&net->last_ts, (unsigned)ts->valuedouble);
      }
    }
    cJSON_Delete(json);
  }
  return err;
}
static void fetch_catalog(network_t *net) {
  char path[144];
  snprintf(path, sizeof(path),
           "/terminals?page=%u&mode=%s&terminal=%.32s&viewer=%.32s",
           net->terminal_page, modes[net->mode], net->data->fixed,
           net->data->viewer);
  if (request(net, path, NULL, false) != ESP_OK) {
    network_status_message(net, "终端列表暂不可用");
    return;
  }
  cJSON *json = cJSON_Parse(net->data->response);
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(json, "terminals"),
              *total = cJSON_GetObjectItemCaseSensitive(json, "total");
  if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > 4 ||
      !cJSON_IsNumber(total) || total->valueint < 0 || total->valueint > 64) {
    cJSON_Delete(json);
    network_status_message(net, "终端列表格式错误");
    return;
  }
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  app_pet_management_t *view = &net->data->view;
  view->terminal_total = total->valueint;
  view->terminal_page = net->terminal_page;
  (void)get_string(json, "selected", view->selected_terminal,
                   sizeof(view->selected_terminal));
  for (unsigned i = 0; i < 4; ++i) {
    const cJSON *row = cJSON_GetArrayItem(rows, i);
    view->terminal_ids[i][0] = view->terminal_names[i][0] = 0;
    view->terminal_online[i] = false;
    view->terminal_active[i] = 0;
    if (row &&
        get_string(row, "id", view->terminal_ids[i],
                   sizeof(view->terminal_ids[i])) &&
        get_string(row, "name", view->terminal_names[i],
                   sizeof(view->terminal_names[i]))) {
      view->terminal_online[i] =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "online"));
      const cJSON *active = cJSON_GetObjectItemCaseSensitive(row, "active");
      if (cJSON_IsNumber(active) && active->valueint >= 0 &&
          active->valueint <= 9999)
        view->terminal_active[i] = active->valueint;
    }
  }
  ++view->generation;
  xSemaphoreGive(net->view_lock);
  cJSON_Delete(json);
}
static void enrollment(network_t *net) {
  if (!net->data->manage[0]) {
    network_status_message(net, "请先在 Mac 完成初始配置");
    return;
  }
  if (request(net, "/enroll-code", "{}", true) != ESP_OK) {
    network_status_message(net, "配对码生成失败，请重试");
    return;
  }
  cJSON *json = cJSON_Parse(net->data->response);
  const cJSON *expires = cJSON_GetObjectItemCaseSensitive(json, "expires");
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  bool ok = get_string(json, "code", net->data->view.enrollment_code,
                       sizeof(net->data->view.enrollment_code)) &&
            cJSON_IsNumber(expires);
  net->data->view.enrollment_expires = ok ? expires->valueint : 0;
  ++net->data->view.generation;
  xSemaphoreGive(net->view_lock);
  cJSON_Delete(json);
  network_status_message(net, ok ? "在另一台 Mac 输入配对码"
                                 : "配对码生成失败，请重试");
}
static void wifi_event(void *ctx, esp_event_base_t base, int32_t id,
                       void *data) {
  network_t *net = ctx;
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    atomic_store(&net->connected, true);
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    atomic_store(&net->connected, false);
    atomic_store(&net->disconnect_reason,
                 ((wifi_event_sta_disconnected_t *)data)->reason);
  }
}
static esp_err_t initialize_wifi(network_t *net) {
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK)
    return err;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;
  if (!esp_netif_create_default_wifi_sta())
    return ESP_ERR_NO_MEM;
  const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&config);
  if (err != ESP_OK)
    return err;
  if (mdns_init() == ESP_OK) {
    (void)mdns_hostname_set("codex-pet-d0a33a");
    const esp_iris_mdns_config_t discovery = {.instance_prefix = "Codex Pet",
                                              .mode = "normal"};
    (void)esp_iris_mdns_register(&discovery);
  }
  err =
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, net);
  if (err != ESP_OK)
    return err;
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event,
                                   net);
  if (err != ESP_OK)
    return err;
  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err == ESP_OK)
    err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK)
    err = esp_wifi_start();
  if (err != ESP_OK)
    return err;
  const esp_sntp_config_t time_config =
      ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
  return esp_netif_sntp_init(&time_config);
}
static void scan_visible(network_t *net) {
  net->visible = 0;
  network_status_message(net, "正在查找已保存的网络");
  if (esp_wifi_scan_start(NULL, true) != ESP_OK)
    return;
  uint16_t count = 24;
  wifi_ap_record_t *records = calloc(count, sizeof(*records));
  if (!records)
    return;
  if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK)
    for (unsigned i = 0; i < net->data->profiles.count; ++i)
      for (unsigned j = 0; j < count; ++j)
        if (strcmp(net->data->profiles.profiles[i].ssid,
                   (const char *)records[j].ssid) == 0)
          net->visible |= 1U << i;
  free(records);
}
static void connect_profile(network_t *net,const struct pet_wifi_profile *profile) {
  network_data_t *d=net->data;
  close_client(net);
  (void)esp_wifi_disconnect();
  atomic_store(&net->connected,false);
  strcpy(d->current_ssid,profile->ssid);
  strcpy(d->current_password,profile->password);
  memset(&d->wifi, 0, sizeof(d->wifi));
  memcpy(d->wifi.sta.ssid, d->current_ssid, strlen(d->current_ssid));
  memcpy(d->wifi.sta.password, d->current_password,
         strlen(d->current_password));
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &d->wifi);
  if (err == ESP_OK)
    err = esp_wifi_connect();
  net->attempting = true;
  net->attempt_tick = xTaskGetTickCount();

  net->profiles_synced = false;
  network_status_message(net, err == ESP_OK ? "正在连接，失败会尝试下一个网络"
                                            : "正在尝试下一个网络");
  refresh_view(net);
}
static void begin_attempt(network_t *net,unsigned index) {
  if(index>=net->data->profiles.count)return;
  connect_profile(net,&net->data->profiles.profiles[index]);
  net->tried |= 1U << index;
}
static void scan_networks(network_t *net) {
  uint16_t count=24;
  wifi_ap_record_t *records=calloc(count,sizeof(*records));
  bool scanned=records && esp_wifi_scan_start(NULL,true)==ESP_OK &&
      esp_wifi_scan_get_ap_records(&count,records)==ESP_OK;
  xSemaphoreTake(net->view_lock,portMAX_DELAY);
  app_pet_management_t *view=&net->data->view;
  view->scan_count=0;
  if(scanned) for(unsigned i=0;i<count;++i){
    const char *ssid=(const char *)records[i].ssid;
    if(!ssid[0])continue;
    bool duplicate=false;
    for(unsigned j=0;j<view->scan_count;++j)
      if(!strcmp(view->scan_ssids[j],ssid))duplicate=true;
    if(!duplicate)snprintf(view->scan_ssids[view->scan_count++],33,"%.32s",ssid);
  }
  ++view->generation;
  xSemaphoreGive(net->view_lock);
  free(records);
  network_status_message(net,scanned?"选择网络":"扫描失败，请重试");
}
static void join_result(network_t *net,unsigned result) {
  xSemaphoreTake(net->view_lock,portMAX_DELAY);
  net->data->view.join_result=result;
  if(result>=2)++net->data->view.join_revision;
  ++net->data->view.generation;
  xSemaphoreGive(net->view_lock);
}
static void sync_recovery_wifi(network_t *net) {
  nvs_handle_t h;
  if (nvs_open_from_partition("sysmeta", "wifi", NVS_READWRITE, &h) != ESP_OK)
    return;
  char ssid[33] = {0}, password[65] = {0};
  size_t bytes = sizeof(ssid);
  (void)nvs_get_str(h, "ssid", ssid, &bytes);
  bytes = sizeof(password);
  (void)nvs_get_str(h, "password", password, &bytes);
  if (strcmp(ssid, net->data->current_ssid) != 0 ||
      strcmp(password, net->data->current_password) != 0) {
    if (nvs_set_str(h, "ssid", net->data->current_ssid) == ESP_OK &&
        nvs_set_str(h, "password", net->data->current_password) == ESP_OK)
      (void)nvs_commit(h);
  }
  memset(password, 0, sizeof(password));
  nvs_close(h);
  net->profiles_synced = true;
}
static bool change_profiles(network_t *net, const net_command_t *command) {
  pet_wifi_profiles_t *copy = malloc(sizeof(*copy));
  if (!copy)
    return false;
  *copy = net->data->profiles;
  bool ok = false;
  if (command->kind == NET_ADD_PROFILE)
    ok = pet_wifi_put(copy, command->profile.ssid, command->profile.password);
  else if (command->kind == PET_MANAGE_WIFI_DELETE)
    ok = pet_wifi_remove(copy, command->value);
  else if (command->kind == PET_MANAGE_WIFI_PROMOTE)
    ok = pet_wifi_promote(copy, command->value);
  if (ok)
    ok = save_profiles(copy) == ESP_OK;
  if (ok) {
    net->data->profiles = *copy;
    net->wifi_page = net->wifi_page * 4 < copy->count ? net->wifi_page : 0;
    net->tried = 0;
    net->visible = 0;
    refresh_view(net);
  }
  memset(copy, 0, sizeof(*copy));
  free(copy);
  network_status_message(net, ok ? "已保存" : "保存失败，请检查网络数量和密码");
  return ok;
}
static void process_command(network_t *net, net_command_t *command) {
  network_data_t *d = net->data;
  switch (command->kind) {
  case PET_MANAGE_SCAN:
    if(!net->joining)scan_networks(net);
    break;
  case NET_TRY_PROFILE: {
    pet_wifi_profiles_t *candidate=malloc(sizeof(*candidate));
    bool valid=false;
    if(candidate){
      *candidate=d->profiles;
      valid=pet_wifi_put(candidate,command->profile.ssid,command->profile.password);
      memset(candidate,0,sizeof(*candidate));
      free(candidate);
    }
    if(!valid || net->joining){join_result(net,3);break;}
    net->join_previous=atomic_load(&net->connected)?pet_wifi_find(&d->profiles,d->current_ssid):-1;
    net->joining=true;
    net->had_connected=false;
    join_result(net,1);
    connect_profile(net,&command->profile);
    break;
  }
  case PET_MANAGE_SCREEN:
    net->menu = command->value;
    net->roster_tick = 0;
    break;
  case PET_MANAGE_MODE:
    if (command->value > 2)
      break;
    if (command->value == 1 && !d->fixed[0]) {
      xSemaphoreTake(net->view_lock, portMAX_DELAY);
      snprintf(d->fixed, sizeof(d->fixed), "%s", d->view.selected_terminal);
      xSemaphoreGive(net->view_lock);
      if (!d->fixed[0]) {
        network_status_message(net, "请先选择终端");
        break;
      }
    }
    net->mode = command->value;
    save_selection(net);
    net->roster_tick = 0;
    refresh_view(net);
    break;
  case PET_MANAGE_TERMINAL:
    if (command->value >= 4)
      break;
    xSemaphoreTake(net->view_lock, portMAX_DELAY);
    snprintf(d->fixed, sizeof(d->fixed), "%s",
             d->view.terminal_ids[command->value]);
    xSemaphoreGive(net->view_lock);
    if (d->fixed[0]) {
      net->mode = 1;
      save_selection(net);
      net->roster_tick = 0;
      refresh_view(net);
    }
    break;
  case PET_MANAGE_TERMINAL_PAGE:
    if (command->value < 16) {
      net->terminal_page = command->value;
      net->roster_tick = 0;
      refresh_view(net);
    }
    break;
  case PET_MANAGE_ENROLL:
    enrollment(net);
    break;
  case PET_MANAGE_WIFI_PAGE:
    net->wifi_page = command->value < 2 ? command->value : 0;
    refresh_view(net);
    break;
  case PET_MANAGE_WIFI_CONNECT:
  case NET_PROFILE_CONNECT:
    vTaskDelay(pdMS_TO_TICKS(200));
    net->tried = 0;
    net->had_connected = false;
    begin_attempt(net, command->value);
    break;
  case PET_MANAGE_WIFI_PROMOTE:
    (void)change_profiles(net, command);
    break;
  case PET_MANAGE_WIFI_DELETE:
    if (change_profiles(net, command) &&
        pet_wifi_find(&d->profiles, d->current_ssid) < 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      close_client(net);
      (void)esp_wifi_disconnect();
      atomic_store(&net->connected, false);
      net->attempting = false;
      net->had_connected = false;
    }
    break;
  case NET_ADD_PROFILE:
    if (change_profiles(net, command) && command->value) {
      int index = pet_wifi_find(&d->profiles, command->profile.ssid);
      vTaskDelay(pdMS_TO_TICKS(200));
      net->tried = 0;
      net->had_connected = false;
      if (index >= 0)
        begin_attempt(net, index);
    }
    break;
  case PET_MANAGE_WIFI_ADD:
    if (network_portal_start(net) != ESP_OK)
      network_status_message(net, "热点启动失败，请重试");
    else
      network_status_message(net, "用手机连接热点并打开配网页面");
    break;
  case PET_MANAGE_PORTAL_STOP:
    network_portal_stop(net);
    network_status_message(net, "配网热点已关闭");
    break;
  default:
    break;
  }
}
static void maintain_wifi(network_t *net) {
  TickType_t now = xTaskGetTickCount();
  if(net->joining){
    wifi_ap_record_t ap;
    bool joined=atomic_load(&net->connected) && esp_wifi_sta_get_ap_info(&ap)==ESP_OK &&
        !strcmp((const char *)ap.ssid,net->data->current_ssid);
    if(!joined && now-net->attempt_tick<pdMS_TO_TICKS(12000))return;
    net->joining=false;
    if(joined){
      net_command_t command={.kind=NET_ADD_PROFILE};
      strcpy(command.profile.ssid,net->data->current_ssid);
      strcpy(command.profile.password,net->data->current_password);
      bool saved=change_profiles(net,&command);
      memset(&command,0,sizeof(command));
      if(saved){join_result(net,2);return;}
    }
    (void)esp_wifi_disconnect();
    atomic_store(&net->connected,false);
    net->attempting=false;
    net->tried=0;
    join_result(net,3);
    if(net->join_previous>=0){
      begin_attempt(net,(unsigned)net->join_previous);
      return;
    }
  }
  if (atomic_load(&net->connected)) {
    if (!net->had_connected) {
      net->had_connected = true;
      net->attempting = false;
      net->tried = 0;
      network_status_message(net, "已连接");
      refresh_view(net);
    }
    if (!net->profiles_synced)
      sync_recovery_wifi(net);
    return;
  }
  if (net->had_connected) {
    net->had_connected = false;
    close_client(net);
    net->tried = 0;
    int index = pet_wifi_find(&net->data->profiles, net->data->current_ssid);
    if (index >= 0) {
      begin_attempt(net, index);
      return;
    }
    net->attempting = false;
  }
  if (!net->data->profiles.count) {
    if (!net->portal)
      (void)network_portal_start(net);
    network_status_message(net, "请添加 Wi-Fi");
    return;
  }
  if (net->attempting && now - net->attempt_tick < pdMS_TO_TICKS(12000))
    return;
  if ((int32_t)(now - net->retry_tick) < 0)
    return;
  if (!net->tried)
    scan_visible(net);
  int next = pet_wifi_next(&net->data->profiles, net->visible, net->tried);
  if (next >= 0)
    begin_attempt(net, next);
  else {
    net->tried = 0;
    net->attempting = false;
    net->retry_tick = now + pdMS_TO_TICKS(10000);
    network_status_message(net, "暂未连上，稍后重试已保存网络");
  }
}
static void network_task(void *context) {
  network_t *net = context;
  unsigned revision = ~0U;
  for (;;) {
    TickType_t started = xTaskGetTickCount();
    unsigned current = atomic_load(&net->revision);
    if (current != revision) {
      close_client(net);
      atomic_store(&net->configured, load_cloud(net));
      if (!net->initialized) {
        load_profiles(net);
        esp_err_t err = initialize_wifi(net);
        if (err != ESP_OK) {
          network_status_message(net, "Wi-Fi 初始化失败");
          vTaskDelay(pdMS_TO_TICKS(2000));
          continue;
        }
        net->initialized = true;
      }
      revision = current;
    }
    net_command_t command;
    for (unsigned i = 0;
         i < 8 && xQueueReceive(net->commands, &command, 0) == pdTRUE; ++i) {
      process_command(net, &command);
      memset(&command, 0, sizeof(command));
    }
    maintain_wifi(net);
    if (atomic_load(&net->connected) && atomic_load(&net->configured) &&
        time(NULL) > 1700000000) {
      if (fetch_state(net) != ESP_OK)
        ESP_LOGW(TAG, "Cloud fetch failed; retry scheduled");
      if (net->menu == 1 && (!net->roster_tick || started - net->roster_tick >=
                                                      pdMS_TO_TICKS(2000))) {
        fetch_catalog(net);
        net->roster_tick = xTaskGetTickCount();
      }
    }
    if (net->portal && (int32_t)(xTaskGetTickCount() - net->portal_until) >= 0)
      network_portal_stop(net);
    xSemaphoreTake(net->view_lock, portMAX_DELAY);
    if (net->data->view.enrollment_code[0] &&
        time(NULL) > net->data->view.enrollment_expires) {
      net->data->view.enrollment_code[0] = 0;
      snprintf(net->data->view.status, sizeof(net->data->view.status),
               "配对码已过期，请重新生成");
      ++net->data->view.generation;
    }
    xSemaphoreGive(net->view_lock);
    TickType_t spent = xTaskGetTickCount() - started,
               interval = pdMS_TO_TICKS(FETCH_INTERVAL_MS);
    vTaskDelay(spent < interval ? interval - spent : 1);
  }
}
bool app_network_management_poll(void *ctx, app_pet_management_t *out) {
  network_t *net = ctx;
  if (!net || !out || xSemaphoreTake(net->view_lock, 0) != pdTRUE)
    return false;
  *out = net->data->view;
  xSemaphoreGive(net->view_lock);
  return true;
}
bool app_network_management_action(void *ctx,
                                   app_pet_management_action_t action,
                                   unsigned value) {
  network_t *net = ctx;
  if (!net)
    return false;
  net_command_t command = {.kind = action, .value = value};
  return xQueueSend(net->commands, &command, 0) == pdTRUE;
}
static esp_err_t configure(const esp_iris_rpc_request_t *req, uint8_t *response,
                           size_t capacity, size_t *size, void *ctx) {
  network_t *net = ctx;
  if (!req->payload_size || req->payload_size > 1024 || capacity < 2)
    return ESP_ERR_INVALID_ARG;
  network_data_t *cfg = calloc(1, sizeof(*cfg));
  cJSON *root =
      cJSON_ParseWithLength((const char *)req->payload, req->payload_size);
  if (!cfg || !root) {
    free(cfg);
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  bool valid = get_string(root, "url", cfg->url, sizeof(cfg->url)) &&
               strcmp(cfg->url, API "/state") == 0 &&
               get_string(root, "token", cfg->read, sizeof(cfg->read)) &&
               strlen(cfg->read) >= 32;
  bool manage = cJSON_HasObjectItem(root, "manage");
  if (manage)
    valid = valid &&
            get_string(root, "manage", cfg->manage, sizeof(cfg->manage)) &&
            strlen(cfg->manage) >= 32;
  bool wifi = cJSON_HasObjectItem(root, "ssid");
  if (wifi)
    valid =
        valid &&
        get_string(root, "ssid", cfg->legacy_ssid, sizeof(cfg->legacy_ssid)) &&
        get_string(root, "password", cfg->legacy_password,
                   sizeof(cfg->legacy_password));
  cJSON_Delete(root);
  esp_err_t err = valid ? ESP_OK : ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  if (err == ESP_OK)
    err = nvs_open("pet_network", NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_set_str(h, "url", cfg->url);
    if (err == ESP_OK)
      err = nvs_set_str(h, "token", cfg->read);
    if (err == ESP_OK && manage)
      err = nvs_set_str(h, "manage", cfg->manage);
    if (err == ESP_OK)
      err = nvs_commit(h);
    nvs_close(h);
  }
  if (err == ESP_OK && wifi) {
    net_command_t command = {.kind = NET_ADD_PROFILE, .value = 1};
    strcpy(command.profile.ssid, cfg->legacy_ssid);
    strcpy(command.profile.password, cfg->legacy_password);
    if (xQueueSend(net->commands, &command, 0) != pdTRUE)
      err = ESP_ERR_TIMEOUT;
    memset(&command, 0, sizeof(command));
  }
  memset(cfg, 0, sizeof(*cfg));
  free(cfg);
  if (err != ESP_OK)
    return err;
  atomic_fetch_add(&net->revision, 1);
  memcpy(response, "OK", 2);
  *size = 2;
  return ESP_OK;
}
static esp_err_t status(const esp_iris_rpc_request_t *req, uint8_t *response,
                        size_t capacity, size_t *size, void *ctx) {
  (void)req;
  network_t *net = ctx;
  esp_netif_ip_info_t ip = {0};
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta)
    (void)esp_netif_get_ip_info(sta, &ip);
  int n = snprintf((char *)response, capacity,
                   "{\"configured\":%s,\"connected\":%s,\"pulls\":%u,\"last_"
                   "ts\":%u,\"connections\":%u,\"ip\":\"" IPSTR "\"}",
                   atomic_load(&net->configured) ? "true" : "false",
                   atomic_load(&net->connected) ? "true" : "false",
                   atomic_load(&net->pulls), atomic_load(&net->last_ts),
                   atomic_load(&net->connections), IP2STR(&ip.ip));
  if (n < 0 || (size_t)n >= capacity)
    return ESP_ERR_INVALID_SIZE;
  *size = n;
  return ESP_OK;
}
static esp_err_t pair(const esp_iris_rpc_request_t *req, uint8_t *response,
                      size_t cap, size_t *size, void *ctx) {
  (void)ctx;
  esp_iris_status_t *status = calloc(1, sizeof(*status));
  if (!status)
    return ESP_ERR_NO_MEM;
  esp_err_t err = esp_iris_get_status(status);
  bool usb = err == ESP_OK && status->transport == ESP_IRIS_TRANSPORT_KIND_USB;
  free(status);
  if (!usb || req->payload_size != 64 || cap < 2)
    return ESP_ERR_INVALID_ARG;
  char token[65];
  memcpy(token, req->payload, 64);
  token[64] = 0;
  err = esp_iris_pairing_token_set(token);
  memset(token, 0, sizeof(token));
  if (err != ESP_OK)
    return err;
  memcpy(response, "OK", 2);
  *size = 2;
  return ESP_OK;
}
static esp_err_t management_rpc(const esp_iris_rpc_request_t *req,
                                uint8_t *response, size_t cap, size_t *size,
                                void *ctx) {
  network_t *net = ctx;
  if (req->payload_size) {
    cJSON *body =
        cJSON_ParseWithLength((const char *)req->payload, req->payload_size);
    if (!body)
      return ESP_ERR_INVALID_ARG;
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op"),
                *value = cJSON_GetObjectItemCaseSensitive(body, "value");
    net_command_t command = {0};
    bool valid = cJSON_IsString(op);
    const char *name = valid ? op->valuestring : "";
    const char *const names[] = {"screen",        "mode",       "terminal",
                                 "terminal_page", "enroll",     "wifi_page",
                                 "connect",       "promote",    "delete",
                                 "portal",        "portal_stop"};
    bool found = false;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
      if (strcmp(name, names[i]) == 0) {
        command.kind = i;
        found = true;
        break;
      }
    if (strcmp(name, "add") == 0) {
      command.kind = NET_ADD_PROFILE;
      command.value =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "connect"));
      found = true;
      valid = valid &&
              get_string(body, "ssid", command.profile.ssid,
                         sizeof(command.profile.ssid)) &&
              get_string(body, "password", command.profile.password,
                         sizeof(command.profile.password));
    } else if (value) {
      valid = valid && cJSON_IsNumber(value) && value->valueint >= 0 &&
              value->valuedouble == value->valueint;
      command.value = value->valueint;
    }
    cJSON_Delete(body);
    if (!valid || !found)
      return ESP_ERR_INVALID_ARG;
    bool sent = xQueueSend(net->commands, &command, 0) == pdTRUE;
    memset(&command, 0, sizeof(command));
    if (!sent)
      return ESP_ERR_TIMEOUT;
  }
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return ESP_ERR_NO_MEM;
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  app_pet_management_t *v = &net->data->view;
  cJSON_AddNumberToObject(json, "mode", v->mode);
  cJSON_AddNumberToObject(json, "wifi_total", v->wifi_total);
  cJSON_AddNumberToObject(json, "wifi_page", v->wifi_page);
  cJSON_AddNumberToObject(json, "terminal_total", v->terminal_total);
  cJSON_AddStringToObject(json, "selected", v->selected_terminal);
  cJSON_AddBoolToObject(json, "portal_active", v->portal_active);
  cJSON_AddBoolToObject(json, "code_ready", v->enrollment_code[0] != 0);
  cJSON_AddStringToObject(json, "ssid", v->connected_ssid);
  cJSON_AddStringToObject(json, "status", v->status);
  cJSON *rows = cJSON_AddArrayToObject(json, "wifi");
  for (unsigned i = 0; i < 4; ++i)
    if (v->wifi_ssids[i][0])
      cJSON_AddItemToArray(rows, cJSON_CreateString(v->wifi_ssids[i]));
  xSemaphoreGive(net->view_lock);
  char *text = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!text)
    return ESP_ERR_NO_MEM;
  size_t bytes = strlen(text);
  if (bytes > cap) {
    free(text);
    return ESP_ERR_INVALID_SIZE;
  }
  memcpy(response, text, bytes);
  *size = bytes;
  free(text);
  return ESP_OK;
}
esp_err_t app_network_start(app_link_handle_t link, app_network_handle_t *out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  *out = NULL;
  network_t *net = calloc(1, sizeof(*net));
  if (!net)
    return ESP_ERR_NO_MEM;
  net->link = link;
  net->data = heap_caps_calloc(1, sizeof(*net->data),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  net->commands = xQueueCreate(8, sizeof(net_command_t));
  net->view_lock = xSemaphoreCreateMutex();
  if (!net->data || !net->commands || !net->view_lock)
    goto no_memory;
  (void)esp_iris_format_device_id(net->data->viewer);
  load_selection(net);
  esp_err_t err = esp_iris_rpc_register(0x70, 2, configure, net);
  if (err != ESP_OK)
    goto failure;
  err = esp_iris_rpc_register(0x70, 3, status, net);
  if (err != ESP_OK)
    goto unregister;
  err = esp_iris_rpc_register(0x70, 7, pair, net);
  if (err != ESP_OK)
    goto unregister;
  err = esp_iris_rpc_register(0x70, 9, management_rpc, net);
  if (err != ESP_OK)
    goto unregister;
  if (xTaskCreate(network_task, "pet_cloud", 8192, net, 3, NULL) != pdPASS) {
    err = ESP_ERR_NO_MEM;
    goto unregister;
  }
  *out = net;
  return ESP_OK;
unregister:
  (void)esp_iris_rpc_unregister(0x70, 2);
  (void)esp_iris_rpc_unregister(0x70, 3);
  (void)esp_iris_rpc_unregister(0x70, 7);
  (void)esp_iris_rpc_unregister(0x70, 9);
failure:
  vQueueDelete(net->commands);
  vSemaphoreDelete(net->view_lock);
  free(net->data);
  free(net);
  return err;
no_memory:
  if (net->commands)
    vQueueDelete(net->commands);
  if (net->view_lock)
    vSemaphoreDelete(net->view_lock);
  free(net->data);
  free(net);
  return ESP_ERR_NO_MEM;
}

bool app_network_join(void *ctx,const char *ssid,const char *password) {
  network_t *net=ctx;
  if(!net || !ssid || !password || !ssid[0] || strlen(ssid)>32 || strlen(password)>64)return false;
  net_command_t cmd={.kind=NET_TRY_PROFILE};
  strcpy(cmd.profile.ssid,ssid);
  strcpy(cmd.profile.password,password);
  bool sent=xQueueSend(net->commands,&cmd,0)==pdTRUE;
  memset(&cmd,0,sizeof(cmd));
  return sent;
}
