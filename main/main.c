#include "esp_log.h"
#include "esp_psram.h"
#include "microphone.h"
#include "voice.h"

static const char *TAG = "voice_main";

void app_main(void)
{
    ESP_LOGI(TAG, "I2S %d Hz; GPIO BCLK=%d WS=%d DIN=%d; %s channel",
             MIC_SAMPLE_RATE_HZ, MIC_GPIO_BCLK, MIC_GPIO_WS, MIC_GPIO_DIN,
             MIC_CHANNEL_LEFT ? "left" : "right");
    ESP_LOGI(TAG, "PSRAM available: %u bytes", (unsigned)esp_psram_get_size());
    esp_err_t err = voice_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "Voice startup failed: %s", esp_err_to_name(err));
}
