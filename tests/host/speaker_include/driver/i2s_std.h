#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef void *i2s_chan_handle_t;
typedef struct {
    int id, role, dma_desc_num, dma_frame_num;
    bool auto_clear_after_cb;
} i2s_chan_config_t;
typedef struct {
    unsigned clk_cfg, slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define I2S_NUM_1 1
#define I2S_ROLE_MASTER 1
#define I2S_GPIO_UNUSED -1
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(num, master) ((i2s_chan_config_t){.id=(num), .role=(master)})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) ((bits) + (mode))
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, void *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, unsigned);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
