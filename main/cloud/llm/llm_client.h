#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define LLM_TEXT_CAPACITY 512
#define LLM_REPLY_CAPACITY 1024

typedef struct {
    char text[LLM_TEXT_CAPACITY];
} llm_request_t;

typedef struct {
    esp_err_t err;
    int http_status;
    int64_t elapsed_ms;
    char reply[LLM_REPLY_CAPACITY];
} llm_result_t;

esp_err_t llm_client_init(void);
esp_err_t llm_client_chat(const char *text, llm_result_t *result);
void llm_client_deinit(void);
void llm_task(void *arg);
void llm_set_result_queue(QueueHandle_t results);
