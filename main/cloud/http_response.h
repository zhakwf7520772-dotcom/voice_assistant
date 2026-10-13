#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_http_client.h"

// Parser completion does not mean the caller has consumed cached body bytes.
esp_err_t http_response_drain(esp_http_client_handle_t client, size_t maximum,
                              int64_t deadline_us,
                              esp_err_t (*set_timeout)(esp_http_client_handle_t, int));
