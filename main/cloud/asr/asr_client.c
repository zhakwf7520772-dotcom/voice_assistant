#include "asr_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "tencent_credentials.h"
#include "tencent_sign.h"
#include "wifi_sta.h"

#define MAX_ASR_SAMPLES (16000 * 15)
#define RESPONSE_CAPACITY 4096

static const char *TAG = "asr";
static QueueHandle_t s_results;
static bool s_time_synced;

void asr_set_result_queue(QueueHandle_t queue) { s_results = queue; }

static esp_err_t wait_for_time(void)
{
    if (s_time_synced) return ESP_OK;
    const char *servers[] = {"ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org"};
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); ++i) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(servers[i]);
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "syncing clock via %s", servers[i]);
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(12000));
        if (err == ESP_OK && time(NULL) > 1700000000) {
            s_time_synced = true;
            ESP_LOGI(TAG, "network clock synchronized");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "clock sync with %s failed: %s", servers[i], esp_err_to_name(err));
        esp_netif_sntp_deinit();
    }
    ESP_LOGE(TAG, "clock sync failed on all NTP servers; check DNS and UDP/123 on hotspot");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t http_write_all(esp_http_client_handle_t client, const char *p, size_t size)
{
    while (size > 0) {
        int written = esp_http_client_write(client, p, size);
        if (written <= 0) return ESP_FAIL;
        p += written;
        size -= written;
    }
    return ESP_OK;
}

static esp_err_t parse_response(const char *response, char *text, size_t capacity)
{
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *root = cJSON_Parse(response);
    cJSON *body = root ? cJSON_GetObjectItemCaseSensitive(root, "Response") : NULL;
    cJSON *error = body ? cJSON_GetObjectItemCaseSensitive(body, "Error") : NULL;
    if (error) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "Code");
        if (cJSON_IsString(code)) ESP_LOGE(TAG, "Tencent Cloud error: %s", code->valuestring);
    } else if (body) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(body, "Result");
        if (cJSON_IsString(result) && result->valuestring && result->valuestring[0] &&
            strlen(result->valuestring) < capacity) {
            strcpy(text, result->valuestring);
            err = ESP_OK;
        } else {
            ESP_LOGW(TAG, "Tencent Cloud returned empty or oversized text");
        }
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t recognize(const asr_request_t *request, char *text, size_t capacity)
{
    if (!TENCENT_SECRET_ID[0] || !TENCENT_SECRET_KEY[0]) {
        ESP_LOGE(TAG, "Tencent Cloud credentials are not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!request || !request->pcm || !request->samples || request->samples > MAX_ASR_SAMPLES) return ESP_ERR_INVALID_ARG;
    esp_err_t err = wifi_sta_wait_connected();
    if (err != ESP_OK) return err;
    err = wait_for_time();
    if (err != ESP_OK) return err;

    size_t pcm_bytes = request->samples * sizeof(int16_t);
    size_t encoded_size = 4 * ((pcm_bytes + 2) / 3);
    const char *prefix = "{\"EngSerViceType\":\"16k_zh\",\"SubServiceType\":2,\"SourceType\":1,\"VoiceFormat\":\"pcm\",\"DataLen\":";
    char number[16];
    int number_length = snprintf(number, sizeof(number), "%u", (unsigned)pcm_bytes);
    if (number_length <= 0 || (size_t)number_length >= sizeof(number)) return ESP_FAIL;
    const char *middle = ",\"Data\":\"";
    size_t header_size = strlen(prefix) + number_length + strlen(middle);
    size_t body_size = header_size + encoded_size + 2;
    char *body = heap_caps_malloc(body_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body, prefix, strlen(prefix));
    memcpy(body + strlen(prefix), number, number_length);
    memcpy(body + strlen(prefix) + number_length, middle, strlen(middle));
    size_t written = 0;
    int base64_err = mbedtls_base64_encode((unsigned char *)body + header_size, encoded_size + 1, &written,
                                            (const unsigned char *)request->pcm, pcm_bytes);
    if (base64_err != 0 || written != encoded_size) {
        free(body);
        return ESP_FAIL;
    }
    body[header_size + written] = '"';
    body[header_size + written + 1] = '}';
    body[body_size] = '\0';

    time_t now = time(NULL);
    char authorization[320], timestamp[24];
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)now);
    err = tencent_sign_authorization(body, body_size, (int64_t)now, authorization, sizeof(authorization));
    if (err != ESP_OK) { free(body); return err; }

    esp_http_client_config_t config = {
        .url = "https://" TENCENT_ASR_HOST "/",
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_VOICE_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(body); return ESP_ERR_NO_MEM; }
    esp_http_client_set_header(client, "Content-Type", TENCENT_CONTENT_TYPE);
    esp_http_client_set_header(client, "Host", TENCENT_ASR_HOST);
    esp_http_client_set_header(client, "X-TC-Action", "SentenceRecognition");
    esp_http_client_set_header(client, "X-TC-Version", "2019-06-14");
    esp_http_client_set_header(client, "X-TC-Timestamp", timestamp);
    esp_http_client_set_header(client, "Authorization", authorization);
    err = esp_http_client_open(client, body_size);
    if (err == ESP_OK) err = http_write_all(client, body, body_size);
    free(body);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "Tencent Cloud HTTP status %d", status);
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        char *response = heap_caps_malloc(RESPONSE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!response) err = ESP_ERR_NO_MEM;
        else {
            size_t total = 0;
            int n;
            while (total < RESPONSE_CAPACITY - 1 &&
                   (n = esp_http_client_read(client, response + total, RESPONSE_CAPACITY - 1 - total)) > 0) {
                total += n;
                if (esp_http_client_is_complete_data_received(client)) break;
            }
            response[total] = 0;
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_SIZE;
            else err = parse_response(response, text, capacity);
            free(response);
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

void asr_task(void *arg)
{
    QueueHandle_t requests = arg;
    // Prepare networking in this worker while microphone and wake detection run.
    esp_err_t warmup_err = wifi_sta_wait_connected();
    if (warmup_err == ESP_OK) warmup_err = wait_for_time();
    if (warmup_err == ESP_OK) ESP_LOGI(TAG, "ASR network ready");
    else ESP_LOGW(TAG, "ASR network warmup failed: %s; will retry on request",
                  esp_err_to_name(warmup_err));
    while (true) {
        asr_request_t request;
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
        asr_result_t result = { 0 };
        int64_t start = esp_timer_get_time();
        result.err = recognize(&request, result.text, sizeof(result.text));
        result.elapsed_ms = (esp_timer_get_time() - start) / 1000;
        if (s_results && xQueueSend(s_results, &result, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "ASR result queue full");
        }
    }
}
