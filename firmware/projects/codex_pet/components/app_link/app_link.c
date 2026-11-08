// SPDX-License-Identifier: Apache-2.0
#include "app_link.h"
#include "esp_iris.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SERVICE 0x70
#define PAYLOAD_MAX 2048
struct app_link { QueueHandle_t queue; app_notify_handle_t notify; };
static bool number(const cJSON *object, const char *key, double minimum, double maximum, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < minimum || v->valuedouble > maximum || floor(v->valuedouble) != v->valuedouble) return false;
    *out = v->valuedouble; return true;
}
static bool string_copy(const cJSON *object, const char *key, char *out, size_t capacity)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(v)) return false;
    const size_t len = strlen(v->valuestring);
    size_t n = len < capacity - 1 ? len : capacity - 1;
    /* Never cut a UTF-8 code point between continuation bytes. */
    while (n && ((unsigned char)v->valuestring[n] & 0xc0) == 0x80) --n;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)v->valuestring[i] < 0x20) return false;
    memcpy(out, v->valuestring, n); out[n] = 0; return true;
}
static bool decode(const cJSON *root, app_pet_snapshot_t *out)
{
    double v;
    if (!number(root, "v", 1, 1, &v) || !number(root, "ts", 1, UINT32_MAX, &v)) return false;
    out->generated_at = (uint32_t)v;
    if (cJSON_HasObjectItem(root,"server_time")) {
        if (!number(root,"server_time",1,UINT32_MAX,&v)) return false;
        out->server_time=(uint32_t)v;
    }
    if (!number(root, "active", 0, 9999, &v)) return false;
    out->active = (unsigned)v;
    if(cJSON_HasObjectItem(root,"done_count")){
        if(!number(root,"done_count",0,8,&v))return false;
        out->done_count=(unsigned)v;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const cJSON *partial = cJSON_GetObjectItemCaseSensitive(root, "partial");
    if (!cJSON_IsBool(ok) || !cJSON_IsBool(partial)) return false;
    out->source_ok = cJSON_IsTrue(ok); out->partial = cJSON_IsTrue(partial);
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state)) return false;
    const char *const names[] = {"offline", "idle", "working", "waiting", "completed", "failed"};
    unsigned i;
    for (i = 0; i < 6; ++i) if (strcmp(state->valuestring, names[i]) == 0) break;
    if (i == 6) return false;
    out->state = (app_pet_state_t)i;
    out->remaining = -1;
    const cJSON *windows = cJSON_GetObjectItemCaseSensitive(root, "windows");
    if (!cJSON_IsArray(windows) || cJSON_GetArraySize(windows) > 2) return false;
    const cJSON *window = cJSON_GetArrayItem(windows, 0);
    if (window) {
        if (!number(window, "remaining", 0, 100, &v)) return false;
        out->remaining = (int)v;
        if (!number(window, "reset_at", 0, UINT32_MAX, &v)) return false;
        out->reset_at = (uint32_t)v;
        if (!string_copy(window, "label", out->quota_label, sizeof(out->quota_label))) return false;
    }
    const cJSON *tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (!cJSON_IsArray(tasks) || cJSON_GetArraySize(tasks) > PET_TASKS) return false;
    for (i = 0; i < (unsigned)cJSON_GetArraySize(tasks); ++i) {
        const cJSON *task=cJSON_GetArrayItem(tasks,i);
        if (!string_copy(task,"title",out->tasks[i],sizeof(out->tasks[i]))) return false;
        const cJSON *unread=cJSON_GetObjectItemCaseSensitive(task,"unread");
        if(unread && !cJSON_IsBool(unread))return false;
        out->task_unread[i]=cJSON_IsTrue(unread);
        const cJSON *task_state=cJSON_GetObjectItemCaseSensitive(task,"state");
        if (cJSON_IsString(task_state)) {
            for (unsigned j=0;j<6;++j) if (strcmp(task_state->valuestring,names[j])==0) {out->task_states[i]=(app_pet_state_t)j;break;}
        }
    }
    const cJSON *source=cJSON_GetObjectItemCaseSensitive(root,"source");
    if(cJSON_IsObject(source) && !string_copy(source,"name",out->source_name,sizeof(out->source_name)))return false;
    if(cJSON_HasObjectItem(root,"quota_label") && !string_copy(root,"quota_label",out->quota_label,sizeof(out->quota_label)))return false;
    const cJSON *done = cJSON_GetObjectItemCaseSensitive(root, "done");
    if (cJSON_IsObject(done)) {
        if (!number(done, "at", 1, out->generated_at, &v)) return false;
        out->done_at = (uint32_t)v;
        if (!string_copy(done, "title", out->done_title, sizeof(out->done_title))) return false;
    } else if (!cJSON_IsNull(done)) return false;
    const cJSON *recent=cJSON_GetObjectItemCaseSensitive(root,"recent");
    if(cJSON_IsObject(recent) && !string_copy(recent,"title",out->recent_title,sizeof(out->recent_title)))return false;
    return true;
}
esp_err_t app_link_submit(app_link_handle_t link, const char *data, size_t size)
{
    if (!link || !data || !size || size > PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_ParseWithLength(data, size);
    if (!root) return ESP_ERR_INVALID_ARG;
    app_pet_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    const bool valid = decode(root, snapshot);
    if(valid)app_notify_observe(link->notify,root);
    cJSON_Delete(root);
    if (!valid) { free(snapshot); return ESP_ERR_INVALID_ARG; }
    BaseType_t sent = xQueueOverwrite(link->queue, snapshot);
    free(snapshot);
    return sent == pdPASS ? ESP_OK : ESP_FAIL;
}
static esp_err_t update(const esp_iris_rpc_request_t *request, uint8_t *response,
                         size_t capacity, size_t *size, void *context)
{
    if (capacity < 2) return ESP_ERR_INVALID_ARG;
    esp_err_t err = app_link_submit(context, (const char *)request->payload, request->payload_size);
    if (err != ESP_OK) return err;
    memcpy(response, "OK", 2); *size = 2;
    return ESP_OK;
}
esp_err_t app_link_create(app_notify_handle_t notify, app_link_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    struct app_link *link = calloc(1, sizeof(*link));
    if (!link) return ESP_ERR_NO_MEM;
    link->notify=notify;
    link->queue = xQueueCreate(1, sizeof(app_pet_snapshot_t));
    if (!link->queue) { free(link); return ESP_ERR_NO_MEM; }
    esp_err_t e = esp_iris_rpc_register(SERVICE, 1, update, link);
    if (e != ESP_OK) { vQueueDelete(link->queue); free(link); return e; }
    *out = link; return ESP_OK;
}
bool app_link_poll(void *context, app_pet_snapshot_t *out)
{
    struct app_link *link = context;
    return xQueueReceive(link->queue, out, 0) == pdTRUE;
}
