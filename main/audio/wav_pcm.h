#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    size_t data_offset;
    size_t data_bytes;
    uint32_t sample_rate;
    uint16_t channels;
} wav_pcm_info_t;

// Validate PCM16 WAV metadata without reading beyond the published prefix.
// NOT_FINISHED means more header bytes are needed, not that all PCM is needed.
esp_err_t wav_pcm_parse(const uint8_t *wav, size_t available, size_t total,
                        wav_pcm_info_t *info);
