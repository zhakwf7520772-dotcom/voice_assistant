#pragma once

#include "ir_transmitter.h"

// Pure conversion helper: RX low -> logical carrier-on high. Ignore terminal
// zero-duration markers, split long idle gaps at 32767 ticks and pack symbols.
// On failure *num_symbols is zero, so partially written output cannot be sent.
esp_err_t ir_waveform_prepare(const ir_raw_command_t *command,
                              rmt_symbol_word_t *output, size_t capacity,
                              size_t *num_symbols, uint64_t *duration_us);
