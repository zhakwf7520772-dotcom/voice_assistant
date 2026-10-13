#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speaker.h"
#include "wav_pcm.h"
#include "driver/i2s_std.h"

static uint8_t wav[200044], captured[400000];
static size_t total, published, captured_size;
static unsigned starts, delays, delays_after_start, writes, opens, closes;
static bool advance_on_write, fail_before_start, fail_during_play;
static esp_err_t download_status;

static void put16(uint8_t *p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
static void put32(uint8_t *p, uint32_t value) { put16(p, value); put16(p + 2, value >> 16); }
static int16_t get16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }

static void fixture(unsigned channels)
{
    size_t samples = 80000; // 2.5 seconds mono, or 1.25 seconds stereo.
    total = 44 + samples * 2;
    memset(wav, 0, total);
    memcpy(wav, "RIFF", 4); put32(wav + 4, total - 8);
    memcpy(wav + 8, "WAVEfmt ", 8); put32(wav + 16, 16);
    put16(wav + 20, 1); put16(wav + 22, channels);
    put32(wav + 24, 32000); put32(wav + 28, 64000 * channels);
    put16(wav + 32, 2 * channels); put16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); put32(wav + 40, samples * 2);
    for (size_t i = 0; i < samples; ++i) put16(wav + 44 + i * 2, (uint16_t)(int16_t)((int)i % 60001 - 30000));
    published = 0; captured_size = 0;
    starts = delays = delays_after_start = writes = opens = closes = 0;
    advance_on_write = fail_before_start = fail_during_play = false;
    download_status = ESP_ERR_NOT_FINISHED;
}

static void publish(size_t bytes)
{
    published += bytes;
    if (published >= total) { published = total; download_status = ESP_OK; }
}
void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    assert(++delays < 10000);
    if (starts) ++delays_after_start;
    if (fail_before_start) { download_status = ESP_ERR_TIMEOUT; return; }
    publish(4096);
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "host-error"; }
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, void *rx)
{
    (void)rx;
    assert(config->id == I2S_NUM_1 && config->auto_clear_after_cb);
    ++opens; *tx = (void *)1; return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t tx, const i2s_std_config_t *config)
{
    assert(tx && config->clk_cfg == 32000);
    assert(config->gpio_cfg.bclk == 16 && config->gpio_cfg.ws == 15 && config->gpio_cfg.dout == 17);
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t tx) { assert(tx); return ESP_OK; }
esp_err_t i2s_channel_disable(i2s_chan_handle_t tx) { assert(tx); return ESP_OK; }
esp_err_t i2s_del_channel(i2s_chan_handle_t tx) { assert(tx); ++closes; return ESP_OK; }
esp_err_t i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t size, size_t *written, unsigned timeout)
{
    assert(tx && timeout == 2000 && size % 4 == 0);
    assert(captured_size + size <= sizeof(captured));
    memcpy(captured + captured_size, data, size); captured_size += size; *written = size;
    ++writes;
    if (advance_on_write) publish(512);
    if (fail_during_play && writes == 3) download_status = ESP_ERR_TIMEOUT;
    return ESP_OK;
}
static size_t available(void *ctx) { (void)ctx; return published; }
static esp_err_t status(void *ctx) { (void)ctx; return download_status; }
static void on_start(void *ctx) { (void)ctx; assert(published < total); ++starts; }

static void check_samples(unsigned channels)
{
    size_t frames = (total - 44) / (channels * 2);
    assert(captured_size == frames * 4);
    for (size_t frame = 0; frame < frames; ++frame) {
        for (size_t ch = 0; ch < 2; ++ch) {
            int32_t sample = get16(wav + 44 + (frame * channels + (channels == 1 ? 0 : ch)) * 2);
            assert(get16(captured + frame * 4 + ch * 2) == (int16_t)(sample * 15 / 100));
        }
    }
}

int main(void)
{
    wav_pcm_info_t info;
    fixture(1);
    // Every incomplete metadata prefix must be safe and request more bytes.
    for (size_t prefix = 0; prefix < 44; ++prefix) {
        uint8_t *short_buffer = malloc(prefix ? prefix : 1);
        memcpy(short_buffer, wav, prefix);
        assert(wav_pcm_parse(short_buffer, prefix, total, &info) == ESP_ERR_NOT_FINISHED);
        free(short_buffer);
    }
    assert(wav_pcm_parse(wav, 44, total, &info) == ESP_OK && info.data_bytes == total - 44);
    put32(wav + 40, (uint32_t)total);
    assert(wav_pcm_parse(wav, 44, total, &info) == ESP_ERR_INVALID_SIZE);
    fixture(1); put16(wav + 20, 3);
    assert(wav_pcm_parse(wav, 44, total, &info) == ESP_ERR_NOT_SUPPORTED);
    fixture(1); put32(wav + 40, 159999);
    assert(wav_pcm_parse(wav, 44, total, &info) == ESP_ERR_INVALID_RESPONSE);

    speaker_wav_source_t source = { .wav = wav, .available = available, .status = status, .on_start = on_start };
    fixture(1); source.length = total; advance_on_write = true;
    assert(speaker_play_wav_stream(&source) == ESP_OK && starts == 1 && opens == closes);
    check_samples(1);

    // Exhaust the first prebuffer, then refill; preserve all PCM samples.
    fixture(1); source.length = total;
    assert(speaker_play_wav_stream(&source) == ESP_OK && starts == 1 && delays_after_start > 1);
    check_samples(1);

    fixture(1); source.length = total; fail_before_start = true;
    assert(speaker_play_wav_stream(&source) == ESP_ERR_TIMEOUT && !opens && !starts);
    fixture(1); source.length = total; published = 100; download_status = ESP_OK;
    assert(speaker_play_wav_stream(&source) == ESP_ERR_INVALID_SIZE && !opens);
    fixture(1); source.length = total; fail_during_play = true;
    assert(speaker_play_wav_stream(&source) == ESP_ERR_TIMEOUT && opens == 1 && closes == 1 && writes == 3);

    fixture(2);
    assert(speaker_play_wav(wav, total) == ESP_OK && opens == closes);
    check_samples(2);
    puts("WAV metadata, streaming start, rebuffering, cancellation and stereo PCM tests passed");
    return 0;
}
