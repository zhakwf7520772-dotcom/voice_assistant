idf_component_register(
    SRCS "main.c"
         "audio/microphone.c"
         "voice/voice.c"
         "cloud/wifi/wifi_sta.c"
         "cloud/asr/tencent_sign.c"
         "cloud/asr/asr_client.c"
         "cloud/llm/llm_client.c"
    INCLUDE_DIRS "." "audio" "voice" "cloud/wifi" "cloud/asr" "cloud/llm"
    PRIV_REQUIRES esp_driver_i2s esp_timer esp_wifi esp_event esp_netif
                  esp_http_client mbedtls nvs_flash esp_psram espressif__esp-sr espressif__cjson
)
