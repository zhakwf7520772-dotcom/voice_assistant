#include "microphone.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

static i2s_chan_handle_t s_rx_channel;

esp_err_t microphone_init(void)
{
    if (s_rx_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = MIC_FRAME_SAMPLES;

    esp_err_t err = i2s_new_channel(&channel_config, NULL, &s_rx_channel);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_GPIO_BCLK,
            .ws = MIC_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // On ESP32-S3 the default Philips macro selects BOTH slots even in mono mode.
    standard_config.slot_cfg.slot_mask = MIC_CHANNEL_LEFT ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;

    err = i2s_channel_init_std_mode(s_rx_channel, &standard_config);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx_channel);
    }
    if (err != ESP_OK) {
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    return err;
}

esp_err_t microphone_read(int32_t *samples, size_t capacity, size_t *samples_read)
{
    if (samples == NULL || samples_read == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_read = 0;
    if (s_rx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_channel, samples,
                                     capacity * sizeof(*samples), &bytes_read,
                                     MIC_READ_TIMEOUT_MS);
    if (err == ESP_OK) {
        if (bytes_read % sizeof(*samples) != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        *samples_read = bytes_read / sizeof(*samples);
    }
    return err;
}

esp_err_t microphone_deinit(void)
{
    if (s_rx_channel == NULL) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_disable(s_rx_channel);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_del_channel(s_rx_channel);
    if (err == ESP_OK) {
        s_rx_channel = NULL;
    }
    return err;
}
