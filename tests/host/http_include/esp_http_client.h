#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct mock_http *esp_http_client_handle_t;
#define ESP_ERR_HTTP_EAGAIN 0x7007
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
