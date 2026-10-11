#pragma once

#include "ir_transmitter.h"

typedef enum { GREE_COOL = 1 } gree_ac_mode_t;

typedef struct {
    bool power;
    gree_ac_mode_t mode;
    uint8_t temperature_c;
    uint8_t fan; // 0=auto, 1..5=manual.
} gree_ac_state_t;

// Prototype control API: only measured catalog combinations may be replayed.
// There is no speculative dynamic protocol encoder in this stage.
esp_err_t gree_ac_init(const ir_transmitter_config_t *config);
esp_err_t gree_ac_set_power(bool power);
esp_err_t gree_ac_set_mode(gree_ac_mode_t mode);
esp_err_t gree_ac_set_temp(uint8_t temperature_c);
esp_err_t gree_ac_set_fan(uint8_t fan);
esp_err_t gree_ac_send(void);

// Atomic whole-state alternative for concurrent clients. ESP_ERR_NOT_FOUND
// means the exact requested combination has not been captured yet.
esp_err_t gree_ac_send_state(const gree_ac_state_t *state);
