idf_component_register(SRCS "main.c" "app_battery.c" "app_settings.c"
    PRIV_REQUIRES esp_app_format esp-mosaico-bsp esp_driver_i2c freertos app_display app_pet app_link app_network app_notify app_voice esp_iris esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris nvs_flash log)
