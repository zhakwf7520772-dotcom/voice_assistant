#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "http_response.h"

struct mock_http {
    const char *cached;
    size_t length, consumed;
    bool parser_complete;
    int read_error, reads;
    esp_err_t timeout_error;
};
static int64_t clock_us;

int64_t esp_timer_get_time(void) { return clock_us; }
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length)
{
    ++client->reads;
    if (client->read_error) return client->read_error;
    size_t n = client->length - client->consumed;
    if (n > (size_t)length) n = length;
    if (n) memcpy(buffer, client->cached + client->consumed, n);
    client->consumed += n;
    return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
    return client->parser_complete;
}
static esp_err_t set_timeout(esp_http_client_handle_t client, int milliseconds)
{
    assert(milliseconds > 0 && milliseconds <= 4000);
    return client->timeout_error;
}
int main(void)
{
    // Health can be wholly cached during fetch_headers, before caller read().
    struct mock_http client = { .cached = "{\"status\":\"ok\"}", .length = 15,
                                .parser_complete = true };
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_OK);
    assert(client.consumed == 15 && client.reads == 2);
    char large[600]; memset(large, 'x', sizeof(large));
    client = (struct mock_http){ .cached = large, .length = sizeof(large), .parser_complete = true };
    assert(http_response_drain(&client, 600, 4000000, set_timeout) == ESP_OK);
    assert(client.consumed == 600 && client.reads == 4);
    client = (struct mock_http){ .parser_complete = true };
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_OK);
    client = (struct mock_http){0};
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_ERR_INVALID_RESPONSE);
    client = (struct mock_http){ .cached = large, .length = 600, .parser_complete = true };
    assert(http_response_drain(&client, 599, 4000000, set_timeout) == ESP_ERR_INVALID_SIZE);
    client = (struct mock_http){ .read_error = -ESP_ERR_HTTP_EAGAIN };
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_ERR_TIMEOUT);
    client = (struct mock_http){ .read_error = ESP_FAIL };
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_FAIL);
    client = (struct mock_http){ .timeout_error = ESP_FAIL };
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_FAIL);
    clock_us = 4000000; client = (struct mock_http){0};
    assert(http_response_drain(&client, 4096, 4000000, set_timeout) == ESP_ERR_TIMEOUT);
    assert(client.reads == 0);
    puts("Cached health-body draining, size limits, truncation and timeout tests passed");
}
