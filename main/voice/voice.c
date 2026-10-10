#include "voice.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "microphone.h"
#include "model_path.h"
#include "asr_client.h"
#include "llm_client.h"

#define PRE_SAMPLES (MIC_SAMPLE_RATE_HZ / 2)
#define MAX_SAMPLES (MIC_SAMPLE_RATE_HZ * 15)
#define NO_SPEECH_US 5000000LL
#define WAKE_COOLDOWN_US 350000LL
#define END_SILENCE_SAMPLES (MIC_SAMPLE_RATE_HZ * CONFIG_VOICE_END_SILENCE_MS / 1000)

static const char *TAG = "voice";
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static const char *s_wake_words;
static int16_t *s_record;
static int16_t *s_preroll;
static size_t s_pre_pos, s_pre_count, s_record_count;
static QueueHandle_t s_asr_requests, s_asr_results;
static QueueHandle_t s_llm_requests, s_llm_results;

typedef enum { WAIT_WAKE, WAKE_DETECTED, RECORDING, ASR_PROCESSING, LLM_PROCESSING } voice_state_t;
static voice_state_t s_state = WAIT_WAKE;

static void pre_add(const int16_t *data, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        s_preroll[s_pre_pos] = data[i];
        s_pre_pos = (s_pre_pos + 1) % PRE_SAMPLES;
        if (s_pre_count < PRE_SAMPLES) ++s_pre_count;
    }
}

static bool record_add(const int16_t *data, size_t count)
{
    if (count > MAX_SAMPLES - s_record_count) return false;
    memcpy(s_record + s_record_count, data, count * sizeof(int16_t));
    s_record_count += count;
    return true;
}

static void resume_wake(void)
{
    s_record_count = 0;
    s_pre_pos = 0;
    s_pre_count = 0;
    s_afe->reset_vad(s_afe_data);
    s_afe->enable_wakenet(s_afe_data);
    s_state = WAIT_WAKE;
    ESP_LOGI(TAG, "[SYS] free internal=%u PSRAM=%u; voice task stack=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "[WAKE] Waiting for wake word...");
}

static void finish_record(void)
{
    ESP_LOGI(TAG, "[VAD] Speech ended");
    ESP_LOGI(TAG, "[VOICE] duration: %.2fs | PCM size: %u bytes",
             (double)s_record_count / MIC_SAMPLE_RATE_HZ, (unsigned)(s_record_count * 2));
    if (!s_record_count) {
        resume_wake();
        return;
    }
    asr_request_t request = { .pcm = s_record, .samples = s_record_count };
    if (xQueueSend(s_asr_requests, &request, 0) != pdTRUE) {
        ESP_LOGE(TAG, "ASR request queue busy");
        resume_wake();
        return;
    }
    s_state = ASR_PROCESSING;
    ESP_LOGI(TAG, "[ASR] recognizing...");
}

static void feed_task(void *arg)
{
    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *pcm = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int32_t *raw = heap_caps_malloc(MIC_FRAME_SAMPLES * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm || !raw) {
        ESP_LOGE(TAG, "feed buffers allocation failed");
        abort();
    }
    size_t filled = 0;
    while (true) {
        size_t count = 0;
        esp_err_t err = microphone_read(raw, MIC_FRAME_SAMPLES, &count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "microphone read: %s", esp_err_to_name(err));
            filled = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (size_t i = 0; i < count; ++i) {
            pcm[filled++] = (int16_t)(raw[i] >> 16); // signed 24-bit MSB to signed 16-bit
            if (filled == (size_t)chunk) {
                if (s_afe->feed(s_afe_data, pcm) <= 0) ESP_LOGW(TAG, "AFE feed failed/overflow");
                filled = 0;
            }
        }
    }
}

static void fetch_task(void *arg)
{
    int64_t wake_at = 0;
    int64_t last_buffer_log = 0;
    size_t silence_samples = 0;
    ESP_LOGI(TAG, "[SYS] Voice Assistant Ready");
    ESP_LOGI(TAG, "[WAKE] Waiting for wake word...");
    while (true) {
        if (s_state == ASR_PROCESSING) {
            asr_result_t result;
            if (xQueueReceive(s_asr_results, &result, 0) == pdTRUE) {
                if (result.err == ESP_OK && result.text[0]) {
                    ESP_LOGI(TAG, "[ASR] result: %s", result.text);
                    llm_request_t request = { 0 };
                    size_t length = strlen(result.text);
                    if (length < sizeof(request.text)) {
                        memcpy(request.text, result.text, length + 1);
                        if (xQueueSend(s_llm_requests, &request, 0) == pdTRUE) {
                            s_state = LLM_PROCESSING;
                            ESP_LOGI(TAG, "[LLM] requesting...");
                        } else {
                            ESP_LOGE(TAG, "[LLM] request queue full");
                            resume_wake();
                        }
                    } else {
                        ESP_LOGE(TAG, "[LLM] ASR text exceeds request limit");
                        resume_wake();
                    }
                } else {
                    ESP_LOGE(TAG, "[ASR] failed: %s", esp_err_to_name(result.err));
                    resume_wake();
                }
                ESP_LOGI(TAG, "[ASR] elapsed: %lld ms", (long long)result.elapsed_ms);
            }
        } else if (s_state == LLM_PROCESSING) {
            llm_result_t result;
            if (xQueueReceive(s_llm_results, &result, 0) == pdTRUE) {
                if (result.err == ESP_OK) ESP_LOGI(TAG, "[LLM] response: %s", result.reply);
                else ESP_LOGE(TAG, "[LLM] error: %s (HTTP %d)", esp_err_to_name(result.err), result.http_status);
                ESP_LOGI(TAG, "[LLM] elapsed: %lld ms", (long long)result.elapsed_ms);
                resume_wake();
            }
        }
        afe_fetch_result_t *r = s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(1000));
        if (!r || r->ret_value != ESP_OK || !r->data || r->data_size < 0 || r->data_size % 2) {
            ESP_LOGW(TAG, "AFE fetch timeout/error");
            continue;
        }
        size_t samples = r->data_size / sizeof(int16_t);
        int64_t now = esp_timer_get_time();
        if (r->ringbuff_free_pct > 0.7f && now - last_buffer_log >= 5000000) {
            ESP_LOGI(TAG, "AFE ringbuff_free_pct: %.0f%% (verify feed failures if audio stalls)",
                     r->ringbuff_free_pct * 100);
            last_buffer_log = now;
        }
        if (s_state == WAIT_WAKE) {
            if (r->wakeup_state == WAKENET_DETECTED && r->wakenet_model_index == 1 && r->wake_word_index == 1) {
                ESP_LOGI(TAG, "[WAKE] detected: %s", s_wake_words);
                s_afe->disable_wakenet(s_afe_data);
                s_afe->reset_vad(s_afe_data);
                s_pre_count = s_pre_pos = s_record_count = 0;
                wake_at = esp_timer_get_time();
                s_state = WAKE_DETECTED;
                ESP_LOGI(TAG, "[VOICE] listening...");
            }
        } else if (s_state == WAKE_DETECTED) {
            if (esp_timer_get_time() - wake_at < WAKE_COOLDOWN_US) {
                s_afe->reset_vad(s_afe_data);
                continue;
            }
            if (r->vad_state == VAD_SPEECH) {
                s_record_count = 0;
                size_t start = (s_pre_pos + PRE_SAMPLES - s_pre_count) % PRE_SAMPLES;
                for (size_t i = 0; i < s_pre_count; ++i) s_record[s_record_count++] = s_preroll[(start + i) % PRE_SAMPLES];
                // The 500 ms processed-audio preroll already covers VAD's earlier frames;
                // appending vad_cache as well would duplicate the same audio.
                if (!record_add(r->data, samples)) { resume_wake(); continue; }
                silence_samples = 0;
                s_state = RECORDING;
                ESP_LOGI(TAG, "[VAD] Speech started");
            } else {
                pre_add(r->data, samples);
                if (esp_timer_get_time() - wake_at >= NO_SPEECH_US) {
                    ESP_LOGW(TAG, "no speech after wake; timeout");
                    resume_wake();
                }
            }
        } else if (s_state == RECORDING) {
            if (!record_add(r->data, samples)) { finish_record(); continue; }
            silence_samples = (r->vad_state == VAD_SPEECH) ? 0 : silence_samples + samples;
            if (silence_samples >= END_SILENCE_SAMPLES || s_record_count >= MAX_SAMPLES) finish_record();
        }
    }
}

esp_err_t voice_start(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models) return ESP_FAIL;
    char *name = esp_srmodel_filter(s_models, "wn9_hilexin", NULL);
    if (!name) {
        ESP_LOGE(TAG, "WakeNet model wn9_hilexin missing: check model partition/flash");
        esp_srmodel_deinit(s_models);
        return ESP_ERR_NOT_FOUND;
    }
    s_wake_words = esp_srmodel_get_wake_words(s_models, name);
    if (!s_wake_words || !s_wake_words[0]) {
        free((void *)s_wake_words);
        esp_srmodel_deinit(s_models);
        return ESP_ERR_NOT_FOUND;
    }
    afe_config_t *config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!config) {
        free((void *)s_wake_words);
        esp_srmodel_deinit(s_models);
        return ESP_ERR_NO_MEM;
    }
    config->wakenet_init = true;
    config->wakenet_model_name = name;
    config->vad_init = true;
    config->vad_min_speech_ms = 160;
    config->vad_min_noise_ms = 200;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_afe = esp_afe_handle_from_config(config);
    s_afe_data = s_afe ? s_afe->create_from_config(config) : NULL;
    afe_config_free(config);
    esp_err_t err = ESP_OK;
    if (!s_afe_data) { err = ESP_ERR_NO_MEM; goto fail; }
    if (s_afe->get_feed_channel_num(s_afe_data) != 1 || s_afe->get_samp_rate(s_afe_data) != MIC_SAMPLE_RATE_HZ) {
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    s_record = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_preroll = heap_caps_malloc(PRE_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_asr_requests = xQueueCreate(1, sizeof(asr_request_t));
    s_asr_results = xQueueCreate(1, sizeof(asr_result_t));
    s_llm_requests = xQueueCreate(1, sizeof(llm_request_t));
    s_llm_results = xQueueCreate(1, sizeof(llm_result_t));
    if (!s_record || !s_preroll || !s_asr_requests || !s_asr_results ||
        !s_llm_requests || !s_llm_results) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = microphone_init();
    if (err != ESP_OK) goto fail;
    asr_set_result_queue(s_asr_results);
    llm_set_result_queue(s_llm_results);
    // Task creation failure after other tasks start cannot safely be rolled back here.
    if (xTaskCreatePinnedToCore(asr_task, "asr_http", 12288, s_asr_requests, 4, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(llm_task, "llm_http", 12288, s_llm_requests, 4, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(fetch_task, "voice_fetch", 8192, NULL, 6, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(feed_task, "mic_feed", 4096, NULL, 8, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        abort();
    }
    return ESP_OK;
fail:
    microphone_deinit();
    if (s_asr_requests) vQueueDelete(s_asr_requests);
    if (s_asr_results) vQueueDelete(s_asr_results);
    if (s_llm_requests) vQueueDelete(s_llm_requests);
    if (s_llm_results) vQueueDelete(s_llm_results);
    free(s_record);
    free(s_preroll);
    if (s_afe_data) s_afe->destroy(s_afe_data);
    free((void *)s_wake_words);
    esp_srmodel_deinit(s_models);
    return err;
}
