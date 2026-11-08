// SPDX-License-Identifier: Apache-2.0
#include "app_notify.h"
#include "notification_policy.h"
#include "audio_level.h"
#include "bsp/audio.h"
#include "bsp/esp_mosaico.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "bsp/motor.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 16000U
#define TONE_AMPLITUDE 9500.0f
#define TONE_FRAMES 12800U
#define CHUNK_FRAMES 128U
struct app_notify {
    SemaphoreHandle_t audio_lock;
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    notification_policy_t policy;
    esp_codec_dev_handle_t speaker;
    int16_t pcm[CHUNK_FRAMES];
    int16_t *tone;
    int tone_peak;
    atomic_uint stream_ms,volume,merged,last_batch,applied_volume,pcm_peak,audible_plays,muted_events;
    atomic_bool vibration,sound;
    unsigned buffered_volume;
    atomic_uint queued,played,tests,dropped;
    atomic_int motor_error,audio_error;
};
static const char *TAG="pet_notify";
extern const uint8_t chime_pcm[] asm("_binary_chime_pcm_start");
static esp_err_t pulse_motor(void)
{
    esp_err_t result=ESP_OK;
    for(unsigned i=0;i<2;++i){
        esp_err_t err=bsp_motor_set_strength(60);
        if(err!=ESP_OK)result=err;
        vTaskDelay(pdMS_TO_TICKS(90));
        err=bsp_motor_set(false);if(err!=ESP_OK)result=err;
        if(i==0)vTaskDelay(pdMS_TO_TICKS(80));
    }
    return result;
}
static int play_tone(struct app_notify *n,bool diagnostic)
{
    unsigned volume=atomic_load(&n->volume);
    atomic_store(&n->applied_volume,volume);
    if(!volume || !atomic_load(&n->sound)){
        atomic_store(&n->pcm_peak,0);atomic_store(&n->stream_ms,0);atomic_fetch_add(&n->muted_events,1);
        return ESP_OK;
    }
    if(n->buffered_volume!=volume){
        n->tone_peak=0;
        for(unsigned frame=0;frame<TONE_FRAMES;++frame){
            int16_t base=(int16_t)((uint16_t)chime_pcm[2*frame]|((uint16_t)chime_pcm[2*frame+1]<<8));
            n->tone[frame]=notification_scale_sample(base,volume);
            if(abs(n->tone[frame])>n->tone_peak)n->tone_peak=abs(n->tone[frame]);
        }
        n->buffered_volume=volume;
    }
    if(!n->speaker)n->speaker=bsp_audio_codec_speaker_init();
    if(!n->speaker)return ESP_FAIL;
    esp_codec_dev_sample_info_t info={.sample_rate=SAMPLE_RATE,.bits_per_sample=16,.channel=1};
    int err=esp_codec_dev_open(n->speaker,&info);
    if(err!=ESP_CODEC_DEV_OK)return err;
    err=esp_codec_dev_set_out_vol(n->speaker,100);
    if(err==ESP_CODEC_DEV_OK)err=esp_codec_dev_set_out_mute(n->speaker,false);
    memset(n->pcm,0,sizeof(n->pcm));
    for(unsigned i=0;i<12 && err==ESP_CODEC_DEV_OK;++i)
        err=esp_codec_dev_write(n->speaker,n->pcm,sizeof(n->pcm));
    int peak=n->tone_peak;
    int64_t stream_start=esp_timer_get_time();
    if(!diagnostic){
        /* One blocking DMA-fed write, using prebuffered writable RAM. */
        if(err==ESP_CODEC_DEV_OK)err=esp_codec_dev_write(n->speaker,n->tone,TONE_FRAMES*sizeof(int16_t));
    }else{
        const unsigned frames=25600U;peak=0;
        for(unsigned start=0;start<frames && err==ESP_CODEC_DEV_OK;start+=CHUNK_FRAMES){
            for(unsigned i=0;i<CHUNK_FRAMES;++i){
                unsigned frame=start+i;
                float envelope=1.0f-(float)frame/frames;
                if(frame<480)envelope*=(float)frame/480;
                float phase=6.28318530718f*1046.5f*(float)frame/SAMPLE_RATE;
                int16_t value=notification_scale_sample((int16_t)(TONE_AMPLITUDE*envelope*sinf(phase)),volume);
                n->pcm[i]=value;
                if(abs(value)>peak)peak=abs(value);
            }
            err=esp_codec_dev_write(n->speaker,n->pcm,sizeof(n->pcm));
        }
    }
    atomic_store(&n->stream_ms,(unsigned)((esp_timer_get_time()-stream_start)/1000));
    memset(n->pcm,0,sizeof(n->pcm));
    for(unsigned i=0;i<15 && err==ESP_CODEC_DEV_OK;++i)
        err=esp_codec_dev_write(n->speaker,n->pcm,sizeof(n->pcm));
    int mute=-1,volume_reg=-1,power=-1;
    int read_mute=esp_codec_dev_read_reg(n->speaker,0x31,&mute);
    int read_volume=esp_codec_dev_read_reg(n->speaker,0x32,&volume_reg);
    int read_power=esp_codec_dev_read_reg(n->speaker,0x14,&power);
    unsigned pa=(REG_READ(BSP_AUDIO_PA_CTRL<32?GPIO_OUT_REG:GPIO_OUT1_REG)>>(BSP_AUDIO_PA_CTRL%32))&1U;
    ESP_LOGI(TAG,"Audio output: peak=%d PA=%u mute=0x%02x volume=0x%02x power=0x%02x reads=%d/%d/%d",peak,pa,mute,volume_reg,power,read_mute,read_volume,read_power);
    atomic_store(&n->pcm_peak,peak);
    if(err==ESP_CODEC_DEV_OK)atomic_fetch_add(&n->audible_plays,1);
    (void)esp_codec_dev_set_out_mute(n->speaker,true);
    int close_error=esp_codec_dev_close(n->speaker);
    return err==ESP_CODEC_DEV_OK?close_error:err;
}
static void worker(void *context)
{
    struct app_notify *n=context;int pending=-1;
    for(;;){
        uint8_t test;
        if(pending>=0){test=(uint8_t)pending;pending=-1;}
        else if(xQueueReceive(n->queue,&test,portMAX_DELAY)!=pdTRUE)continue;
        unsigned batch=1;
        if(test==0 || test==3){
            TickType_t began=xTaskGetTickCount();
            while(batch<8){
                TickType_t elapsed=xTaskGetTickCount()-began;
                if(elapsed>=pdMS_TO_TICKS(1000))break;
                uint8_t next;
                if(xQueueReceive(n->queue,&next,pdMS_TO_TICKS(1000)-elapsed)!=pdTRUE)break;
                if(next!=0 && next!=3){pending=next;break;}
                if(next==3)test=3;
                ++batch;
            }
            atomic_fetch_add(&n->merged,batch-1);
        }
        atomic_store(&n->last_batch,batch);
        int motor=test==2 || !atomic_load(&n->vibration)?ESP_OK:pulse_motor();atomic_store(&n->motor_error,motor);
        int audio=ESP_OK;
        if(xSemaphoreTake(n->audio_lock,0)==pdTRUE){audio=play_tone(n,test==2);xSemaphoreGive(n->audio_lock);}atomic_store(&n->audio_error,audio);
        atomic_fetch_add(&n->played,1);if(test)atomic_fetch_add(&n->tests,1);
        ESP_LOGI(TAG,"%s alert: batch=%u motor=%d audio=%d played=%u",test?"Self-test":"Completion",batch,motor,audio,atomic_load(&n->played));
    }
}

static esp_err_t self_test(const esp_iris_rpc_request_t *req,uint8_t *response,size_t capacity,size_t *size,void *ctx)
{
    struct app_notify *n=ctx;uint8_t test=1;unsigned burst=1;
    if(req->payload_size){
        cJSON *body=cJSON_ParseWithLength((const char *)req->payload,req->payload_size);
        if(!body)return ESP_ERR_INVALID_ARG;
        if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body,"audio_only")))test=2;
        const cJSON *count=cJSON_GetObjectItemCaseSensitive(body,"burst");
        if(count){
            if(!cJSON_IsNumber(count) || count->valueint<1 || count->valueint>8 || count->valuedouble!=count->valueint){cJSON_Delete(body);return ESP_ERR_INVALID_ARG;}
            burst=count->valueint;test=3;
        }
        cJSON_Delete(body);
    }
    if(capacity<2)return ESP_ERR_INVALID_SIZE;
    if(uxQueueSpacesAvailable(n->queue)<burst)return ESP_ERR_TIMEOUT;
    for(unsigned i=0;i<burst;++i)if(xQueueSend(n->queue,&test,0)!=pdTRUE)return ESP_ERR_TIMEOUT;
    memcpy(response,"OK",2);*size=2;return ESP_OK;
}
static esp_err_t status(const esp_iris_rpc_request_t *req,uint8_t *response,size_t capacity,size_t *size,void *ctx)
{
    (void)req;struct app_notify *n=ctx;
    xSemaphoreTake(n->lock,portMAX_DELAY);bool armed=n->policy.armed;xSemaphoreGive(n->lock);
    int count=snprintf((char *)response,capacity,"{\"armed\":%s,\"queued\":%u,\"played\":%u,\"self_tests\":%u,\"dropped\":%u,\"motor_error\":%d,\"audio_error\":%d,\"stream_ms\":%u,\"merged\":%u,\"last_batch\":%u,\"volume\":%u,\"vibration\":%s,\"sound\":%s,\"applied_volume\":%u,\"pcm_peak\":%u,\"audible_plays\":%u,\"muted_events\":%u}",armed?"true":"false",atomic_load(&n->queued),atomic_load(&n->played),atomic_load(&n->tests),atomic_load(&n->dropped),atomic_load(&n->motor_error),atomic_load(&n->audio_error),atomic_load(&n->stream_ms),atomic_load(&n->merged),atomic_load(&n->last_batch),atomic_load(&n->volume),atomic_load(&n->vibration)?"true":"false",atomic_load(&n->sound)?"true":"false",atomic_load(&n->applied_volume),atomic_load(&n->pcm_peak),atomic_load(&n->audible_plays),atomic_load(&n->muted_events));
    if(count<0 || (size_t)count>=capacity)return ESP_ERR_INVALID_SIZE;
    *size=(size_t)count;return ESP_OK;
}
void app_notify_observe(app_notify_handle_t n,const cJSON *snapshot)
{
    if(!n)return;
    const cJSON *server=cJSON_GetObjectItemCaseSensitive(snapshot,"server_time");
    if(!cJSON_IsNumber(server) || server->valuedouble<1 || server->valuedouble>UINT32_MAX)return;
    const cJSON *events=cJSON_GetObjectItemCaseSensitive(snapshot,"completions");
    xSemaphoreTake(n->lock,portMAX_DELAY);
    bool first=!n->policy.armed;
    if(first){n->policy.armed=true;n->policy.baseline=(uint32_t)server->valuedouble;ESP_LOGI(TAG,"Completion baseline established; history suppressed");}
    const cJSON *item;unsigned count=0;
    cJSON_ArrayForEach(item,events){
        if(++count>8)break;
        const cJSON *key=cJSON_GetObjectItemCaseSensitive(item,"key"),*at=cJSON_GetObjectItemCaseSensitive(item,"at");
        if(!cJSON_IsString(key) || !cJSON_IsNumber(at) || at->valuedouble<1 || at->valuedouble>UINT32_MAX)continue;
        if(first){notification_policy_record(&n->policy,key->valuestring);continue;}
        if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snapshot,"ok")))continue;
        if(notification_policy_is_new(&n->policy,key->valuestring,(uint32_t)at->valuedouble)){
            uint8_t test=0;
            if(xQueueSend(n->queue,&test,0)==pdTRUE){notification_policy_record(&n->policy,key->valuestring);atomic_fetch_add(&n->queued,1);}
            else atomic_fetch_add(&n->dropped,1);
        }
    }
    xSemaphoreGive(n->lock);
}
esp_err_t app_notify_create(app_notify_handle_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    *out=NULL;
    struct app_notify *n=calloc(1,sizeof(*n));if(!n)return ESP_ERR_NO_MEM;
    n->audio_lock=xSemaphoreCreateMutex();
    if(!n->audio_lock){free(n);return ESP_ERR_NO_MEM;}
    atomic_init(&n->volume,50);atomic_init(&n->vibration,true);atomic_init(&n->sound,true);n->buffered_volume=101;
    n->queue=xQueueCreate(16,sizeof(uint8_t));n->lock=xSemaphoreCreateMutex();
    if(!n->queue || !n->lock){if(n->queue)vQueueDelete(n->queue);if(n->lock)vSemaphoreDelete(n->lock);free(n);return ESP_ERR_NO_MEM;}
    n->tone=heap_caps_malloc(TONE_FRAMES*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!n->tone){vQueueDelete(n->queue);vSemaphoreDelete(n->lock);free(n);return ESP_ERR_NO_MEM;}
    for(unsigned frame=0;frame<TONE_FRAMES;++frame){
        n->tone[frame]=(int16_t)((uint16_t)chime_pcm[2*frame]|((uint16_t)chime_pcm[2*frame+1]<<8));
        if(abs(n->tone[frame])>n->tone_peak)n->tone_peak=abs(n->tone[frame]);
    }
    esp_err_t err=esp_iris_rpc_register(0x70,4,self_test,n);
    if(err!=ESP_OK)goto fail;
    err=esp_iris_rpc_register(0x70,5,status,n);
    if(err!=ESP_OK){(void)esp_iris_rpc_unregister(0x70,4);goto fail;}
    if(xTaskCreate(worker,"pet_alert",8192,n,5,NULL)!=pdPASS){(void)esp_iris_rpc_unregister(0x70,4);(void)esp_iris_rpc_unregister(0x70,5);err=ESP_ERR_NO_MEM;goto fail;}
    *out=n;return ESP_OK;
fail:
    free(n->tone);vQueueDelete(n->queue);vSemaphoreDelete(n->lock);free(n);return err;
}

void app_notify_set_options(app_notify_handle_t n,unsigned volume,bool vibration,bool sound)
{
    if(!n || volume>100)return;
    atomic_store(&n->volume,volume);atomic_store(&n->vibration,vibration);atomic_store(&n->sound,sound);
}
esp_err_t app_notify_preview(app_notify_handle_t n)
{
    if(!n)return ESP_ERR_INVALID_ARG;
    uint8_t test=1;return xQueueSend(n->queue,&test,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}

bool app_notify_audio_acquire(app_notify_handle_t n){return n && xSemaphoreTake(n->audio_lock,pdMS_TO_TICKS(3000))==pdTRUE;}
void app_notify_audio_release(app_notify_handle_t n){if(n)xSemaphoreGive(n->audio_lock);}
