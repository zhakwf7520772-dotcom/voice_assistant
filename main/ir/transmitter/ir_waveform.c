#include "ir_waveform.h"

typedef struct {
    rmt_symbol_word_t *output;
    size_t capacity;
    size_t halves;
    uint64_t duration_us;
} waveform_builder_t;

static esp_err_t append_segment(waveform_builder_t *builder, unsigned level, uint32_t duration)
{
    while (duration) {
        size_t index = builder->halves / 2;
        if (index >= builder->capacity) return ESP_ERR_INVALID_SIZE;
        uint32_t chunk = duration > 32767 ? 32767 : duration;
        if ((builder->halves & 1) == 0) {
            builder->output[index].val = 0;
            builder->output[index].level0 = level;
            builder->output[index].duration0 = chunk;
        } else {
            builder->output[index].level1 = level;
            builder->output[index].duration1 = chunk;
        }
        ++builder->halves;
        builder->duration_us += chunk;
        duration -= chunk;
    }
    return ESP_OK;
}

esp_err_t ir_waveform_prepare(const ir_raw_command_t *command,
                              rmt_symbol_word_t *output, size_t capacity,
                              size_t *num_symbols, uint64_t *duration_us)
{
    if (num_symbols) *num_symbols = 0;
    if (duration_us) *duration_us = 0;
    if (!command || !command->frames || !command->num_frames || !output ||
        !capacity || !num_symbols || !duration_us) return ESP_ERR_INVALID_ARG;
    waveform_builder_t builder = { .output = output, .capacity = capacity };
    bool have_mark = false;
    for (size_t frame_index = 0; frame_index < command->num_frames; ++frame_index) {
        const ir_raw_frame_t *frame = &command->frames[frame_index];
        if (!frame->symbols || !frame->num_symbols) return ESP_ERR_INVALID_ARG;
        for (size_t i = 0; i < frame->num_symbols; ++i) {
            const rmt_symbol_word_t symbol = frame->symbols[i];
            // A zero-duration field stops hardware TX. Only a trailing RX EOF
            // may be omitted; an internal zero would silently cut off playback.
            if ((!symbol.duration0 || !symbol.duration1) && i + 1 != frame->num_symbols) {
                return ESP_ERR_INVALID_ARG;
            }
            if (!symbol.duration0 && symbol.duration1) return ESP_ERR_INVALID_ARG;
            if ((!symbol.level0 && symbol.duration0) || (!symbol.level1 && symbol.duration1)) {
                have_mark = true;
            }
            esp_err_t err = append_segment(&builder, !symbol.level0, symbol.duration0);
            if (err != ESP_OK) return err;
            err = append_segment(&builder, !symbol.level1, symbol.duration1);
            if (err != ESP_OK) return err;
        }
        esp_err_t err = append_segment(&builder, 0, frame->gap_after_us);
        if (err != ESP_OK) return err;
    }
    if (!have_mark) return ESP_ERR_INVALID_ARG;
    // Complete an odd symbol with a one-tick idle tail, never a zero field.
    // The native driver appends its own EOF with logical idle level zero.
    if (builder.halves & 1) {
        esp_err_t err = append_segment(&builder, 0, 1);
        if (err != ESP_OK) return err;
    }
    *num_symbols = builder.halves / 2;
    *duration_us = builder.duration_us;
    return ESP_OK;
}
