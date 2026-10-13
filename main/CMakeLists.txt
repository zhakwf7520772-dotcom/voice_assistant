idf_component_register(
    SRCS "main.c"
         "ir/receiver/ir_receiver.c"
         "ir/transmitter/ir_waveform.c"
         "ir/transmitter/ir_transmitter.c"
         "ir/gree/gree_raw_catalog.c"
         "ir/gree/gree_ac.c"
         "audio/microphone.c"
         "audio/speaker.c"
         "voice/voice.c"
         "cloud/wifi/wifi_sta.c"
         "cloud/asr/tencent_sign.c"
         "cloud/asr/asr_client.c"
         "cloud/llm/llm_client.c"
         "cloud/tts/tts_client.c"
    INCLUDE_DIRS "." "audio" "voice" "ir/receiver" "ir/transmitter" "ir/gree"
                 "cloud/wifi" "cloud/asr" "cloud/llm" "cloud/tts"
    PRIV_REQUIRES esp_driver_i2s esp_driver_rmt esp_driver_gpio esp_timer esp_wifi esp_event esp_netif
                  esp_http_client mbedtls nvs_flash esp_psram espressif__esp-sr espressif__cjson
)
