#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Pins match the installed MAX98357: LRC=15, BCLK=16, DIN=17.
#define SPEAKER_GPIO_WS    15
#define SPEAKER_GPIO_BCLK  16
#define SPEAKER_GPIO_DOUT  17

// Plays a complete RIFF/WAVE file containing 16-bit little-endian PCM.
esp_err_t speaker_play_wav(const uint8_t *wav, size_t length);

typedef struct {
    const uint8_t *wav;
    size_t length;
    size_t (*available)(void *context);
    // NOT_FINISHED while downloading, OK on completion, otherwise an error.
    esp_err_t (*status)(void *context);
    void (*on_start)(void *context);
    void *context;
} speaker_wav_source_t;

esp_err_t speaker_play_wav_stream(const speaker_wav_source_t *source);

// Two short local tones to test the same playback path without a server.
esp_err_t speaker_play_test_tones(void);
