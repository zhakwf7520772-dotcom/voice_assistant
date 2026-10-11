#include "gree_ac.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gree_raw_catalog.h"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static gree_ac_state_t s_state = { .power = false, .mode = GREE_COOL, .temperature_c = 25, .fan = 1 };

esp_err_t gree_ac_init(const ir_transmitter_config_t *config)
{
    return ir_transmitter_init(config);
}

esp_err_t gree_ac_set_power(bool power)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.power = power;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t gree_ac_set_mode(gree_ac_mode_t mode)
{
    if (mode != GREE_COOL) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_state_lock);
    s_state.mode = mode;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t gree_ac_set_temp(uint8_t temperature_c)
{
    if (temperature_c < 25 || temperature_c > 27) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_state_lock);
    s_state.temperature_c = temperature_c;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t gree_ac_set_fan(uint8_t fan)
{
    if (fan > 5) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_state_lock);
    s_state.fan = fan;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t gree_ac_send_state(const gree_ac_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    // Copy caller data before waiting for TX so a concurrent state change cannot
    // alter which recorded command was selected.
    const gree_ac_state_t request = *state;
    for (size_t i = 0; i < gree_raw_catalog_count; ++i) {
        const gree_raw_entry_t *entry = &gree_raw_catalog[i];
        if (entry->power == request.power && entry->mode_code == request.mode &&
            entry->temperature_c == request.temperature_c && entry->fan == request.fan) {
            return ir_transmitter_replay(entry->command);
        }
    }
    ESP_LOGE("GREE_AC", "No recorded command: power=%d mode=%d temp=%u fan=%u",
             request.power, request.mode, request.temperature_c, request.fan);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t gree_ac_send(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const gree_ac_state_t request = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return gree_ac_send_state(&request);
}
