idf_component_register(SRCS "app_network.c" "network_portal.c" "wifi_profiles.c" INCLUDE_DIRS "include"
    REQUIRES app_link PRIV_REQUIRES espressif__mdns esp_wifi esp_netif esp_event esp_http_client
        esp_http_server heap nvs_flash esp_iris espressif__cjson esp_timer log)
target_add_binary_data(${COMPONENT_LIB} "server.crt" TEXT)
