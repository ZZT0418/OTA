idf_component_register(SRCS "WS2812/led_ws2812.c" "iot/onenet_dm.c" "iot/onenet_token.c" "iot/wifi_manager.c" "iot/onenet_mqtt.c" "iot/onenet_ota.c" "main.c"
                    INCLUDE_DIRS "." "iot" "WS2812"
                    REQUIRES mqtt driver esp_wifi esp_http_client app_update esp_https_ota mbedtls json nvs_flash esp_event fatfs wear_levelling lwip)
