#include "ir_transmitter.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ir_waveform.h"
#include "microphone.h"

#define IR_TX_MAX_SYMBOLS 4096U

static const char *TAG = "IR_TX";
static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static rmt_symbol_word_t *s_waveform;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static bool s_enabled;
static bool s_initialized;
static bool s_faulted;

static esp_err_t cancel_transaction(void)
{
    // Keep the payload allocated and locked until cancellation has succeeded.
    esp_err_t err = rmt_disable(s_channel);
    if (err != ESP_OK) {
        s_faulted = true;
        return err;
    }
    s_enabled = false;
    err = rmt_tx_wait_all_done(s_channel, 0);
    if (err == ESP_OK) err = rmt_encoder_reset(s_encoder);
    if (err == ESP_OK) err = rmt_enable(s_channel);
    if (err == ESP_OK) s_enabled = true;
    else s_faulted = true;
    return err;
}

esp_err_t ir_transmitter_init(const ir_transmitter_config_t *config)
{
    if (!config || !GPIO_IS_VALID_OUTPUT_GPIO(config->gpio_num) ||
        config->gpio_num == GPIO_NUM_7 || config->gpio_num == MIC_GPIO_BCLK ||
        config->gpio_num == MIC_GPIO_WS || config->gpio_num == MIC_GPIO_DIN ||
        config->carrier_hz < 10000 || config->carrier_hz > 100000 ||
        !(config->duty_cycle > 0 && config->duty_cycle < 1)) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (!s_mutex) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_initialized || s_channel) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_faulted = false;
    s_waveform = heap_caps_calloc(IR_TX_MAX_SYMBOLS, sizeof(*s_waveform),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = s_waveform ? ESP_OK : ESP_ERR_NO_MEM;
    if (err != ESP_OK) goto fail;
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = config->gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 96,
        .trans_queue_depth = 1,
        .flags.invert_out = config->active_low,
        .flags.init_level = 0, // Logical idle, physically inverted if requested.
        .flags.with_dma = false,
    };
    err = rmt_new_tx_channel(&channel_config, &s_channel);
    if (err != ESP_OK) goto fail;
    const rmt_copy_encoder_config_t encoder_config = {};
    err = rmt_new_copy_encoder(&encoder_config, &s_encoder);
    if (err != ESP_OK) goto fail;
    const rmt_carrier_config_t carrier = {
        .frequency_hz = config->carrier_hz,
        .duty_cycle = config->duty_cycle,
        .flags.polarity_active_low = false, // Logical one enables carrier.
        .flags.always_on = false,
    };
    err = rmt_apply_carrier(s_channel, &carrier);
    if (err != ESP_OK) goto fail;
    err = rmt_enable(s_channel);
    if (err != ESP_OK) goto fail;
    s_enabled = s_initialized = true;
    ESP_LOGI(TAG, "Raw replay ready: GPIO%d, carrier=%" PRIu32 "Hz, active-%s",
             config->gpio_num, config->carrier_hz, config->active_low ? "low" : "high");
    xSemaphoreGive(s_mutex);
    return ESP_OK;

fail:
    if (s_encoder) { rmt_del_encoder(s_encoder); s_encoder = NULL; }
    if (s_channel) { rmt_del_channel(s_channel); s_channel = NULL; }
    heap_caps_free(s_waveform);
    s_waveform = NULL;
    ESP_LOGE(TAG, "TX initialization failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t ir_transmitter_replay(const ir_raw_command_t *command)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_initialized || s_faulted) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    size_t count = 0;
    uint64_t duration_us = 0;
    esp_err_t err = ir_waveform_prepare(command, s_waveform, IR_TX_MAX_SYMBOLS, &count, &duration_us);
    if (err == ESP_OK) {
        const rmt_transmit_config_t config = { .loop_count = 0, .flags.eot_level = 0 };
        err = rmt_transmit(s_channel, s_encoder, s_waveform, count * sizeof(*s_waveform), &config);
        if (err == ESP_OK) {
            // Bounded by 4096 symbols * 65534 us; comfortably fits int timeout.
            int timeout_ms = (int)(duration_us / 1000) + 1000;
            err = rmt_tx_wait_all_done(s_channel, timeout_ms);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Replay failed: %s; cancelling before buffer reuse", esp_err_to_name(err));
            esp_err_t cancelled = cancel_transaction();
            if (cancelled != ESP_OK) ESP_LOGE(TAG, "Cancellation/recovery failed: %s", esp_err_to_name(cancelled));
        } else {
            ESP_LOGI(TAG, "Raw replay complete: %u symbols, %" PRIu64 "us including captured gaps",
                     (unsigned)count, duration_us);
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t ir_transmitter_deinit(void)
{
    if (!s_mutex) return ESP_OK;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = s_enabled ? rmt_disable(s_channel) : ESP_OK;
    if (err == ESP_OK) s_enabled = false;
    if (err == ESP_OK && s_channel) {
        err = rmt_del_channel(s_channel);
        if (err == ESP_OK) s_channel = NULL;
    }
    if (err == ESP_OK && s_encoder) {
        err = rmt_del_encoder(s_encoder);
        if (err == ESP_OK) s_encoder = NULL;
    }
    if (err == ESP_OK) {
        heap_caps_free(s_waveform);
        s_waveform = NULL;
        s_initialized = s_faulted = false;
    } else {
        s_faulted = true; // Retain resources if stopping the hardware failed.
    }
    xSemaphoreGive(s_mutex);
    return err;
}
