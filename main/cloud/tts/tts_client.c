#include "tts_client.h"

#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
#include "sdkconfig.h"
#include "speaker.h"
#include "wav_pcm.h"
#include "http_response.h"
#include "connection_schedule.h"
#include "wifi_sta.h"

#define TTS_MAX_WAV_BYTES (3 * 1024 * 1024)
#define TTS_URL_CAPACITY 512
#define TTS_READ_CHUNK_BYTES 4096
#define TTS_READ_TIMEOUT_MS 15000
#define TTS_CONNECT_TIMEOUT_MS 10000
#define TTS_HEALTH_TIMEOUT_MS 4000

static const char *TAG = "tts";
static QueueHandle_t s_results;
static esp_http_client_handle_t s_http_client;
static connection_schedule_t s_connection;

typedef struct {
    const uint8_t *wav;
    size_t total;
    atomic_size_t received;
    atomic_int status;
    atomic_bool began;
    QueueHandle_t completion;
    int64_t request_started;
    int64_t speech_end_us;
} streamed_wav_t;

static size_t stream_available(void *context)
{
    return atomic_load(&((streamed_wav_t *)context)->received);
}

static esp_err_t stream_status(void *context)
{
    return atomic_load(&((streamed_wav_t *)context)->status);
}

static void stream_began(void *context)
{
    streamed_wav_t *stream = context;
    atomic_store(&stream->began, true);
    ESP_LOGI(TAG, "First audio queued: %lld ms from TTS request; downloaded %u/%u bytes",
             (long long)((esp_timer_get_time() - stream->request_started) / 1000),
             (unsigned)atomic_load(&stream->received), (unsigned)stream->total);
    if (stream->speech_end_us) ESP_LOGI(TAG, "Speech ended to first audio: %lld ms",
        (long long)((esp_timer_get_time() - stream->speech_end_us) / 1000));
}

static void playback_task(void *context)
{
    streamed_wav_t *stream = context;
    speaker_wav_source_t source = {
        .wav = stream->wav, .length = stream->total,
        .available = stream_available, .status = stream_status,
        .on_start = stream_began, .context = stream,
    };
    esp_err_t err = speaker_play_wav_stream(&source);
    xQueueSend(stream->completion, &err, portMAX_DELAY);
    vTaskDelete(NULL);
}

typedef struct {
    uint16_t local_port;
    bool found;
    bool sack_enabled;
    uint32_t window, advertised_window, next_sequence;
    unsigned queued_segments, queued_bytes;
    unsigned received, dropped, memory_errors, checksum_errors;
} tcp_receive_snapshot_t;

static esp_err_t snapshot_tcp_receive(void *context)
{
    tcp_receive_snapshot_t *snapshot = context;
    // The PCB list is private to lwIP: inspect it only in its TCP/IP thread.
    // Copy values here; do all serial logging after leaving that thread.
    for (const struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
        if (pcb->local_port != snapshot->local_port || pcb->remote_port != 443) continue;
        snapshot->found = true;
        snapshot->window = pcb->rcv_wnd;
        snapshot->advertised_window = pcb->rcv_ann_wnd;
        snapshot->next_sequence = pcb->rcv_nxt;
#if LWIP_TCP_SACK_OUT
        snapshot->sack_enabled = (pcb->flags & TF_SACK) != 0;
#endif
#if TCP_QUEUE_OOSEQ
        for (const struct tcp_seg *segment = pcb->ooseq; segment; segment = segment->next) {
            ++snapshot->queued_segments;
            snapshot->queued_bytes += segment->len;
        }
#endif
        break;
    }
#if LWIP_STATS && TCP_STATS
    snapshot->received = lwip_stats.tcp.recv;
    snapshot->dropped = lwip_stats.tcp.drop;
    snapshot->memory_errors = lwip_stats.tcp.memerr;
    snapshot->checksum_errors = lwip_stats.tcp.chkerr;
#endif
    return ESP_OK;
}

static void log_tcp_receive(esp_http_client_handle_t client)
{
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    int fd = esp_http_client_get_socket(client);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &length) != 0 ||
        local.sin_family != AF_INET) return;
    tcp_receive_snapshot_t snapshot = { .local_port = ntohs(local.sin_port) };
    if (esp_netif_tcpip_exec(snapshot_tcp_receive, &snapshot) != ESP_OK || !snapshot.found) return;
    ESP_LOGI(TAG, "TCP receive: port=%u window=%lu advertised=%lu next_seq=%lu; out_of_order=%u/%u bytes; SACK=%s",
             (unsigned)snapshot.local_port, (unsigned long)snapshot.window,
             (unsigned long)snapshot.advertised_window, (unsigned long)snapshot.next_sequence,
             snapshot.queued_segments, snapshot.queued_bytes, snapshot.sack_enabled ? "yes" : "no");
    ESP_LOGI(TAG, "TCP stack counters: recv=%u drop=%u memerr=%u checksum=%u (all connections)",
             snapshot.received, snapshot.dropped, snapshot.memory_errors, snapshot.checksum_errors);
}

static void log_receive_memory(void)
{
    ESP_LOGI(TAG, "Receive memory: internal free=%u largest=%u minimum=%u; PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t set_read_timeout(esp_http_client_handle_t client, int timeout_ms)
{
    esp_err_t err = esp_http_client_set_timeout_ms(client, timeout_ms);
    if (err != ESP_OK) return err;
    // IDF's HTTP setter only changes client->timeout_ms. A blocking TLS
    // socket retains the receive timeout set when the connection was opened.
    int fd = esp_http_client_get_socket(client);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGE(TAG, "Could not set TLS socket receive timeout");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        (!strcasecmp(event->header_key, "CF-Ray") ||
         !strcasecmp(event->header_key, "Content-Encoding") ||
         !strcasecmp(event->header_key, "Content-Type"))) {
        ESP_LOGI(TAG, "Response header %s: %s", event->header_key, event->header_value);
    }
    return ESP_OK;
}

static void log_peer(esp_http_client_handle_t client)
{
    int fd = esp_http_client_get_socket(client);
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (fd >= 0 && getpeername(fd, (struct sockaddr *)&address, &length) == 0 &&
        address.ss_family == AF_INET) {
        struct sockaddr_in *peer = (struct sockaddr_in *)&address;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip))) {
            ESP_LOGI(TAG, "Public HTTPS peer: %s:%u", ip, (unsigned)ntohs(peer->sin_port));
        }
    }
}

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

static esp_err_t fetch_wav(esp_http_client_handle_t client, uint8_t **wav, size_t *wav_size,
                           bool *played, int64_t request_started, int64_t speech_end_us)
{
    int64_t expected = esp_http_client_get_content_length(client);
    if (expected > TTS_MAX_WAV_BYTES) return ESP_ERR_INVALID_SIZE;
    size_t capacity = expected > 0 ? (size_t)expected : 65536;
    uint8_t *buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    size_t used = 0;
    size_t last_progress = 0;
    int64_t download_started = esp_timer_get_time();
    int64_t last_progress_time = download_started;
    int read_timeout = CONFIG_TTS_REQUEST_TIMEOUT_MS < TTS_READ_TIMEOUT_MS ?
                       CONFIG_TTS_REQUEST_TIMEOUT_MS : TTS_READ_TIMEOUT_MS;
    esp_err_t err = set_read_timeout(client, read_timeout);
    if (err != ESP_OK) { free(buffer); return err; }
    streamed_wav_t stream = { .wav = buffer, .total = expected > 0 ? (size_t)expected : 0,
                              .request_started = request_started, .speech_end_us = speech_end_us };
    atomic_init(&stream.received, 0);
    atomic_init(&stream.status, ESP_ERR_NOT_FINISHED);
    atomic_init(&stream.began, false);
    bool concurrent_playback = false;
    if (expected > 0) {
        stream.completion = xQueueCreate(1, sizeof(esp_err_t));
        if (stream.completion &&
            xTaskCreatePinnedToCore(playback_task, "tts_play", 4096, &stream, 4, NULL, 1) == pdPASS) {
            concurrent_playback = true;
            ESP_LOGI(TAG, "Streaming playback enabled; prebuffer %d ms", CONFIG_TTS_PLAYBACK_BUFFER_MS);
        } else {
            if (stream.completion) vQueueDelete(stream.completion);
            stream.completion = NULL;
            ESP_LOGW(TAG, "Playback worker unavailable; using complete-file playback");
        }
    }
    while (true) {
        if (esp_timer_get_time() - download_started >= (int64_t)CONFIG_TTS_REQUEST_TIMEOUT_MS * 1000) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (used == capacity) {
            if (esp_http_client_is_complete_data_received(client)) break;
            if (expected > 0) { err = ESP_ERR_INVALID_RESPONSE; break; }
            if (capacity == TTS_MAX_WAV_BYTES) { err = ESP_ERR_INVALID_SIZE; break; }
            size_t next = capacity > TTS_MAX_WAV_BYTES / 2 ? TTS_MAX_WAV_BYTES : capacity * 2;
            uint8_t *grown = heap_caps_realloc(buffer, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { err = ESP_ERR_NO_MEM; break; }
            buffer = grown;
            capacity = next;
        }
        // esp_http_client_read tries to fill the requested length before returning.
        // Keep reads small so a large WAV does not hide all progress in one call.
        size_t to_read = capacity - used;
        if (to_read > TTS_READ_CHUNK_BYTES) to_read = TTS_READ_CHUNK_BYTES;
        int n = esp_http_client_read(client, (char *)buffer + used, (int)to_read);
        if (n < 0) {
            err = n == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            ESP_LOGE(TAG, "WAV read failed: %d; received %u/%lld bytes; errno=%d",
                     n, (unsigned)used, (long long)expected, esp_http_client_get_errno(client));
            break;
        }
        if (n == 0) break;
        bool first_chunk = used == 0;
        used += n;
        if (first_chunk && expected > 0) {
            wav_pcm_info_t info;
            esp_err_t header_err = wav_pcm_parse(buffer, used, (size_t)expected, &info);
            if (header_err != ESP_OK && header_err != ESP_ERR_NOT_FINISHED) {
                ESP_LOGE(TAG, "Invalid WAV prefix: %s; refusing audio before playback",
                         esp_err_to_name(header_err));
                err = header_err;
                break;
            }
        }
        if (concurrent_playback) atomic_store(&stream.received, used);
        int64_t now = esp_timer_get_time();
        if (first_chunk || now - last_progress_time >= 5000000) {
            int64_t interval_us = now - last_progress_time;
            unsigned long rate = interval_us > 0 ?
                (unsigned long)((uint64_t)(used - last_progress) * 1000000 / interval_us) : 0;
            ESP_LOGI(TAG, "WAV download: %u/%lld bytes; interval %lu bytes/s; core=%d",
                     (unsigned)used, (long long)expected, rate, xPortGetCoreID());
            log_receive_memory();
            log_tcp_receive(client);
            last_progress = used;
            last_progress_time = now;
        }
        if (esp_http_client_is_complete_data_received(client)) break;
    }
    if (err == ESP_OK && (!esp_http_client_is_complete_data_received(client) ||
                          (expected > 0 && used != (size_t)expected))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        wav_pcm_info_t info;
        err = wav_pcm_parse(buffer, used, used, &info);
    }
    if (err == ESP_OK) {
        int64_t elapsed_us = esp_timer_get_time() - download_started;
        unsigned long bytes_per_second = elapsed_us > 0 ?
            (unsigned long)((uint64_t)used * 1000000 / elapsed_us) : 0;
        ESP_LOGI(TAG, "WAV download complete: %u bytes; %lld ms; %lu bytes/s",
                 (unsigned)used, (long long)(elapsed_us / 1000), bytes_per_second);
    } else {
        ESP_LOGE(TAG, "WAV download incomplete: %u/%lld bytes; %s", (unsigned)used,
                 (long long)expected, esp_err_to_name(err));
    }
    // With TLS allocations in PSRAM, retain a fully consumed persistent response.
    // Never reuse an incomplete response or a connection the server closes.
    if (err != ESP_OK || !esp_http_client_is_persistent_connection(client)) {
        esp_http_client_close(client);
        s_connection.ready = false;
    }
    if (concurrent_playback) {
        atomic_store(&stream.status, err);
        esp_err_t playback_err;
        xQueueReceive(stream.completion, &playback_err, portMAX_DELAY);
        vQueueDelete(stream.completion);
        *played = atomic_load(&stream.began);
        if (err == ESP_OK && playback_err != ESP_OK) {
            if (playback_err == ESP_ERR_NO_MEM && !*played) {
                ESP_LOGW(TAG, "Not enough memory to overlap playback; using complete-file playback");
            } else {
                err = playback_err;
            }
        }
    }
    if (err == ESP_OK) {
        *wav = buffer;
        *wav_size = used;
    } else {
        free(buffer);
    }
    return err;
}

static esp_http_client_handle_t get_client(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_CONNECT_TIMEOUT_MS,
        .buffer_size = TTS_READ_CHUNK_BYTES,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = http_event,
        .keep_alive_enable = true,
        .keep_alive_idle = 15,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    if (!s_http_client) s_http_client = esp_http_client_init(&config);
    return s_http_client;
}

static esp_err_t set_authorization(esp_http_client_handle_t client)
{
    char authorization[sizeof("Bearer ") + sizeof(CONFIG_TTS_BEARER_TOKEN)];
    snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_TTS_BEARER_TOKEN);
    return esp_http_client_set_header(client, "Authorization", authorization);
}

static bool socket_is_closed(esp_http_client_handle_t client)
{
    int fd = esp_http_client_get_socket(client);
    if (fd < 0) return true;
    // Only peek at the TCP socket. TLS must consume any actual received bytes.
    char byte;
    int n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

static void prewarm_connection(void)
{
    if (!CONFIG_TTS_BEARER_TOKEN[0] || strncmp(CONFIG_TTS_SERVER_URL, "https://", 8) ||
        !wifi_sta_is_connected()) return;
    int64_t started = esp_timer_get_time();
    char url[TTS_URL_CAPACITY];
    size_t len = strlen(CONFIG_TTS_SERVER_URL);
    while (len && CONFIG_TTS_SERVER_URL[len - 1] == '/') --len;
    int count = snprintf(url, sizeof(url), "%.*s/health", (int)len, CONFIG_TTS_SERVER_URL);
    if (count < 0 || count >= sizeof(url)) return;
    esp_http_client_handle_t client = get_client(url);
    if (!client) { connection_schedule_failed(&s_connection, esp_timer_get_time()); return; }
    bool was_ready = s_connection.ready;
    const char *stage = "prepare";
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (err == ESP_OK) err = set_authorization(client);
    if (err == ESP_OK) {
        err = was_ready ? set_read_timeout(client, TTS_CONNECT_TIMEOUT_MS) :
                          esp_http_client_set_timeout_ms(client, TTS_CONNECT_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        stage = "connect";
        err = esp_http_client_open(client, 0);
    }
    int64_t health_deadline = esp_timer_get_time() + (int64_t)TTS_HEALTH_TIMEOUT_MS * 1000;
    if (err == ESP_OK) {
        stage = "response headers";
        err = set_read_timeout(client, TTS_HEALTH_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        int64_t headers = esp_http_client_fetch_headers(client);
        if (headers < 0) err = headers == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200) err = ESP_FAIL;
    // Consume the entire health response before reusing the connection.
    if (err == ESP_OK) {
        stage = "health response body";
        err = http_response_drain(client, 4096, health_deadline, set_read_timeout);
    }
    s_connection.ready = err == ESP_OK && esp_http_client_is_persistent_connection(client);
    ESP_LOGI(TAG, "Connection prewarm: %s; %lld ms", s_connection.ready ? "ready" : "unavailable",
             (long long)((esp_timer_get_time() - started) / 1000));
    if (s_connection.ready) {
        connection_schedule_ready(&s_connection, esp_timer_get_time());
        ESP_LOGI(TAG, "Persistent HTTPS ready; open/health %s", was_ready ? "reused" : "connected");
    } else {
        connection_schedule_failed(&s_connection, esp_timer_get_time());
        ESP_LOGW(TAG, "Prewarm detail: stage=%s; error=%s; HTTP=%d; errno=%d; persistent=%s",
                 stage, esp_err_to_name(err), esp_http_client_get_status_code(client),
                 esp_http_client_get_errno(client),
                 esp_http_client_is_persistent_connection(client) ? "yes" : "no");
        esp_http_client_cleanup(client);
        s_http_client = NULL;
    }
}

static esp_err_t synthesize_and_play(const tts_request_t *request, tts_result_t *result)
{
    const char *text = request->text;
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
    int64_t started = request->queued_us ? request->queued_us : esp_timer_get_time();
    const char *stage = "prepare request";
    cJSON *json = cJSON_CreateObject();
    char *payload = NULL;
    esp_http_client_handle_t client = NULL;
    uint8_t *wav = NULL;
    size_t wav_size = 0;
    bool played = false;
    if (!json || !cJSON_AddStringToObject(json, "text", text) ||
        !cJSON_AddNumberToObject(json, "speed", 1.0)) { err = ESP_ERR_NO_MEM; goto done; }
    payload = cJSON_PrintUnformatted(json);
    if (!payload) { err = ESP_ERR_NO_MEM; goto done; }
    char authorization[sizeof("Bearer ") + sizeof(CONFIG_TTS_BEARER_TOKEN)];
    int auth_len = snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_TTS_BEARER_TOKEN);
    if (auth_len < 0 || auth_len >= sizeof(authorization)) { err = ESP_ERR_INVALID_SIZE; goto done; }
    bool reused = s_http_client != NULL;
    client = get_client(url);
    if (!client) { err = ESP_ERR_NO_MEM; goto done; }
    if (s_connection.ready && socket_is_closed(client)) {
        ESP_LOGW(TAG, "Idle HTTPS socket closed; reconnecting before request body");
        esp_http_client_close(client);
        s_connection.ready = false;
    }
    err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err != ESP_OK) goto done;
    if (s_connection.ready) err = set_read_timeout(client, TTS_CONNECT_TIMEOUT_MS);
    else err = esp_http_client_set_timeout_ms(client, TTS_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "HTTPS client: %s; prewarmed connection: %s",
             reused ? "reused" : "new", s_connection.ready ? "yes" : "no");
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "audio/wav");
    size_t payload_size = strlen(payload);
    stage = "connect";
    ESP_LOGI(TAG, "POST %s; timeout %d ms", url, CONFIG_TTS_REQUEST_TIMEOUT_MS);
    bool connection_reused = s_connection.ready;
    int64_t connect_started = esp_timer_get_time();
    err = esp_http_client_open(client, payload_size);
    if (err != ESP_OK && s_connection.ready) {
        // No request body has been sent: safely reconnect an expired idle socket.
        ESP_LOGW(TAG, "Prewarmed connection expired; reconnecting");
        esp_http_client_close(client);
        connection_reused = false;
        err = esp_http_client_open(client, payload_size);
    }
    s_connection.ready = false;
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "TTS connection: reused=%s; open=%lld ms; queue wait=%lld ms",
             connection_reused ? "yes" : "no",
             (long long)((esp_timer_get_time() - connect_started) / 1000),
             (long long)((connect_started - started) / 1000));
    err = set_read_timeout(client, CONFIG_TTS_REQUEST_TIMEOUT_MS);
    if (err != ESP_OK) goto done;
    log_peer(client);
    stage = "send request";
    ESP_LOGI(TAG, "Connected; sending %u JSON bytes", (unsigned)payload_size);
    err = write_all(client, payload, payload_size);
    if (err != ESP_OK) goto done;
    stage = "wait for response headers";
    ESP_LOGI(TAG, "Request sent; waiting for response headers");
    int64_t header_result = esp_http_client_fetch_headers(client);
    if (header_result < 0) {
        ESP_LOGE(TAG, "Response header read failed: %lld; errno=%d",
                 (long long)header_result, esp_http_client_get_errno(client));
        err = header_result == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
        goto done;
    }
    result->http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d; content length: %lld; chunked: %s; elapsed: %lld ms",
             result->http_status, (long long)esp_http_client_get_content_length(client),
             esp_http_client_is_chunked_response(client) ? "yes" : "no",
             (long long)((esp_timer_get_time() - started) / 1000));
    if (result->http_status != 200) {
        ESP_LOGE(TAG, "TTS HTTP status: %d", result->http_status);
        err = ESP_FAIL;
        goto done;
    }
    stage = "download and stream WAV";
    log_receive_memory();
    log_tcp_receive(client);
    wifi_ap_record_t ap;
    wifi_ps_type_t power_save;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && esp_wifi_get_ps(&power_save) == ESP_OK) {
        ESP_LOGI(TAG, "Receive diagnostics: RSSI=%d dBm; power_save=%d; TCP window=%d; task priority=%u",
                 ap.rssi, (int)power_save, CONFIG_LWIP_TCP_WND_DEFAULT,
                 (unsigned)uxTaskPriorityGet(NULL));
    }
    ESP_LOGI(TAG, "Receive buffers: Wi-Fi static=%d dynamic=%d BA window=%d; TCP queue=%d OOSEQ limit=%d",
             CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM,
             CONFIG_ESP_WIFI_RX_BA_WIN, CONFIG_LWIP_TCP_RECVMBOX_SIZE,
             CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS);
    ESP_LOGI(TAG, "Downloading WAV");
    err = fetch_wav(client, &wav, &wav_size, &played, started, request->speech_end_us);
    if (err == ESP_OK && !played) {
        stage = "validate and play WAV";
        ESP_LOGI(TAG, "Starting speaker; elapsed: %lld ms",
                 (long long)((esp_timer_get_time() - started) / 1000));
        err = speaker_play_wav(wav, wav_size);
    }
done:
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed during %s: %s; elapsed: %lld ms", stage,
                 esp_err_to_name(err), (long long)((esp_timer_get_time() - started) / 1000));
    } else {
        ESP_LOGI(TAG, "Playback complete; elapsed: %lld ms",
                 (long long)((esp_timer_get_time() - started) / 1000));
    }
    free(wav);
    if (client) {
        if (err == ESP_OK && esp_http_client_is_persistent_connection(client) &&
            esp_http_client_get_socket(client) >= 0) {
            connection_schedule_ready(&s_connection, esp_timer_get_time());
            ESP_LOGI(TAG, "Persistent HTTPS retained for next turn");
        } else {
            esp_http_client_cleanup(client);
            s_http_client = NULL;
            connection_schedule_failed(&s_connection, esp_timer_get_time());
        }
    }
    free(payload);
    cJSON_Delete(json);
    return err;
}

void tts_task(void *arg)
{
    QueueHandle_t requests = arg;
#ifdef CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
    ESP_LOGI(TAG, "TLS allocator: PSRAM; session ticket cache disabled");
#else
    ESP_LOGW(TAG, "TLS allocator: internal RAM; configure MBEDTLS_EXTERNAL_MEM_ALLOC for parallel prewarm");
#endif
    while (true) {
        tts_request_t request;
        if (xQueueReceive(requests, &request, pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (!wifi_sta_is_connected()) {
                if (s_http_client) {
                    esp_http_client_cleanup(s_http_client);
                    s_http_client = NULL;
                    connection_schedule_failed(&s_connection, esp_timer_get_time());
                    ESP_LOGW(TAG, "Wi-Fi offline; persistent HTTPS released");
                }
                continue;
            }
            if (s_connection.ready && socket_is_closed(s_http_client)) {
                esp_http_client_cleanup(s_http_client);
                s_http_client = NULL;
                connection_schedule_failed(&s_connection, esp_timer_get_time());
                ESP_LOGW(TAG, "Idle HTTPS socket closed; background reconnect scheduled");
            }
            // Wait for ASR's background clock sync before certificate validation.
            if (time(NULL) > 1700000000 && connection_schedule_due(&s_connection, esp_timer_get_time()))
                prewarm_connection();
            continue;
        }
        if (request.prewarm) {
            if (!s_connection.ready && connection_schedule_due(&s_connection, esp_timer_get_time()))
                prewarm_connection();
            continue;
        }
        tts_result_t result = { 0 };
        result.err = synthesize_and_play(&request, &result);
        if (s_results) xQueueOverwrite(s_results, &result);
    }
}
