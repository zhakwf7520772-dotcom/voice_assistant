#include "esp_log.h"
#include "esp_psram.h"
#include "microphone.h"
#include "voice.h"
#include "wifi_sta.h"
#include "ir_receiver.h"

static const char *TAG = "voice_main";

void app_main(void)
{
    ESP_LOGI(TAG, "I2S %d Hz; GPIO BCLK=%d WS=%d DIN=%d; %s channel",
             MIC_SAMPLE_RATE_HZ, MIC_GPIO_BCLK, MIC_GPIO_WS, MIC_GPIO_DIN,
             MIC_CHANNEL_LEFT ? "left" : "right");
    ESP_LOGI(TAG, "PSRAM available: %u bytes", (unsigned)esp_psram_get_size());
    ESP_LOGI(TAG, "Starting Wi-Fi connection at boot");
    esp_err_t err = wifi_sta_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
    err = ir_receiver_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "IR startup failed: %s", esp_err_to_name(err));
    err = voice_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "Voice startup failed: %s", esp_err_to_name(err));
}
