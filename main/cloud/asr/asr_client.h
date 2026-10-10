#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    const int16_t *pcm;
    size_t samples;
} asr_request_t;

typedef struct {
    esp_err_t err;
    int64_t elapsed_ms;
    char text[512];
} asr_result_t;

void asr_set_result_queue(QueueHandle_t queue);
void asr_task(void *arg);
