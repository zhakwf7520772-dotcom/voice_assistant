#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Provisional pins: verify GPIO4/5/6 on the exact board schematic before wiring.
#define MIC_GPIO_BCLK          4
#define MIC_GPIO_WS            5
#define MIC_GPIO_DIN           6
#define MIC_SAMPLE_RATE_HZ     16000
#define MIC_FRAME_SAMPLES      256
#define MIC_READ_TIMEOUT_MS    1000

// INMP441 L/R is wired to GND (left slot). For 3.3 V use right slot instead.
#define MIC_CHANNEL_LEFT       1

esp_err_t microphone_init(void);
esp_err_t microphone_read(int32_t *samples, size_t capacity, size_t *samples_read);
esp_err_t microphone_deinit(void);
