#include "app_settings.h"
#include "bsp/display.h"
#include "esp_iris.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {app_pet_action_t action;unsigned value;} change_t;
struct app_settings {QueueHandle_t queue;app_notify_handle_t notify;atomic_uint volume,brightness,color;atomic_bool vibration,sound,saved;};
static esp_err_t persist(struct app_settings *s)
{
    nvs_handle_t h;esp_err_t err=nvs_open("pet_settings",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_set_u8(h,"volume",atomic_load(&s->volume));
    if(err==ESP_OK)err=nvs_set_u8(h,"brightness",atomic_load(&s->brightness));
    if(err==ESP_OK)err=nvs_set_u8(h,"vibration",atomic_load(&s->vibration));
    if(err==ESP_OK)err=nvs_set_u8(h,"sound",atomic_load(&s->sound));
    if(err==ESP_OK)err=nvs_set_u8(h,"color",atomic_load(&s->color));
    if(err==ESP_OK)err=nvs_set_u8(h,"schema",2);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);return err;
}
static void worker(void *ctx)
{
    struct app_settings *s=ctx;change_t change;bool dirty=true;TickType_t last=xTaskGetTickCount();
    for(;;){
        if(xQueueReceive(s->queue,&change,pdMS_TO_TICKS(100))==pdTRUE){
            if(change.action==PET_PREVIEW){(void)app_notify_preview(s->notify);continue;}
            if(change.action==PET_SET_VOLUME)atomic_store(&s->volume,change.value);
            else if(change.action==PET_SET_SOUND)atomic_store(&s->sound,change.value!=0);
            else if(change.action==PET_SET_COLOR)atomic_store(&s->color,change.value);
            else if(change.action==PET_TOGGLE_VIBRATION)atomic_store(&s->vibration,!atomic_load(&s->vibration));
            else if(change.action==PET_SET_BRIGHTNESS){
                if(bsp_display_brightness_set(change.value)!=ESP_OK){atomic_store(&s->saved,false);continue;}
                atomic_store(&s->brightness,change.value);
            }
            app_notify_set_options(s->notify,atomic_load(&s->volume),atomic_load(&s->vibration),atomic_load(&s->sound));
            dirty=true;last=xTaskGetTickCount();
        }
        if(dirty && xTaskGetTickCount()-last>=pdMS_TO_TICKS(750)){
            esp_err_t err=persist(s);atomic_store(&s->saved,err==ESP_OK);
            if(err==ESP_OK)dirty=false;else last=xTaskGetTickCount();
        }
    }
}
bool app_settings_poll(void *ctx,app_pet_preferences_t *out)
{
    struct app_settings *s=ctx;if(!s || !out)return false;
    char token[65];bool ready=esp_iris_pairing_token_get(token)==ESP_OK;memset(token,0,sizeof(token));
    *out=(app_pet_preferences_t){.volume=atomic_load(&s->volume),.brightness=atomic_load(&s->brightness),.vibration=atomic_load(&s->vibration),.sound=atomic_load(&s->sound),.color=atomic_load(&s->color),.wireless_ready=ready,.save_error=!atomic_load(&s->saved)};
    snprintf(out->version,sizeof(out->version),"%.15s",esp_app_get_description()->version);return true;
}
bool app_settings_action(void *ctx,app_pet_action_t action,unsigned value)
{
    struct app_settings *s=ctx;if(!s)return false;
    if(action==PET_SET_VOLUME && value>100)return false;
    if(action==PET_SET_BRIGHTNESS && (value<10 || value>100))return false;
    if(action==PET_SET_SOUND && value>1)return false;
    if(action==PET_SET_COLOR && value>=6)return false;
    change_t c={action,value};return xQueueSend(s->queue,&c,0)==pdTRUE;
}
static esp_err_t control(const esp_iris_rpc_request_t *req,uint8_t *response,size_t cap,size_t *size,void *ctx)
{
    struct app_settings *s=ctx;
    if(req->payload_size){
        cJSON *json=cJSON_ParseWithLength((const char *)req->payload,req->payload_size);
        if(!json)return ESP_ERR_INVALID_ARG;
        bool ok=true;
        const cJSON *volume=cJSON_GetObjectItemCaseSensitive(json,"volume");
        const cJSON *brightness=cJSON_GetObjectItemCaseSensitive(json,"brightness");
        const cJSON *sound=cJSON_GetObjectItemCaseSensitive(json,"sound");
        const cJSON *color=cJSON_GetObjectItemCaseSensitive(json,"color");
        const cJSON *toggle=cJSON_GetObjectItemCaseSensitive(json,"toggle_vibration");
        if(volume)ok=cJSON_IsNumber(volume) && volume->valuedouble==volume->valueint && volume->valueint>=0 && volume->valueint<=100;
        if(brightness)ok=ok && cJSON_IsNumber(brightness) && brightness->valuedouble==brightness->valueint && brightness->valueint>=10 && brightness->valueint<=100;
        if(toggle)ok=ok && cJSON_IsBool(toggle);
        if(sound)ok=ok && cJSON_IsBool(sound);
        if(color)ok=ok && cJSON_IsNumber(color) && color->valuedouble==color->valueint && color->valueint>=0 && color->valueint<6;
        unsigned changes=(volume!=NULL)+(brightness!=NULL)+(sound!=NULL)+(color!=NULL)+cJSON_IsTrue(toggle);
        if(ok && uxQueueSpacesAvailable(s->queue)<changes){cJSON_Delete(json);return ESP_ERR_TIMEOUT;}
        if(ok && volume)ok=app_settings_action(s,PET_SET_VOLUME,volume->valueint);
        if(ok && brightness)ok=app_settings_action(s,PET_SET_BRIGHTNESS,brightness->valueint);
        if(ok && cJSON_IsTrue(toggle))ok=app_settings_action(s,PET_TOGGLE_VIBRATION,0);
        if(ok && sound)ok=app_settings_action(s,PET_SET_SOUND,cJSON_IsTrue(sound));
        if(ok && color)ok=app_settings_action(s,PET_SET_COLOR,color->valueint);
        cJSON_Delete(json);if(!ok)return ESP_ERR_INVALID_ARG;
    }
    uint8_t schema=0,persisted_sound=0,persisted_color=0,persisted_volume=0;nvs_handle_t h;
    if(nvs_open("pet_settings",NVS_READONLY,&h)==ESP_OK){
        (void)nvs_get_u8(h,"schema",&schema);(void)nvs_get_u8(h,"sound",&persisted_sound);
        (void)nvs_get_u8(h,"color",&persisted_color);(void)nvs_get_u8(h,"volume",&persisted_volume);nvs_close(h);
    }
    int n=snprintf((char *)response,cap,"{\"volume\":%u,\"brightness\":%u,\"vibration\":%s,\"sound\":%s,\"color\":%u,\"saved\":%s,\"schema\":%u,\"persisted_volume\":%u,\"persisted_sound\":%s,\"persisted_color\":%u}",atomic_load(&s->volume),atomic_load(&s->brightness),atomic_load(&s->vibration)?"true":"false",atomic_load(&s->sound)?"true":"false",atomic_load(&s->color),atomic_load(&s->saved)?"true":"false",schema,persisted_volume,persisted_sound?"true":"false",persisted_color);
    if(n<0 || (size_t)n>=cap)return ESP_ERR_INVALID_SIZE;
    *size=n;return ESP_OK;
}
esp_err_t app_settings_create(app_notify_handle_t notify,app_settings_handle_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    *out=NULL;
    struct app_settings *s=calloc(1,sizeof(*s));if(!s)return ESP_ERR_NO_MEM;
    s->notify=notify;uint8_t volume=50,brightness=100,vibration=1,sound=1,color=0,schema=0;nvs_handle_t h;
    if(nvs_open("pet_settings",NVS_READONLY,&h)==ESP_OK){
        (void)nvs_get_u8(h,"volume",&volume);(void)nvs_get_u8(h,"brightness",&brightness);(void)nvs_get_u8(h,"vibration",&vibration);
        (void)nvs_get_u8(h,"sound",&sound);(void)nvs_get_u8(h,"color",&color);(void)nvs_get_u8(h,"schema",&schema);nvs_close(h);
    }
    /* One-time migration to the quieter 50% default. Retain an existing mute,
     * brightness and vibration preference; future boots preserve the new level. */
    if(schema!=2){sound=volume!=0;volume=50;}
    atomic_init(&s->volume,volume<=100?volume:50);atomic_init(&s->brightness,brightness>=10 && brightness<=100?brightness:100);
    atomic_init(&s->color,color<6?color:0);atomic_init(&s->vibration,vibration!=0);atomic_init(&s->sound,sound!=0);atomic_init(&s->saved,true);
    s->queue=xQueueCreate(16,sizeof(change_t));if(!s->queue){free(s);return ESP_ERR_NO_MEM;}
    esp_err_t err=esp_iris_rpc_register(0x70,8,control,s);if(err!=ESP_OK){vQueueDelete(s->queue);free(s);return err;}
    if(xTaskCreate(worker,"pet_settings",4096,s,2,NULL)!=pdPASS){(void)esp_iris_rpc_unregister(0x70,8);vQueueDelete(s->queue);free(s);return ESP_ERR_NO_MEM;}
    app_notify_set_options(notify,atomic_load(&s->volume),atomic_load(&s->vibration),atomic_load(&s->sound));
    (void)bsp_display_brightness_set(atomic_load(&s->brightness));
    *out=s;return ESP_OK;
}
