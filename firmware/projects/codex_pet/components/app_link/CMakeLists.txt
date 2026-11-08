idf_component_register(SRCS "app_link.c" INCLUDE_DIRS "include"
    REQUIRES app_pet app_notify esp_iris PRIV_REQUIRES freertos espressif__cjson)
