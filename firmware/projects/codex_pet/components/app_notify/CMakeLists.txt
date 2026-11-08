idf_component_register(SRCS "app_notify.c" "notification_policy.c" "audio_level.c" EMBED_FILES "chime.pcm" INCLUDE_DIRS "include"
    REQUIRES espressif__cjson PRIV_REQUIRES esp_timer heap esp-mosaico-bsp esp_iris freertos log)
