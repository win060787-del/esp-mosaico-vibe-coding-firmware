#include "app_voice.h"
#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define AUDIO_BYTES 3200
#define AUDIO_SLOTS 24
#define AUDIO_BATCH 4
struct voice_config {char url[160],token[65],cert[2048],name[49];};
struct app_voice {
    struct voice_config config;
    SemaphoreHandle_t lock;
    QueueHandle_t commands,audio;StaticQueue_t audio_control;uint8_t audio_storage[AUDIO_SLOTS*AUDIO_BYTES];
    app_notify_handle_t notify;
    app_pet_voice_t view;
    esp_http_client_handle_t http;
    esp_codec_dev_handle_t mic;
    atomic_bool capture,failed,capturing,screen;
    uint8_t capture_buffer[AUDIO_BYTES],send_buffer[AUDIO_BYTES*AUDIO_BATCH];
    char response[4096],url[192],authorization[80],device[33];
    unsigned response_size;atomic_uint sequence,probe_chunks,peak;atomic_int mic_error,last_http,last_transport;
};
static void status(struct app_voice *v,const char *state,const char *error){
    xSemaphoreTake(v->lock,portMAX_DELAY);
    snprintf(v->view.state,sizeof(v->view.state),"%s",state);
    snprintf(v->view.error,sizeof(v->view.error),"%s",error?error:"");
    v->view.recording=!strcmp(state,"recording");
    v->view.busy=!strcmp(state,"warming")||!strcmp(state,"processing")||!strcmp(state,"sending");
    ++v->view.revision;xSemaphoreGive(v->lock);
}
static esp_err_t http_event(esp_http_client_event_t *e){
    struct app_voice *v=e->user_data;
    if(e->event_id==HTTP_EVENT_ON_DATA){
        if(e->data_len<0 || v->response_size+(unsigned)e->data_len>=sizeof(v->response))return ESP_FAIL;
        memcpy(v->response+v->response_size,e->data,e->data_len);v->response_size+=e->data_len;
    }return ESP_OK;
}
static void copy_field(cJSON *j,const char *key,char *out,size_t size){
    cJSON *s=cJSON_GetObjectItemCaseSensitive(j,key);if(!cJSON_IsString(s))return;
    size_t n=strlen(s->valuestring);if(n>=size)n=size-1;
    while(n && ((unsigned char)s->valuestring[n]&0xc0)==0x80)--n;
    memcpy(out,s->valuestring,n);out[n]=0;
}
static bool request(struct app_voice *v,const char *path,const void *data,size_t length){
    if(!v->http){
        esp_http_client_config_t cfg={.url=v->config.url,.cert_pem=v->config.cert,.timeout_ms=3000,.buffer_size=4096,.buffer_size_tx=1024,.event_handler=http_event,.user_data=v,.keep_alive_enable=true};
        v->http=esp_http_client_init(&cfg);if(!v->http)return false;
        snprintf(v->authorization,sizeof(v->authorization),"Bearer %s",v->config.token);
        esp_http_client_set_header(v->http,"Authorization",v->authorization);
        esp_http_client_set_header(v->http,"X-Device",v->device);
    }
    snprintf(v->url,sizeof(v->url),"%s%s",v->config.url,path);
    esp_http_client_set_url(v->http,v->url);esp_http_client_set_method(v->http,HTTP_METHOD_POST);
    char seq[20];snprintf(seq,sizeof(seq),"%u",v->sequence);esp_http_client_set_header(v->http,"X-Sequence",seq);
    esp_http_client_set_header(v->http,"Content-Type",!strcmp(path,"/audio")?"application/octet-stream":"application/json");
    esp_http_client_set_post_field(v->http,data,length);v->response_size=0;
    esp_err_t err=esp_http_client_perform(v->http);v->response[v->response_size]=0;
    int code=esp_http_client_get_status_code(v->http);atomic_store(&v->last_http,code);atomic_store(&v->last_transport,err);
    if(err!=ESP_OK || code!=200){
        if(err==ESP_OK && code==409){
            cJSON *failure=cJSON_Parse(v->response);char message[193]="操作状态已变化，请重新开始";
            if(failure){copy_field(failure,"error",message,sizeof(message));cJSON_Delete(failure);}
            status(v,"error",message);
        }
        esp_http_client_cleanup(v->http);v->http=NULL;return false;
    }
    cJSON *j=cJSON_Parse(v->response);if(!j)return false;
    xSemaphoreTake(v->lock,portMAX_DELAY);
    copy_field(j,"text",v->view.text,sizeof(v->view.text));copy_field(j,"name",v->view.name,sizeof(v->view.name));
    copy_field(j,"state",v->view.state,sizeof(v->view.state));copy_field(j,"error",v->view.error,sizeof(v->view.error));
    cJSON *level=cJSON_GetObjectItemCaseSensitive(j,"level");v->view.level=cJSON_IsNumber(level)?(unsigned)level->valueint:0;
    v->view.recording=!strcmp(v->view.state,"recording");
    v->view.busy=!strcmp(v->view.state,"warming")||!strcmp(v->view.state,"processing")||!strcmp(v->view.state,"sending");
    v->view.connected=true;++v->view.revision;xSemaphoreGive(v->lock);cJSON_Delete(j);return true;
}
static bool control(struct app_voice *v,const char *action){
    cJSON *j=cJSON_CreateObject();if(!j)return false;
    char id[32];snprintf(id,sizeof(id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
    cJSON_AddStringToObject(j,"action",action);cJSON_AddStringToObject(j,"request",id);cJSON_AddStringToObject(j,"device",v->device);
    char *body=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!body)return false;
    bool ok=request(v,"/control",body,strlen(body));free(body);return ok;
}
static void capture_task(void *ctx){
    struct app_voice *v=ctx;
    for(;;){
        if(!atomic_load(&v->capture)){vTaskDelay(pdMS_TO_TICKS(20));continue;}
        atomic_store(&v->capturing,true);
        bool locked=app_notify_audio_acquire(v->notify),opened=false;
        if(locked){
            if(!v->mic)v->mic=bsp_audio_codec_microphone_init();
            esp_codec_dev_sample_info_t info={.sample_rate=16000,.bits_per_sample=16,.channel=1};
            opened=v->mic && esp_codec_dev_open(v->mic,&info)==ESP_CODEC_DEV_OK;
            if(opened)esp_codec_dev_set_in_gain(v->mic,24.0f);
        }
        if(!opened){atomic_store(&v->mic_error,-1);atomic_store(&v->failed,true);}
        while(opened && atomic_load(&v->capture)){
            int err=esp_codec_dev_read(v->mic,v->capture_buffer,AUDIO_BYTES);
            if(err!=ESP_CODEC_DEV_OK || xQueueSend(v->audio,v->capture_buffer,pdMS_TO_TICKS(50))!=pdTRUE){atomic_store(&v->mic_error,err?err:-2);atomic_store(&v->failed,true);break;}
            unsigned peak=0;int16_t *pcm=(int16_t*)v->capture_buffer;
            for(unsigned i=0;i<AUDIO_BYTES/2;++i){unsigned sample=pcm[i]<0?-(int)pcm[i]:pcm[i];if(sample>peak)peak=sample;}
            atomic_store(&v->peak,peak);
        }
        if(opened)esp_codec_dev_close(v->mic);
        if(locked)app_notify_audio_release(v->notify);
        atomic_store(&v->capture,false);atomic_store(&v->capturing,false);
    }
}
static bool upload_batch(struct app_voice *v){
    unsigned count=0;
    while(count<AUDIO_BATCH && xQueueReceive(v->audio,v->send_buffer+count*AUDIO_BYTES,0)==pdTRUE)++count;
    if(!count)return true;
    if(!request(v,"/audio",v->send_buffer,count*AUDIO_BYTES))return false;
    ++v->sequence;
    return true;
}
static bool drain(struct app_voice *v){
    atomic_store(&v->capture,false);
    while(atomic_load(&v->capturing))vTaskDelay(pdMS_TO_TICKS(10));
    while(uxQueueMessagesWaiting(v->audio))if(!upload_batch(v))return false;
    return true;
}
static void worker(void *ctx){
    struct app_voice *v=ctx;char action[16];int64_t poll_at=0;bool starting=false;
    for(;;){
        if(xQueueReceive(v->commands,action,pdMS_TO_TICKS(25))==pdTRUE){
            if(!strcmp(action,"probe")){
                xQueueReset(v->audio);atomic_store(&v->probe_chunks,0);atomic_store(&v->mic_error,0);atomic_store(&v->capture,true);
                int64_t until=esp_timer_get_time()+1500000;
                while(esp_timer_get_time()<until && !atomic_load(&v->failed)){
                    if(xQueueReceive(v->audio,v->send_buffer,pdMS_TO_TICKS(100))==pdTRUE)atomic_fetch_add(&v->probe_chunks,1);
                }
                atomic_store(&v->capture,false);while(atomic_load(&v->capturing))vTaskDelay(pdMS_TO_TICKS(10));
                xQueueReset(v->audio);atomic_store(&v->failed,false);continue;
            }
            if(!v->config.url[0]){status(v,"error","请先在 Mac 配对语音输入");continue;}
            if(strcmp(action,"start") && !drain(v))goto offline;
            if(!control(v,action))goto offline;
            starting=!strcmp(action,"start");
            if(starting){atomic_store(&v->mic_error,0);v->sequence=0;status(v,"warming",NULL);}else status(v,"processing",NULL);
            poll_at=0;
        }
        if(atomic_exchange(&v->failed,false)){
            atomic_store(&v->capture,false);(void)drain(v);(void)control(v,"stop");status(v,"error","收音或传输中断，请重试");starting=false;continue;
        }
        if(uxQueueMessagesWaiting(v->audio)>=AUDIO_BATCH){
            if(!upload_batch(v))goto offline;
            if(!v->view.recording)atomic_store(&v->capture,false);
        }
        if(!atomic_load(&v->capture) && (atomic_load(&v->screen)||starting||v->view.busy) && esp_timer_get_time()>poll_at && v->config.url[0]){
            poll_at=esp_timer_get_time()+350000;
            if(!request(v,"/state","{}",2))goto offline;
            if(starting && v->view.recording){starting=false;xQueueReset(v->audio);atomic_store(&v->capture,true);}
            else if(!strcmp(v->view.state,"error"))starting=false;
        }
        continue;
    offline:
        atomic_store(&v->capture,false);while(atomic_load(&v->capturing))vTaskDelay(pdMS_TO_TICKS(10));
        xQueueReset(v->audio);starting=false;
        bool rejected=atomic_load(&v->last_transport)==ESP_OK && atomic_load(&v->last_http)==409;
        xSemaphoreTake(v->lock,portMAX_DELAY);v->view.connected=rejected;xSemaphoreGive(v->lock);
        if(!rejected)status(v,"error","无法连接 Mac，已暂停输入");
        poll_at=esp_timer_get_time()+3000000;
    }
}
static esp_err_t configure(const esp_iris_rpc_request_t *r,uint8_t *out,size_t cap,size_t *size,void *ctx){
    (void)ctx;if(cap<2 || r->payload_size>4096)return ESP_ERR_INVALID_ARG;
    cJSON *j=cJSON_ParseWithLength((const char*)r->payload,r->payload_size);if(!j)return ESP_ERR_INVALID_ARG;
    struct voice_config *cfg=calloc(1,sizeof(*cfg));if(!cfg){cJSON_Delete(j);return ESP_ERR_NO_MEM;}
    copy_field(j,"url",cfg->url,sizeof(cfg->url));copy_field(j,"token",cfg->token,sizeof(cfg->token));copy_field(j,"cert",cfg->cert,sizeof(cfg->cert));copy_field(j,"name",cfg->name,sizeof(cfg->name));cJSON_Delete(j);
    bool valid=!strncmp(cfg->url,"https://",8)&&strlen(cfg->token)==64&&strstr(cfg->cert,"-----BEGIN CERTIFICATE-----")&&strstr(cfg->cert,"-----END CERTIFICATE-----");
    esp_err_t err=ESP_ERR_INVALID_ARG;nvs_handle_t nvs;
    if(valid && nvs_open("pet_voice",NVS_READWRITE,&nvs)==ESP_OK){err=nvs_set_blob(nvs,"config",cfg,sizeof(*cfg));if(err==ESP_OK)err=nvs_commit(nvs);nvs_close(nvs);}
    /* Configuration applies on reboot, avoiding concurrent transport mutation. */
    memset(cfg,0,sizeof(*cfg));free(cfg);if(err==ESP_OK){memcpy(out,"OK",2);*size=2;}return err;
}
static esp_err_t diagnostics(const esp_iris_rpc_request_t *r,uint8_t *out,size_t cap,size_t *size,void *ctx){
    struct app_voice *v=ctx;
    if(r->payload_size){cJSON *j=cJSON_ParseWithLength((const char*)r->payload,r->payload_size);bool probe=j && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"probe"));cJSON_Delete(j);if(probe && !atomic_load(&v->capture)){char action[16]="probe";if(xQueueSend(v->commands,action,0)!=pdTRUE)return ESP_ERR_TIMEOUT;}}
    xSemaphoreTake(v->lock,portMAX_DELAY);
    int n=snprintf((char*)out,cap,"{\"configured\":%s,\"connected\":%s,\"recording\":%s,\"state\":\"%s\",\"level\":%u,\"chunks\":%u,\"probe_chunks\":%u,\"peak\":%u,\"mic_error\":%d,\"http\":%d,\"transport\":%d,\"internal_free\":%u}",v->config.url[0]?"true":"false",v->view.connected?"true":"false",v->view.recording?"true":"false",v->view.state,v->view.level,(unsigned)v->sequence,(unsigned)v->probe_chunks,(unsigned)v->peak,(int)v->mic_error,(int)v->last_http,(int)v->last_transport,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    xSemaphoreGive(v->lock);if(n<0 || (size_t)n>=cap)return ESP_ERR_INVALID_SIZE;*size=n;return ESP_OK;
}
esp_err_t app_voice_create(app_notify_handle_t notify,app_voice_handle_t *out){
    if(!out || !notify)return ESP_ERR_INVALID_ARG;
    struct app_voice *v=heap_caps_calloc(1,sizeof(*v),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!v)return ESP_ERR_NO_MEM;
    v->notify=notify;v->lock=xSemaphoreCreateMutex();v->commands=xQueueCreate(8,16);v->audio=xQueueCreateStatic(AUDIO_SLOTS,AUDIO_BYTES,v->audio_storage,&v->audio_control);
    if(!v->lock || !v->commands || !v->audio){if(v->lock)vSemaphoreDelete(v->lock);if(v->commands)vQueueDelete(v->commands);if(v->audio)vQueueDelete(v->audio);free(v);return ESP_ERR_NO_MEM;}
    nvs_handle_t nvs;if(nvs_open("pet_voice",NVS_READONLY,&nvs)==ESP_OK){size_t len=sizeof(v->config);if(nvs_get_blob(nvs,"config",&v->config,&len)!=ESP_OK)memset(&v->config,0,sizeof(v->config));nvs_close(nvs);}
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);snprintf(v->device,sizeof(v->device),"pet-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(v->view.name,sizeof(v->view.name),"%s",v->config.name);status(v,"idle",NULL);
    TaskHandle_t mic_task=NULL,network_task=NULL;
    esp_err_t registered=esp_iris_rpc_register(0x70,11,configure,v);
    bool registered_config=registered==ESP_OK,registered_status=false;
    if(registered_config){registered=esp_iris_rpc_register(0x70,12,diagnostics,v);registered_status=registered==ESP_OK;}
    if(registered_status && xTaskCreate(capture_task,"pet_mic",4096,v,5,&mic_task)==pdPASS &&
       xTaskCreate(worker,"pet_voice",8192,v,3,&network_task)==pdPASS){*out=v;return ESP_OK;}
    if(mic_task)vTaskDelete(mic_task);
    if(registered_status)esp_iris_rpc_unregister(0x70,12);
    if(registered_config)esp_iris_rpc_unregister(0x70,11);
    vQueueDelete(v->commands);vQueueDelete(v->audio);vSemaphoreDelete(v->lock);free(v);
    return registered==ESP_OK?ESP_ERR_NO_MEM:registered;
}
bool app_voice_poll(void *ctx,app_pet_voice_t *out){struct app_voice *v=ctx;if(xSemaphoreTake(v->lock,0)!=pdTRUE)return false;*out=v->view;xSemaphoreGive(v->lock);return true;}
bool app_voice_action(void *ctx,const char *action){struct app_voice *v=ctx;if(!strcmp(action,"open")){atomic_store(&v->screen,true);return true;}if(!strcmp(action,"leave"))atomic_store(&v->screen,false);char cmd[16];snprintf(cmd,sizeof(cmd),"%s",action);return xQueueSend(v->commands,cmd,0)==pdTRUE;}
