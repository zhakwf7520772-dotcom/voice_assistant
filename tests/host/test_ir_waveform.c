#include <assert.h>
#include <stdio.h>

#include "gree_ac.h"
#include "gree_raw_catalog.h"
#include "ir_waveform.h"

static const ir_raw_command_t *selected_command;

esp_err_t ir_transmitter_init(const ir_transmitter_config_t *config)
{
    return config ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t ir_transmitter_replay(const ir_raw_command_t *command)
{
    selected_command = command;
    return ESP_OK;
}

static unsigned level(const rmt_symbol_word_t *symbols, size_t half)
{
    return half & 1 ? symbols[half / 2].level1 : symbols[half / 2].level0;
}

static unsigned duration(const rmt_symbol_word_t *symbols, size_t half)
{
    return half & 1 ? symbols[half / 2].duration1 : symbols[half / 2].duration0;
}

int main(void)
{
    const rmt_symbol_word_t raw[] = {
        { .level0 = 0, .duration0 = 9000, .level1 = 1, .duration1 = 4500 },
        { .level0 = 0, .duration0 = 560, .level1 = 1, .duration1 = 0 },
    };
    const ir_raw_frame_t frames[] = { { raw, 2, 40000 }, { raw, 2, 0 } };
    const ir_raw_command_t command = { frames, 2 };
    rmt_symbol_word_t output[4096];
    size_t count;
    uint64_t total;
    assert(ir_waveform_prepare(&command, output, 4096, &count, &total) == ESP_OK);
    assert(count == 4 && total == 2 * (9000 + 4500 + 560) + 40000);
    assert(level(output, 0) == 1 && duration(output, 0) == 9000);
    assert(level(output, 1) == 0 && duration(output, 1) == 4500);
    assert(level(output, 3) == 0 && duration(output, 3) == 32767);
    assert(level(output, 4) == 0 && duration(output, 4) == 7233);
    assert(level(output, 5) == 1 && duration(output, 5) == 9000);
    assert(ir_waveform_prepare(&command, output, 1, &count, &total) == ESP_ERR_INVALID_SIZE);
    assert(count == 0 && total == 0);

    const ir_raw_frame_t one_frame = { raw, 2, 0 };
    const ir_raw_command_t one = { &one_frame, 1 };
    assert(ir_waveform_prepare(&one, output, 4096, &count, &total) == ESP_OK);
    assert(count == 2 && total == 14061 && output[1].duration1 == 1 && output[1].level1 == 0);
    const rmt_symbol_word_t invalid_raw[] = { { .duration0 = 0, .duration1 = 10 } };
    const ir_raw_frame_t invalid_frame = { invalid_raw, 1, 0 };
    const ir_raw_command_t invalid = { &invalid_frame, 1 };
    assert(ir_waveform_prepare(&invalid, output, 4096, &count, &total) == ESP_ERR_INVALID_ARG);

    // Compare flattened pulse streams from EVERY recorded command against the
    // actual converter output. Preserve levels, split-gap duration and sequence.
    assert(gree_raw_catalog_count == 9);
    for (size_t c = 0; c < gree_raw_catalog_count; ++c) {
        const ir_raw_command_t *input = gree_raw_catalog[c].command;
        assert(ir_waveform_prepare(input, output, 4096, &count, &total) == ESP_OK);
        size_t half = 0;
        uint64_t expected_total = 0;
        for (size_t f = 0; f < input->num_frames; ++f) {
            const ir_raw_frame_t *frame = &input->frames[f];
            for (size_t i = 0; i < frame->num_symbols * 2; ++i) {
                unsigned d = duration(frame->symbols, i);
                if (!d) continue;
                assert(level(output, half) == !level(frame->symbols, i));
                assert(duration(output, half++) == d);
                expected_total += d;
            }
            unsigned gap = frame->gap_after_us;
            while (gap) {
                unsigned chunk = gap > 32767 ? 32767 : gap;
                assert(level(output, half) == 0 && duration(output, half++) == chunk);
                expected_total += chunk;
                gap -= chunk;
            }
        }
        if (half & 1) {
            assert(level(output, half) == 0 && duration(output, half++) == 1);
            ++expected_total;
        }
        assert(half == count * 2 && total == expected_total);
        for (size_t h = 0; h < half; ++h) assert(duration(output, h) > 0);
        const gree_ac_state_t request = {
            gree_raw_catalog[c].power, GREE_COOL,
            gree_raw_catalog[c].temperature_c, gree_raw_catalog[c].fan,
        };
        assert(gree_ac_send_state(&request) == ESP_OK && selected_command == input);
    }
    assert(gree_ac_set_power(true) == ESP_OK);
    assert(gree_ac_set_mode(GREE_COOL) == ESP_OK);
    assert(gree_ac_set_temp(25) == ESP_OK);
    assert(gree_ac_set_fan(3) == ESP_OK);
    assert(gree_ac_send() == ESP_OK);
    assert(gree_ac_set_temp(26) == ESP_OK);
    assert(gree_ac_set_fan(5) == ESP_OK);
    assert(gree_ac_send() == ESP_ERR_NOT_FOUND);
    assert(gree_ac_set_temp(24) == ESP_ERR_NOT_SUPPORTED);
    assert(gree_ac_set_mode((gree_ac_mode_t)2) == ESP_ERR_NOT_SUPPORTED);
    puts("Waveform polarity, EOF, gap splitting, capacity and 9 catalog-state tests passed");
    return 0;
}
