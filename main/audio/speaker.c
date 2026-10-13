#include "speaker.h"

#include <stdbool.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "speaker";

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

esp_err_t speaker_play_wav(const uint8_t *wav, size_t length)
{
    if (!wav || length < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4))
        return ESP_ERR_INVALID_RESPONSE;
    uint32_t riff_size = le32(wav + 4);
    if (riff_size < 36 || (size_t)riff_size > length - 8) return ESP_ERR_INVALID_SIZE;
    size_t end = (size_t)riff_size + 8;
    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_length = 0;
    bool format_found = false;
    for (size_t offset = 12; offset + 8 <= end;) {
        uint32_t chunk_size = le32(wav + offset + 4);
        size_t start = offset + 8;
        if (chunk_size > end - start) return ESP_ERR_INVALID_SIZE;
        if (memcmp(wav + offset, "fmt ", 4) == 0) {
            if (chunk_size < 16) return ESP_ERR_INVALID_RESPONSE;
            const uint8_t *fmt = wav + start;
            if (le16(fmt) != 1) return ESP_ERR_NOT_SUPPORTED;
            channels = le16(fmt + 2);
            sample_rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (channels < 1 || channels > 2 || bits != 16 ||
                sample_rate < 8000 || sample_rate > 48000 ||
                le16(fmt + 12) != channels * sizeof(int16_t) ||
                le32(fmt + 8) != sample_rate * channels * sizeof(int16_t))
                return ESP_ERR_NOT_SUPPORTED;
            format_found = true;
        } else if (memcmp(wav + offset, "data", 4) == 0 && !pcm) {
            pcm = wav + start;
            pcm_length = chunk_size;
        }
        if ((size_t)chunk_size + (chunk_size & 1) > end - start) {
            // A final odd-sized data chunk may omit its padding byte.
            if (!(start + chunk_size == end && (chunk_size & 1))) return ESP_ERR_INVALID_SIZE;
            offset = end;
        } else {
            offset = start + chunk_size + (chunk_size & 1);
        }
    }
    if (!format_found || !pcm || !pcm_length || pcm_length % (channels * sizeof(int16_t)))
        return ESP_ERR_INVALID_RESPONSE;

    ESP_LOGI(TAG, "WAV: %lu Hz, %u channel(s), %u PCM bytes; volume %d%%", (unsigned long)sample_rate,
             (unsigned)channels, (unsigned)pcm_length, CONFIG_SPEAKER_VOLUME_PERCENT);
    i2s_chan_handle_t tx = NULL;
    // Microphone RX uses the first controller; keep TX on the other controller.
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&channel, &tx, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_GPIO_BCLK,
            .ws = SPEAKER_GPIO_WS,
            .dout = SPEAKER_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(tx, &std);
    if (err != ESP_OK) goto cleanup;
    err = i2s_channel_enable(tx);
    if (err != ESP_OK) goto cleanup;

    // Always write stereo: attenuate each PCM sample and duplicate mono to both slots.
    uint8_t block[1024];
    size_t pos = 0;
    while (pos < pcm_length) {
        size_t frames = (pcm_length - pos) / (channels * sizeof(int16_t));
        if (frames > sizeof(block) / (2 * sizeof(int16_t)))
            frames = sizeof(block) / (2 * sizeof(int16_t));
        size_t bytes = frames * 2 * sizeof(int16_t);
        for (size_t i = 0; i < frames; ++i) {
            for (size_t ch = 0; ch < 2; ++ch) {
                size_t source = pos + (i * channels + (channels == 1 ? 0 : ch)) * sizeof(int16_t);
                int16_t sample = (int16_t)le16(pcm + source);
                uint16_t scaled = (uint16_t)(int16_t)((int32_t)sample * CONFIG_SPEAKER_VOLUME_PERCENT / 100);
                size_t target = (i * 2 + ch) * sizeof(int16_t);
                block[target] = (uint8_t)scaled;
                block[target + 1] = (uint8_t)(scaled >> 8);
            }
        }
        size_t written = 0;
        err = i2s_channel_write(tx, block, bytes, &written, pdMS_TO_TICKS(2000));
        if (err != ESP_OK || written != bytes) {
            if (err == ESP_OK) err = ESP_ERR_TIMEOUT;
            break;
        }
        pos += frames * channels * sizeof(int16_t);
    }
    // Let the final DMA frames finish before tearing down the channel.
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    i2s_channel_disable(tx);
cleanup:
    i2s_del_channel(tx);
    return err;
}
