#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Start connecting without waiting for DHCP. Call before starting cloud tasks.
esp_err_t wifi_sta_start(void);
esp_err_t wifi_sta_wait_connected(void);
bool wifi_sta_is_connected(void);
