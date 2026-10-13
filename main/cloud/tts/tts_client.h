#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "llm_client.h"

typedef struct {
    int64_t queued_us;
    int64_t speech_end_us;
    bool prewarm; // Connection preparation only; produces no voice result.
    char text[LLM_REPLY_CAPACITY];
} tts_request_t;

typedef struct {
    esp_err_t err;
    int http_status;
} tts_result_t;

void tts_set_result_queue(QueueHandle_t results);
void tts_task(void *arg);
