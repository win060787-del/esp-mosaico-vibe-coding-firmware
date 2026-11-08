// SPDX-License-Identifier: Apache-2.0
#include "app_display.h"
#include "app_battery.h"
#include "app_settings.h"
#include "app_pet.h"
#include "app_link.h"
#include "app_network.h"
#include "app_notify.h"
#include "app_voice.h"
#include "bsp/audio.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "codex_pet";

/* The composition root retains owned handles, never controller internals. */
static struct {
    esp_gsp_handle_t ui;
    app_pet_handle_t controller;
    app_link_handle_t link;
    app_notify_handle_t notify;
    app_voice_handle_t voice;
    app_battery_handle_t battery;
    app_settings_handle_t settings;
    app_network_handle_t network;
} application;

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_audio_init(NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_notify_create(&application.notify));
    ESP_ERROR_CHECK(app_link_create(application.notify,&application.link));
    ESP_ERROR_CHECK(app_pet_create(application.ui, app_link_poll, application.link, &application.controller));
    ESP_ERROR_CHECK(app_settings_create(application.notify,&application.settings));
    app_pet_set_controls(application.controller,app_settings_poll,app_settings_action,application.settings);
    ESP_ERROR_CHECK(app_network_start(application.link,&application.network));
    app_pet_set_management(application.controller,app_network_management_poll,app_network_management_action,application.network);
    app_pet_set_join(application.controller,app_network_join);
    if(app_voice_create(application.notify,&application.voice)==ESP_OK)
        app_pet_set_voice(application.controller,app_voice_poll,app_voice_action,application.voice);
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_battery_create(&application.battery));
    app_pet_set_battery_source(application.controller,app_battery_poll,application.battery);
    /* GSP retains the callback context. Device UI/controller live until reboot;
     * teardown requires quiescing GSP before app_pet_destroy(), then closing UI. */
    ESP_LOGI(TAG, "Codex Pet ready");
}
