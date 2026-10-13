#include "http_response.h"
#include "esp_timer.h"

esp_err_t http_response_drain(esp_http_client_handle_t client, size_t maximum,
                              int64_t deadline_us,
                              esp_err_t (*set_timeout)(esp_http_client_handle_t, int))
{
    char buffer[256];
    size_t consumed = 0;
    while (true) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return ESP_ERR_TIMEOUT;
        int remaining_ms = (int)((remaining_us + 999) / 1000);
        esp_err_t err = set_timeout(client, remaining_ms);
        if (err != ESP_OK) return err;
        // Always read, even if the parser has already received the whole body.
        int n = esp_http_client_read(client, buffer, sizeof(buffer));
        if (n < 0) return n == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
        if (n == 0) return esp_http_client_is_complete_data_received(client) ?
                           ESP_OK : ESP_ERR_INVALID_RESPONSE;
        if ((size_t)n > maximum - consumed) return ESP_ERR_INVALID_SIZE;
        consumed += n;
    }
}
