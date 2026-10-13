#include "speaker.h"
#include "wav_pcm.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "speaker";

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static size_t source_available(const speaker_wav_source_t *source)
{
    size_t available = source->available ? source->available(source->context) : source->length;
    return available > source->length ? source->length : available;
}

static esp_err_t source_status(const speaker_wav_source_t *source)
{
    return source->status ? source->status(source->context) : ESP_OK;
}

static esp_err_t wait_for_bytes(const speaker_wav_source_t *source, size_t wanted)
{
    while (true) {
        esp_err_t status = source_status(source);
        if (status != ESP_OK && status != ESP_ERR_NOT_FINISHED) return status;
        if (source_available(source) >= wanted) return ESP_OK;
        if (status == ESP_OK) return ESP_ERR_INVALID_SIZE;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t speaker_play_wav_stream(const speaker_wav_source_t *source)
{
    if (!source || !source->wav) return ESP_ERR_INVALID_ARG;
    wav_pcm_info_t info;
    esp_err_t err;
    while (true) {
        esp_err_t status = source_status(source);
        if (status != ESP_OK && status != ESP_ERR_NOT_FINISHED) return status;
        err = wav_pcm_parse(source->wav, source_available(source), source->length, &info);
        if (err != ESP_ERR_NOT_FINISHED) break;
        if (status == ESP_OK) return ESP_ERR_INVALID_RESPONSE;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) return err;
    size_t prebuffer = (uint64_t)info.sample_rate * info.channels * sizeof(int16_t) *
                      CONFIG_TTS_PLAYBACK_BUFFER_MS / 1000;
    if (prebuffer > info.data_bytes) prebuffer = info.data_bytes;
    err = wait_for_bytes(source, info.data_offset + prebuffer);
    if (err != ESP_OK) return err;

    const uint8_t *pcm = source->wav + info.data_offset;
    size_t pcm_length = info.data_bytes;
    uint16_t channels = info.channels;
    uint32_t sample_rate = info.sample_rate;
    ESP_LOGI(TAG, "WAV: %lu Hz, %u channel(s), %u PCM bytes; volume %d%%; buffer %d ms",
             (unsigned long)sample_rate, (unsigned)channels, (unsigned)pcm_length,
             CONFIG_SPEAKER_VOLUME_PERCENT, CONFIG_TTS_PLAYBACK_BUFFER_MS);
    ESP_LOGI(TAG, "I2S1 TX: BCLK=GPIO%d, WS=GPIO%d, DOUT=GPIO%d; amplifier SD must be high",
             SPEAKER_GPIO_BCLK, SPEAKER_GPIO_WS, SPEAKER_GPIO_DOUT);
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 256;
    channel.auto_clear_after_cb = true;
    err = i2s_new_channel(&channel, &tx, NULL);
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

    uint8_t block[1024];
    size_t pos = 0, sample_count = 0;
    uint32_t peak = 0;
    uint64_t magnitude_sum = 0;
    unsigned underruns = 0;
    bool began = false;
    while (pos < pcm_length) {
        esp_err_t status = source_status(source);
        if (status != ESP_OK && status != ESP_ERR_NOT_FINISHED) { err = status; break; }
        size_t available = source_available(source);
        available = available > info.data_offset ? available - info.data_offset : 0;
        if (available > pcm_length) available = pcm_length;
        size_t frames = available > pos ? (available - pos) / (channels * sizeof(int16_t)) : 0;
        if (!frames) {
            ++underruns;
            ESP_LOGW(TAG, "Audio buffer empty; rebuffering (count=%u)", underruns);
            size_t wanted = pcm_length - pos;
            if (wanted > prebuffer) wanted = prebuffer;
            err = wait_for_bytes(source, info.data_offset + pos + wanted);
            if (err != ESP_OK) break;
            continue;
        }
        if (frames > sizeof(block) / (2 * sizeof(int16_t)))
            frames = sizeof(block) / (2 * sizeof(int16_t));
        size_t bytes = frames * 2 * sizeof(int16_t);
        for (size_t i = 0; i < frames; ++i) {
            for (size_t ch = 0; ch < 2; ++ch) {
                size_t offset = pos + (i * channels + (channels == 1 ? 0 : ch)) * sizeof(int16_t);
                int16_t sample = (int16_t)le16(pcm + offset);
                if (channels == 2 || ch == 0) {
                    uint32_t magnitude = sample < 0 ? (uint32_t)-(int32_t)sample : (uint32_t)sample;
                    if (magnitude > peak) peak = magnitude;
                    magnitude_sum += magnitude;
                    ++sample_count;
                }
                uint16_t scaled = (uint16_t)(int16_t)((int32_t)sample * CONFIG_SPEAKER_VOLUME_PERCENT / 100);
                size_t target = (i * 2 + ch) * sizeof(int16_t);
                block[target] = (uint8_t)scaled;
                block[target + 1] = (uint8_t)(scaled >> 8);
            }
        }
        size_t written = 0;
        err = i2s_channel_write(tx, block, bytes, &written, 2000);
        if (err != ESP_OK || written != bytes) {
            if (err == ESP_OK) err = ESP_ERR_TIMEOUT;
            break;
        }
        if (!began) {
            began = true;
            if (source->on_start) source->on_start(source->context);
        }
        pos += frames * channels * sizeof(int16_t);
    }
    ESP_LOGI(TAG, "PCM peak=%lu, mean absolute=%lu; output peak=%lu; rebuffer count=%u",
             (unsigned long)peak, (unsigned long)(sample_count ? magnitude_sum / sample_count : 0),
             (unsigned long)(peak * CONFIG_SPEAKER_VOLUME_PERCENT / 100), underruns);
    if (!peak) ESP_LOGW(TAG, "WAV contains only silence");
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    i2s_channel_disable(tx);
cleanup:
    i2s_del_channel(tx);
    return err;
}

esp_err_t speaker_play_wav(const uint8_t *wav, size_t length)
{
    speaker_wav_source_t source = { .wav = wav, .length = length };
    return speaker_play_wav_stream(&source);
}

static void store_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void store_le32(uint8_t *p, uint32_t value)
{
    store_le16(p, (uint16_t)value);
    store_le16(p + 2, (uint16_t)(value >> 16));
}

esp_err_t speaker_play_test_tones(void)
{
    const uint32_t rate = 32000;
    const uint32_t pcm_bytes = rate * sizeof(int16_t);
    uint8_t *wav = heap_caps_calloc(1, 44 + pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) return ESP_ERR_NO_MEM;
    memcpy(wav, "RIFF", 4);
    store_le32(wav + 4, 36 + pcm_bytes);
    memcpy(wav + 8, "WAVEfmt ", 8);
    store_le32(wav + 16, 16);
    store_le16(wav + 20, 1);
    store_le16(wav + 22, 1);
    store_le32(wav + 24, rate);
    store_le32(wav + 28, pcm_bytes);
    store_le16(wav + 32, 2);
    store_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    store_le32(wav + 40, pcm_bytes);
    for (uint32_t i = 0; i < rate; ++i) {
        // 800 Hz triangle wave: two 350 ms tones with a 150 ms gap.
        if (i >= 11200 && (i < 16000 || i >= 27200)) continue;
        int phase = i % 40;
        int32_t sample = phase < 10 ? phase * 8192 / 10 :
                         phase < 30 ? (20 - phase) * 8192 / 10 :
                                      (phase - 40) * 8192 / 10;
        store_le16(wav + 44 + i * sizeof(int16_t), (uint16_t)(int16_t)sample);
    }
    ESP_LOGI(TAG, "Local speaker test: two 800 Hz tones (no network needed)");
    esp_err_t err = speaker_play_wav(wav, 44 + pcm_bytes);
    free(wav);
    ESP_LOGI(TAG, "Local speaker test finished: %s", esp_err_to_name(err));
    return err;
}
