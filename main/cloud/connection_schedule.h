#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ready;
    unsigned retry_ms;
    int64_t next_check_us;
} connection_schedule_t;

static inline void connection_schedule_ready(connection_schedule_t *state, int64_t now_us)
{
    state->ready = true;
    state->retry_ms = 3000;
    state->next_check_us = now_us + 30000000;
}

static inline void connection_schedule_failed(connection_schedule_t *state, int64_t now_us)
{
    unsigned delay = state->retry_ms ? state->retry_ms : 3000;
    state->ready = false;
    state->next_check_us = now_us + (int64_t)delay * 1000;
    state->retry_ms = delay < 15000 ? delay * 2 : 30000;
}

static inline bool connection_schedule_due(const connection_schedule_t *state, int64_t now_us)
{
    return now_us >= state->next_check_us;
}
