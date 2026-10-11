#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_types.h"
#include "esp_err.h"

typedef struct {
    const rmt_symbol_word_t *symbols; // Original demodulated RX polarity.
    size_t num_symbols;
    uint32_t gap_after_us;           // Actual idle gap to the next capture.
} ir_raw_frame_t;

typedef struct {
    const ir_raw_frame_t *frames;
    size_t num_frames;
} ir_raw_command_t;

typedef struct {
    gpio_num_t gpio_num;     // Required; no default pin and no automatic startup.
    uint32_t carrier_hz;    // Initial physical test: 38000.
    float duty_cycle;       // Initial physical test: 0.33.
    bool active_low;        // Physical emitter input polarity; default active-high.
} ir_transmitter_config_t;

// Initialize/deinitialize from a single lifecycle owner. Never call from an ISR.
esp_err_t ir_transmitter_init(const ir_transmitter_config_t *config);
esp_err_t ir_transmitter_deinit(void);

// Blocking, serialized task-context replay. All captures and their gaps are
// transmitted in ONE transaction; the user command may be released on return.
esp_err_t ir_transmitter_replay(const ir_raw_command_t *command);
