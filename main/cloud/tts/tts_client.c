#include "tts_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "speaker.h"
#include "wifi_sta.h"

#define TTS_MAX_WAV_BYTES (3 * 1024 * 1024)
#define TTS_URL_CAPACITY 512

static const char *TAG = "tts";
static QueueHandle_t s_results;

void tts_set_result_queue(QueueHandle_t results)
{
    s_results = results;
}

static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t size)
{
    while (size) {
        int n = esp_http_client_write(client, data, size);
        if (n <= 0) return ESP_FAIL;
        data += n;
        size -= n;
    }
    return ESP_OK;
}

static esp_err_t fetch_wav(esp_http_client_handle_t client, uint8_t **wav, size_t *wav_size)
{
    int64_t expected = esp_http_client_get_content_length(client);
    if (expected > TTS_MAX_WAV_BYTES) return ESP_ERR_INVALID_SIZE;
    size_t capacity = expected > 0 ? (size_t)expected : 65536;
    uint8_t *buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    size_t used = 0;
    esp_err_t err = ESP_OK;
    while (true) {
        if (used == capacity) {
            if (esp_http_client_is_complete_data_received(client)) break;
            if (capacity == TTS_MAX_WAV_BYTES) { err = ESP_ERR_INVALID_SIZE; break; }
            size_t next = capacity > TTS_MAX_WAV_BYTES / 2 ? TTS_MAX_WAV_BYTES : capacity * 2;
            uint8_t *grown = heap_caps_realloc(buffer, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { err = ESP_ERR_NO_MEM; break; }
            buffer = grown;
            capacity = next;
        }
        int n = esp_http_client_read(client, (char *)buffer + used, capacity - used);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        used += n;
        if (esp_http_client_is_complete_data_received(client)) break;
    }
    if (err == ESP_OK && (!esp_http_client_is_complete_data_received(client) ||
                          (expected > 0 && used != (size_t)expected))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        *wav = buffer;
        *wav_size = used;
    } else {
        free(buffer);
    }
    return err;
}

static esp_err_t synthesize_and_play(const char *text, tts_result_t *result)
{
    if (!CONFIG_TTS_SERVER_URL[0] || !CONFIG_TTS_BEARER_TOKEN[0]) {
        ESP_LOGW(TAG, "TTS disabled: configure server URL and bearer token in menuconfig");
        return ESP_ERR_INVALID_STATE;
    }
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (!wifi_sta_is_connected()) return ESP_ERR_INVALID_STATE;
    const char *base = CONFIG_TTS_SERVER_URL;
    if (strncmp(base, "https://", 8) != 0) {
        ESP_LOGE(TAG, "TTS URL must use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(base);
    while (len && base[len - 1] == '/') --len;
    char url[TTS_URL_CAPACITY];
    int url_len = snprintf(url, sizeof(url), "%.*s/tts", (int)len, base);
    if (url_len < 0 || url_len >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = ESP_OK;
    cJSON *json = cJSON_CreateObject();
    char *payload = NULL;
    esp_http_client_handle_t client = NULL;
    uint8_t *wav = NULL;
    size_t wav_size = 0;
    if (!json || !cJSON_AddStringToObject(json, "text", text) ||
        !cJSON_AddNumberToObject(json, "speed", 1.0)) { err = ESP_ERR_NO_MEM; goto done; }
    payload = cJSON_PrintUnformatted(json);
    if (!payload) { err = ESP_ERR_NO_MEM; goto done; }
    char authorization[sizeof("Bearer ") + sizeof(CONFIG_TTS_BEARER_TOKEN)];
    int auth_len = snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_TTS_BEARER_TOKEN);
    if (auth_len < 0 || auth_len >= sizeof(authorization)) { err = ESP_ERR_INVALID_SIZE; goto done; }
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_TTS_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    client = esp_http_client_init(&config);
    if (!client) { err = ESP_ERR_NO_MEM; goto done; }
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "audio/wav");
    size_t payload_size = strlen(payload);
    err = esp_http_client_open(client, payload_size);
    if (err != ESP_OK) goto done;
    err = write_all(client, payload, payload_size);
    if (err != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(client) < 0) { err = ESP_FAIL; goto done; }
    result->http_status = esp_http_client_get_status_code(client);
    if (result->http_status != 200) {
        ESP_LOGE(TAG, "TTS HTTP status: %d", result->http_status);
        err = ESP_FAIL;
        goto done;
    }
    err = fetch_wav(client, &wav, &wav_size);
    if (err == ESP_OK) err = speaker_play_wav(wav, wav_size);
done:
    free(wav);
    if (client) esp_http_client_cleanup(client);
    free(payload);
    cJSON_Delete(json);
    return err;
}

void tts_task(void *arg)
{
    QueueHandle_t requests = arg;
    while (true) {
        tts_request_t request;
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
        tts_result_t result = { 0 };
        result.err = synthesize_and_play(request.text, &result);
        if (s_results) xQueueOverwrite(s_results, &result);
    }
}
