#pragma once
#include <stdint.h>
typedef int gpio_num_t;
// Match the public ESP-IDF rmt_symbol_word_t layout for host logic tests only.
typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;
