#include "app_battery.h"
#include "bsp/esp_mosaico.h"
#include "driver/i2c_master.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define SOC_REGISTER 0x2c
#define VOLTAGE_REGISTER 0x08
struct app_battery {
    i2c_master_dev_handle_t gauge;
    atomic_int percentage;
    atomic_int voltage_mv;
    atomic_int last_error;
};
static esp_err_t read_word(struct app_battery *b,uint8_t reg,unsigned *value)
{
    uint8_t bytes[2];
    esp_err_t err=i2c_master_transmit_receive(b->gauge,&reg,1,bytes,2,100);
    if(err==ESP_OK)*value=(unsigned)bytes[0]|((unsigned)bytes[1]<<8);
    return err;
}
static void sample(struct app_battery *b)
{
    unsigned soc=0,voltage=0;
    esp_err_t err=read_word(b,SOC_REGISTER,&soc);
    if(err==ESP_OK)err=read_word(b,VOLTAGE_REGISTER,&voltage);
    if(err==ESP_OK && (soc>100 || voltage<2000 || voltage>5000))err=ESP_ERR_INVALID_RESPONSE;
    atomic_store(&b->last_error,err);
    atomic_store(&b->voltage_mv,err==ESP_OK?(int)voltage:-1);
    atomic_store(&b->percentage,err==ESP_OK?(int)soc:-1);
}
static void worker(void *context)
{
    struct app_battery *b=context;
    for(;;){sample(b);vTaskDelay(pdMS_TO_TICKS(30000));}
}
int app_battery_poll(void *context)
{
    struct app_battery *b=context;
    return b?atomic_load(&b->percentage):-1;
}
static esp_err_t status(const esp_iris_rpc_request_t *request,uint8_t *response,
                        size_t capacity,size_t *size,void *context)
{
    (void)request;
    struct app_battery *b=context;
    int n=snprintf((char *)response,capacity,"{\"percentage\":%d,\"voltage_mv\":%d,\"error\":%d}",
        atomic_load(&b->percentage),atomic_load(&b->voltage_mv),atomic_load(&b->last_error));
    if(n<0 || (size_t)n>=capacity)return ESP_ERR_INVALID_SIZE;
    *size=(size_t)n;return ESP_OK;
}
esp_err_t app_battery_create(app_battery_handle_t *out)
{
    if(!out)return ESP_ERR_INVALID_ARG;
    *out=NULL;
    struct app_battery *b=calloc(1,sizeof(*b));
    if(!b)return ESP_ERR_NO_MEM;
    atomic_init(&b->percentage,-1);atomic_init(&b->voltage_mv,-1);atomic_init(&b->last_error,ESP_ERR_NOT_FINISHED);
    esp_err_t err=bsp_i2c_init();
    if(err!=ESP_OK){free(b);return err;}
    const i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=BSP_BATTERY_I2C_ADDR,.scl_speed_hz=100000};
    err=i2c_master_bus_add_device(bsp_i2c_get_handle(),&config,&b->gauge);
    if(err!=ESP_OK){free(b);return err;}
    /* A read-only register access, without BSP battery_init's profile writes. */
    sample(b);
    if(xTaskCreate(worker,"pet_battery",3072,b,2,NULL)!=pdPASS){
        i2c_master_bus_rm_device(b->gauge);free(b);return ESP_ERR_NO_MEM;
    }
    /* RPC failure does not invalidate the lifetime-owned sampler. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_rpc_register(0x70,6,status,b));
    *out=b;return ESP_OK;
}
