#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_sta_wait_connected(void);
bool wifi_sta_is_connected(void);
