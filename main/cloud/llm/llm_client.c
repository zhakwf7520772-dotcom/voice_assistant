#include "llm_client.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

#define DEVICE_ID_CAPACITY 64
#define SESSION_ID_CAPACITY 96
#define REQUEST_ID_CAPACITY 64
#define URL_CAPACITY 512
#define MAX_REQUEST_BYTES 2048

static const char *TAG = "llm";
static QueueHandle_t s_results;
static char s_device_id[DEVICE_ID_CAPACITY];
static char s_session_id[SESSION_ID_CAPACITY];
static char s_url[URL_CAPACITY];
static uint32_t s_boot_nonce;
static uint32_t s_request_counter;
static bool s_initialized;

void llm_set_result_queue(QueueHandle_t results)
{
    s_results = results;
}

esp_err_t llm_client_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    const char *base = CONFIG_LLM_SERVER_URL;
    size_t len = strlen(base);
    if (len == 0) {
        ESP_LOGW(TAG, "LLM server URL is not configured");
        return ESP_ERR_INVALID_STATE;
    }
    bool https = strncmp(base, "https://", 8) == 0;
    if (!https && strncmp(base, "http://", 7) != 0) return ESP_ERR_INVALID_ARG;
    while (len > 0 && base[len - 1] == '/') --len;
    int n = snprintf(s_url, sizeof(s_url), "%.*s/api/chat", (int)len, base);
    if (n < 0 || n >= sizeof(s_url)) return ESP_ERR_INVALID_SIZE;
    if (!https) ESP_LOGW(TAG, "HTTP sends speech text without encryption; use only on a trusted LAN");

    if (CONFIG_LLM_DEVICE_ID[0]) {
        n = snprintf(s_device_id, sizeof(s_device_id), "%s", CONFIG_LLM_DEVICE_ID);
    } else {
        uint8_t mac[6];
        esp_err_t err = esp_efuse_mac_get_default(mac);
        if (err != ESP_OK) return err;
        n = snprintf(s_device_id, sizeof(s_device_id), "esp32_%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if (n < 0 || n >= sizeof(s_device_id)) return ESP_ERR_INVALID_SIZE;
    s_boot_nonce = esp_random();
    if (CONFIG_LLM_SESSION_ID[0]) {
        n = snprintf(s_session_id, sizeof(s_session_id), "%s", CONFIG_LLM_SESSION_ID);
    } else {
        n = snprintf(s_session_id, sizeof(s_session_id), "session_%08" PRIx32 "_%s",
                     s_boot_nonce, s_device_id);
    }
    if (n < 0 || n >= sizeof(s_session_id)) return ESP_ERR_INVALID_SIZE;
    s_request_counter = 0;
    s_initialized = true;
    return ESP_OK;
}

void llm_client_deinit(void)
{
    s_initialized = false;
    s_url[0] = 0;
    s_session_id[0] = 0;
    s_device_id[0] = 0;
    s_request_counter = 0;
}

static esp_err_t http_write_all(esp_http_client_handle_t client, const char *data, size_t size)
{
    while (size > 0) {
        int written = esp_http_client_write(client, data, size);
        if (written <= 0) return ESP_FAIL;
        data += written;
        size -= written;
    }
    return ESP_OK;
}

static esp_err_t parse_reply(const char *body, llm_result_t *result)
{
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *root = cJSON_Parse(body);
    if (root && cJSON_IsObject(root)) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        cJSON *reply = cJSON_GetObjectItemCaseSensitive(root, "reply");
        cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsNumber(code) && code->valuedouble == 0 &&
            cJSON_IsString(reply) && reply->valuestring && reply->valuestring[0] &&
            strlen(reply->valuestring) < sizeof(result->reply) &&
            cJSON_IsString(session) && session->valuestring && session->valuestring[0] &&
            strlen(session->valuestring) < sizeof(s_session_id)) {
            strcpy(result->reply, reply->valuestring);
            strcpy(s_session_id, session->valuestring);
            err = ESP_OK;
        } else if (cJSON_IsNumber(code) && code->valuedouble != 0) {
            ESP_LOGE(TAG, "server business code: %d", code->valueint);
            err = ESP_FAIL;
        } else {
            ESP_LOGE(TAG, "invalid reply or session_id in server response");
        }
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t llm_client_chat(const char *text, llm_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    int64_t start = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    cJSON *request = NULL;
    char *payload = NULL;
    char *response = NULL;
    esp_http_client_handle_t client = NULL;

    if (!s_initialized || !text || !text[0]) { err = ESP_ERR_INVALID_STATE; goto done; }
    if (strlen(text) >= LLM_TEXT_CAPACITY) { err = ESP_ERR_INVALID_SIZE; goto done; }
    if (!wifi_sta_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi disconnected; skipping LLM request");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    char request_id[REQUEST_ID_CAPACITY];
    int id_len = snprintf(request_id, sizeof(request_id), "req_%08" PRIx32 "_%08" PRIx32,
                          s_boot_nonce, ++s_request_counter);
    if (id_len < 0 || id_len >= sizeof(request_id)) { err = ESP_ERR_INVALID_SIZE; goto done; }

    request = cJSON_CreateObject();
    if (!request || !cJSON_AddStringToObject(request, "device_id", s_device_id) ||
        !cJSON_AddStringToObject(request, "session_id", s_session_id) ||
        !cJSON_AddStringToObject(request, "request_id", request_id) ||
        !cJSON_AddStringToObject(request, "text", text)) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    payload = cJSON_PrintUnformatted(request);
    if (!payload) { err = ESP_ERR_NO_MEM; goto done; }
    size_t payload_len = strlen(payload);
    if (payload_len > MAX_REQUEST_BYTES) { err = ESP_ERR_INVALID_SIZE; goto done; }

    esp_http_client_config_t config = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_LLM_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = strncmp(s_url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
    };
    client = esp_http_client_init(&config);
    if (!client) { err = ESP_ERR_NO_MEM; goto done; }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    err = esp_http_client_open(client, payload_len);
    if (err != ESP_OK) goto done;
    err = http_write_all(client, payload, payload_len);
    if (err != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(client) < 0) { err = ESP_FAIL; goto done; }
    result->http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", result->http_status);
    if (result->http_status < 200 || result->http_status >= 300) {
        err = ESP_FAIL;
        goto done;
    }
    int64_t content_length = esp_http_client_get_content_length(client);
    if (content_length > CONFIG_LLM_MAX_RESPONSE_BYTES) { err = ESP_ERR_INVALID_SIZE; goto done; }
    response = heap_caps_malloc(CONFIG_LLM_MAX_RESPONSE_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) { err = ESP_ERR_NO_MEM; goto done; }
    size_t used = 0;
    while (used < CONFIG_LLM_MAX_RESPONSE_BYTES) {
        int count = esp_http_client_read(client, response + used, CONFIG_LLM_MAX_RESPONSE_BYTES - used);
        if (count < 0) { err = ESP_FAIL; goto done; }
        if (count == 0) break;
        used += count;
        if (esp_http_client_is_complete_data_received(client)) break;
    }
    response[used] = 0;
    if (!esp_http_client_is_complete_data_received(client)) { err = ESP_ERR_INVALID_SIZE; goto done; }
    err = parse_reply(response, result);
done:
    if (client) esp_http_client_cleanup(client);
    cJSON_Delete(request);
    free(payload);
    free(response);
    result->err = err;
    result->elapsed_ms = (esp_timer_get_time() - start) / 1000;
    return err;
}

void llm_task(void *arg)
{
    QueueHandle_t requests = arg;
    esp_err_t init_err = llm_client_init();
    while (true) {
        llm_request_t request;
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
        llm_result_t result = { 0 };
        if (init_err == ESP_OK) llm_client_chat(request.text, &result);
        else result.err = init_err;
        // A single in-flight turn uses a one-slot queue; never discard its completion.
        if (s_results) xQueueOverwrite(s_results, &result);
    }
}
