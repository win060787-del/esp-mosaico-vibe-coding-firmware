#include "cJSON.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "network_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char form[] =
    "<!doctype html><meta name=viewport "
    "content='width=device-width,initial-scale=1'><meta "
    "charset=utf-8><title>桌宠配网</"
    "title><style>body{background:#101114;color:#fff;font:18px "
    "system-ui;max-width:420px;margin:48px "
    "auto;padding:24px}input,button{box-sizing:border-box;width:100%;padding:"
    "16px;border:0;border-radius:12px;margin:8px 0 "
    "24px;font:inherit}input{background:#25252d;color:#fff}button{background:#"
    "7028e8;color:white}</style><h1>添加 Wi-Fi</h1><form method=post "
    "action=/save><label>网络名称</label><input name=ssid required "
    "autocomplete=off><label>密码</label><input name=password type=password "
    "autocomplete=off><button>保存并连接</button></form>密码只保存在桌宠本地。";
static bool from_hotspot(httpd_req_t *req) {
  network_t *n = req->user_ctx;
  struct sockaddr_in local, peer;
  socklen_t len = sizeof(local);
  esp_netif_ip_info_t ip;
  int fd = httpd_req_to_sockfd(req);
  if (!n->ap || esp_netif_get_ip_info(n->ap, &ip) != ESP_OK ||
      getsockname(fd, (struct sockaddr *)&local, &len) != 0)
    return false;
  len = sizeof(peer);
  if (getpeername(fd, (struct sockaddr *)&peer, &len) != 0)
    return false;
  return local.sin_addr.s_addr == ip.ip.addr &&
         (peer.sin_addr.s_addr & ip.netmask.addr) ==
             (ip.ip.addr & ip.netmask.addr);
}
static esp_err_t index_page(httpd_req_t *req) {
  if (!from_hotspot(req))
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                               "Connect to the pet hotspot first");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, form, HTTPD_RESP_USE_STRLEN);
}
static int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static bool decode(const char *source, size_t bytes, char *target, size_t cap) {
  size_t used = 0;
  for (size_t i = 0; i < bytes; ++i) {
    unsigned char c = (unsigned char)source[i];
    if (c == '+')
      c = ' ';
    else if (c == '%') {
      if (i + 2 >= bytes)
        return false;
      int hi = hex(source[i + 1]), lo = hex(source[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      c = (unsigned char)((hi << 4) | lo);
      i += 2;
    }
    if (c < 32 || used + 1 >= cap)
      return false;
    target[used++] = (char)c;
  }
  target[used] = 0;
  return true;
}
static esp_err_t save_page(httpd_req_t *req) {
  if (!from_hotspot(req))
    return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                               "Connect to the pet hotspot first");
  if (req->content_len <= 0 || req->content_len > 512)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
  char *body = calloc(1, req->content_len + 1);
  if (!body)
    return ESP_ERR_NO_MEM;
  int used = 0;
  while (used < req->content_len) {
    int n = httpd_req_recv(req, body + used, req->content_len - used);
    if (n <= 0) {
      free(body);
      return ESP_FAIL;
    }
    used += n;
  }
  net_command_t cmd = {.kind = NET_ADD_PROFILE, .value = 1};
  bool ssid = false, password = false, valid = true;
  for (char *item = body, *next; item && *item; item = next) {
    next = strchr(item, '&');
    if (next)
      *next++ = 0;
    char *equal = strchr(item, '=');
    if (!equal) {
      valid = false;
      break;
    }
    *equal++ = 0;
    if (strcmp(item, "ssid") == 0)
      ssid = decode(equal, strlen(equal), cmd.profile.ssid,
                    sizeof(cmd.profile.ssid));
    else if (strcmp(item, "password") == 0)
      password = decode(equal, strlen(equal), cmd.profile.password,
                        sizeof(cmd.profile.password));
  }
  memset(body, 0, req->content_len);
  free(body);
  pet_wifi_profiles_t *test = calloc(1, sizeof(*test));
  if (!test)
    return ESP_ERR_NO_MEM;
  test->version = 1;
  valid = valid && ssid && password &&
          pet_wifi_put(test, cmd.profile.ssid, cmd.profile.password);
  memset(test, 0, sizeof(*test));
  free(test);
  network_t *net = req->user_ctx;
  bool queued = valid && xQueueSend(net->commands, &cmd, 0) == pdTRUE;
  memset(&cmd, 0, sizeof(cmd));
  if (!queued)
    return httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        "Please check the network name and password, then retry");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(
      req, "<!doctype html><meta charset=utf-8><meta name=viewport "
           "content='width=device-width,initial-scale=1'><h2>已提交</"
           "h2><p>请回到桌宠查看连接结果。若密码有误，可返回重新输入。</p>");
}
esp_err_t network_portal_start(network_t *net) {
  if (net->portal) {
    net->portal_until = xTaskGetTickCount() + pdMS_TO_TICKS(600000);
    return ESP_OK;
  }
  if (!net->ap)
    net->ap = esp_netif_create_default_wifi_ap();
  if (!net->ap)
    return ESP_ERR_NO_MEM;
  uint8_t random[6];
  esp_fill_random(random, sizeof(random));
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  snprintf(net->data->view.portal_ssid, sizeof(net->data->view.portal_ssid),
           "Codex-Pet-%.4s", net->data->viewer + 28);
  for (unsigned i = 0; i < 6; ++i)
    snprintf(net->data->view.portal_password + i * 2, 3, "%02X", random[i]);
  memset(&net->data->wifi, 0, sizeof(net->data->wifi));
  strcpy((char *)net->data->wifi.ap.ssid, net->data->view.portal_ssid);
  strcpy((char *)net->data->wifi.ap.password, net->data->view.portal_password);
  net->data->wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
  net->data->wifi.ap.max_connection = 2;
  net->data->wifi.ap.channel = 1;
  xSemaphoreGive(net->view_lock);
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err == ESP_OK)
    err = esp_wifi_set_config(WIFI_IF_AP, &net->data->wifi);
  httpd_config_t *config = malloc(sizeof(*config));
  if (!config) {
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    return ESP_ERR_NO_MEM;
  }
  *config = (httpd_config_t)HTTPD_DEFAULT_CONFIG();
  config->server_port = 80;
  config->max_open_sockets = 2;
  config->lru_purge_enable = true;
  if (err == ESP_OK)
    err = httpd_start(&net->portal, config);
  free(config);
  if (err != ESP_OK) {
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    return err;
  }
  httpd_uri_t get = {
      .uri = "/", .method = HTTP_GET, .handler = index_page, .user_ctx = net};
  httpd_uri_t post = {.uri = "/save",
                      .method = HTTP_POST,
                      .handler = save_page,
                      .user_ctx = net};
  err = httpd_register_uri_handler(net->portal, &get);
  if (err == ESP_OK)
    err = httpd_register_uri_handler(net->portal, &post);
  if (err != ESP_OK) {
    network_portal_stop(net);
    return err;
  }
  net->portal_until = xTaskGetTickCount() + pdMS_TO_TICKS(600000);
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  net->data->view.portal_active = true;
  ++net->data->view.generation;
  xSemaphoreGive(net->view_lock);
  return ESP_OK;
}
void network_portal_stop(network_t *net) {
  if (net->portal) {
    httpd_stop(net->portal);
    net->portal = NULL;
  }
  (void)esp_wifi_set_mode(WIFI_MODE_STA);
  xSemaphoreTake(net->view_lock, portMAX_DELAY);
  net->data->view.portal_active = false;
  memset(net->data->view.portal_password, 0,
         sizeof(net->data->view.portal_password));
  ++net->data->view.generation;
  xSemaphoreGive(net->view_lock);
}
