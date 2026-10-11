#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start raw TSOP38238 reception on GPIO7 and the capture/logging tasks.
 * Call from app_main; subsequent calls after a successful start are harmless.
 * GPIO4 and the other existing peripherals are not reconfigured.
 */
esp_err_t ir_receiver_start(void);

#ifdef __cplusplus
}
#endif
