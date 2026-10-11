#include "ir_receiver.h"

#include <inttypes.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define IR_RX_GPIO                 GPIO_NUM_7
#define IR_RX_RESOLUTION_HZ        1000000U  // One tick = one microsecond.
#define IR_RX_HW_SYMBOLS           96U      // Two ESP32-S3 hardware memory blocks.
#define IR_RX_BUFFER_SYMBOLS       2048U
#define IR_RX_BUFFER_COUNT         4U
#define IR_RX_IDLE_NS              30000000U
#define IR_RX_GLITCH_NS            1000U
#define IR_RX_QUEUE_WAIT_MS        1000U
#define IR_RX_RETRY_MS             100U

static const char *TAG = "IR";

typedef struct {
    rmt_rx_done_event_data_t data;
    int64_t completed_us;
} ir_rx_event_t;

typedef struct {
    rmt_symbol_word_t *symbols;
    size_t num_symbols;
    int64_t completed_us;
    uint32_t sequence;
} ir_frame_t;

// Ownership: free queue -> RMT -> frame queue -> logger -> free queue.
// Keep buffers AND ISR queue storage in internal SRAM, even with PSRAM enabled.
// Partial RX is disabled: the driver must finish before we transfer ownership.
static DRAM_ATTR rmt_symbol_word_t s_buffers[IR_RX_BUFFER_COUNT][IR_RX_BUFFER_SYMBOLS];
static DRAM_ATTR StaticQueue_t s_event_queue_control;
static DRAM_ATTR StaticQueue_t s_free_queue_control;
static DRAM_ATTR StaticQueue_t s_frame_queue_control;
static DRAM_ATTR uint8_t s_event_queue_storage[sizeof(ir_rx_event_t)];
static DRAM_ATTR uint8_t s_free_queue_storage[IR_RX_BUFFER_COUNT * sizeof(rmt_symbol_word_t *)];
static DRAM_ATTR uint8_t s_frame_queue_storage[(IR_RX_BUFFER_COUNT - 1) * sizeof(ir_frame_t)];

static QueueHandle_t s_events;
static QueueHandle_t s_free_buffers;
static QueueHandle_t s_frames;
static rmt_channel_handle_t s_channel;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_log_task;
static atomic_uint s_dropped_frames;
static atomic_uint s_invalid_events;
static atomic_uint s_completed_captures;
static atomic_uint s_empty_captures;
static bool s_started;

#if CONFIG_IR_RECEIVER_DIAGNOSTICS
static DRAM_ATTR portMUX_TYPE s_edge_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint32_t s_gpio_edges;
static bool s_gpio_handler_added;
static bool s_gpio_service_owned;

static void IRAM_ATTR ir_gpio_edge(void *arg)
{
    (void)arg;
    // Independent evidence of input activity, including while RMT is unarmed.
    // No logs, allocations, protocol processing or RMT calls in this ISR.
    portENTER_CRITICAL_ISR(&s_edge_lock);
    ++s_gpio_edges;
    portEXIT_CRITICAL_ISR(&s_edge_lock);
}

static void ir_diagnostics_stop(void)
{
    if (s_gpio_handler_added) {
        gpio_isr_handler_remove(IR_RX_GPIO);
        s_gpio_handler_added = false;
    }
    gpio_set_intr_type(IR_RX_GPIO, GPIO_INTR_DISABLE);
    if (s_gpio_service_owned) {
        gpio_uninstall_isr_service();
        s_gpio_service_owned = false;
    }
}

static esp_err_t ir_diagnostics_start(void)
{
    s_gpio_edges = 0;
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err == ESP_OK) s_gpio_service_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) return err;

    // Do not call gpio_config here: it would overwrite the RMT pin routing.
    err = gpio_set_intr_type(IR_RX_GPIO, GPIO_INTR_ANYEDGE);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(IR_RX_GPIO, ir_gpio_edge, NULL);
        if (err == ESP_OK) s_gpio_handler_added = true;
    }
    if (err != ESP_OK) ir_diagnostics_stop();
    return err;
}
#endif

static const rmt_receive_config_t s_receive_config = {
    // The filter uses the peripheral source clock, not the 1 MHz symbol clock.
    // 1 us is within the ESP32-S3 hardware filter range; 50 us would not be.
    .signal_range_min_ns = IR_RX_GLITCH_NS,
    // This is an idle-level threshold, NOT a maximum total frame duration.
    // 30 ms fits the 15-bit duration counter at 1 MHz (maximum 32767 us).
    .signal_range_max_ns = IR_RX_IDLE_NS,
    .flags.en_partial_rx = false,
};

static bool IRAM_ATTR ir_rx_done(rmt_channel_handle_t channel,
                                const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)channel;
    BaseType_t task_woken = pdFALSE;
    const ir_rx_event_t event = {
        // edata is owned by the driver and expires when this callback returns.
        // Copy its metadata by value; the symbols live in our static buffer.
        .data = *edata,
        .completed_us = esp_timer_get_time(),
    };
    // Exactly one final event per armed transaction, and one transaction at a time.
    xQueueOverwriteFromISR((QueueHandle_t)user_ctx, &event, &task_woken);
    return task_woken == pdTRUE;
}

static void ir_return_buffer(rmt_symbol_word_t *buffer)
{
    // Keep queue operations outside configASSERT: assertions can be disabled.
    BaseType_t returned = xQueueSend(s_free_buffers, &buffer, 0);
    configASSERT(returned == pdTRUE);
    (void)returned;
}

static void ir_arm_receive(rmt_symbol_word_t *buffer)
{
    for (;;) {
        esp_err_t err = rmt_receive(s_channel, buffer, sizeof(s_buffers[0]),
                                    &s_receive_config);
        if (err == ESP_OK) return;

        ESP_LOGE(TAG, "[IR] Receive start failed: %s; recovering channel", esp_err_to_name(err));
        err = rmt_disable(s_channel);
        if (err == ESP_OK) {
            // This task is on the core which installed the RMT ISR. Once disabled,
            // discard any obsolete completion before reusing the same buffer.
            xQueueReset(s_events);
            err = rmt_enable(s_channel);
        }
        if (err != ESP_OK) ESP_LOGE(TAG, "[IR] Channel recovery failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(IR_RX_RETRY_MS));
    }
}

static void ir_capture_task(void *arg)
{
    (void)arg;
    rmt_symbol_word_t *active = s_buffers[0];
    uint32_t sequence = 0;
    // Startup only releases us after the channel and both tasks are ready.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (;;) {
        ir_rx_event_t event;
        if (xQueueReceive(s_events, &event, pdMS_TO_TICKS(IR_RX_QUEUE_WAIT_MS)) != pdTRUE) {
            // No button press is a normal timeout. Keep the existing transaction
            // armed: repeatedly restarting would introduce gaps and split frames.
            continue;
        }
        atomic_fetch_add(&s_completed_captures, 1);

        if (!event.data.flags.is_last || event.data.received_symbols != active ||
            event.data.num_symbols > IR_RX_BUFFER_SYMBOLS) {
            atomic_fetch_add(&s_invalid_events, 1);
            ir_arm_receive(active);
            continue;
        }
        if (event.data.num_symbols == 0) {
            atomic_fetch_add(&s_empty_captures, 1);
            ir_arm_receive(active);
            continue;
        }

        ir_frame_t frame = {
            .symbols = active,
            .num_symbols = event.data.num_symbols,
            .completed_us = event.completed_us,
            .sequence = ++sequence,
        };
        rmt_symbol_word_t *next;
        if (xQueueReceive(s_free_buffers, &next, 0) != pdTRUE) {
            // The UART cannot keep up. Drop this newly completed capture, NOT a
            // buffer owned by the logger; keep receiving instead of blocking.
            atomic_fetch_add(&s_dropped_frames, 1);
            ir_arm_receive(active);
            continue;
        }

        active = next;
        // Rearm before handing the old buffer to the potentially slow logger.
        ir_arm_receive(active);
        if (xQueueSend(s_frames, &frame, 0) != pdTRUE) {
            atomic_fetch_add(&s_dropped_frames, 1);
            // With four buffers and a three-frame queue this should not occur.
            ir_return_buffer(frame.symbols);
        }
    }
}

static void ir_log_task(void *arg)
{
    (void)arg;
    unsigned reported_drops = 0;
    unsigned reported_invalid = 0;
#if CONFIG_IR_RECEIVER_DIAGNOSTICS
    uint32_t previous_edges = 0;
    int64_t next_diagnostic_us = esp_timer_get_time() + 5000000;
#endif
    for (;;) {
        ir_frame_t frame;
        if (xQueueReceive(s_frames, &frame, pdMS_TO_TICKS(IR_RX_QUEUE_WAIT_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "[IR] Signal received; capture=%" PRIu32, frame.sequence);
            ESP_LOGI(TAG, "[IR] Receive time (completion, since boot): %" PRId64 "us", frame.completed_us);
            ESP_LOGI(TAG, "[IR] Symbols: %u", (unsigned)frame.num_symbols);
            if (frame.num_symbols == IR_RX_BUFFER_SYMBOLS) {
                // IDF caps the copy count at the user-buffer capacity; there is no
                // RX truncation status in this callback. Treat a full buffer as
                // potentially truncated, never as a guaranteed complete frame.
                ESP_LOGE(TAG, "[IR] Buffer capacity reached; capture may be truncated (%u symbols)",
                         (unsigned)IR_RX_BUFFER_SYMBOLS);
            }
            uint64_t waveform_us = 0;
            for (size_t i = 0; i < frame.num_symbols; ++i) {
                const rmt_symbol_word_t symbol = frame.symbols[i];
                waveform_us += symbol.duration0 + symbol.duration1;
                ESP_LOGI(TAG, "Symbol[%u]: level0=%u duration0=%uus level1=%u duration1=%uus raw=0x%08" PRIx32,
                         (unsigned)i, (unsigned)symbol.level0, (unsigned)symbol.duration0,
                         (unsigned)symbol.level1, (unsigned)symbol.duration1, symbol.val);
                // Yield during a large dump; capture and voice tasks stay runnable.
                if ((i + 1) % 32 == 0) vTaskDelay(1);
            }
            ESP_LOGI(TAG, "[IR] Captured duration: %" PRIu64 "us; end capture=%" PRIu32,
                     waveform_us, frame.sequence);
            // Only now may the capture task reuse this buffer.
            ir_return_buffer(frame.symbols);
        }

        unsigned dropped = atomic_load(&s_dropped_frames);
        unsigned invalid = atomic_load(&s_invalid_events);
        if (dropped != reported_drops) {
            ESP_LOGE(TAG, "[IR] UART backlog: dropped captures=%u (total); space out button presses", dropped);
            reported_drops = dropped;
        }
        if (invalid != reported_invalid) {
            ESP_LOGE(TAG, "[IR] Invalid receive completions=%u (total)", invalid);
            reported_invalid = invalid;
        }
#if CONFIG_IR_RECEIVER_DIAGNOSTICS
        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_diagnostic_us) {
            portENTER_CRITICAL(&s_edge_lock);
            uint32_t edges = s_gpio_edges;
            portEXIT_CRITICAL(&s_edge_lock);
            ESP_LOGI(TAG, "[IR][DIAG] GPIO7=%d edges=%" PRIu32 " (+%" PRIu32 ") rmt_done=%u empty=%u invalid=%u dropped=%u",
                     gpio_get_level(IR_RX_GPIO), edges, edges - previous_edges,
                     atomic_load(&s_completed_captures), atomic_load(&s_empty_captures),
                     invalid, dropped);
            previous_edges = edges;
            next_diagnostic_us = now_us + 5000000;
        }
#endif
    }
}

esp_err_t ir_receiver_start(void)
{
    if (s_started) return ESP_OK;
    bool enabled = false;
    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << IR_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) return err;

    s_events = xQueueCreateStatic(1, sizeof(ir_rx_event_t), s_event_queue_storage, &s_event_queue_control);
    s_free_buffers = xQueueCreateStatic(IR_RX_BUFFER_COUNT, sizeof(rmt_symbol_word_t *),
                                        s_free_queue_storage, &s_free_queue_control);
    s_frames = xQueueCreateStatic(IR_RX_BUFFER_COUNT - 1, sizeof(ir_frame_t),
                                  s_frame_queue_storage, &s_frame_queue_control);
    configASSERT(s_events && s_free_buffers && s_frames);
    atomic_store(&s_dropped_frames, 0);
    atomic_store(&s_invalid_events, 0);
    atomic_store(&s_completed_captures, 0);
    atomic_store(&s_empty_captures, 0);
    for (size_t i = 1; i < IR_RX_BUFFER_COUNT; ++i) {
        rmt_symbol_word_t *buffer = s_buffers[i];
        ir_return_buffer(buffer);
    }

    const rmt_rx_channel_config_t channel_config = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RX_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_HW_SYMBOLS,
        .intr_priority = 0,
        .flags.invert_in = false, // Preserve TSOP polarity: carrier=low, idle=high.
        // Native S3 RX ping-pong moves symbols into the much larger user buffer.
        // No DMA circular-buffer wrapping and no per-pulse heap allocations.
        .flags.with_dma = false,
    };
    err = rmt_new_rx_channel(&channel_config, &s_channel);
    if (err != ESP_OK) goto fail;
    const rmt_rx_event_callbacks_t callbacks = { .on_recv_done = ir_rx_done };
    err = rmt_rx_register_event_callbacks(s_channel, &callbacks, s_events);
    if (err != ESP_OK) goto fail;
    err = rmt_enable(s_channel);
    if (err != ESP_OK) goto fail;
    enabled = true;

#if CONFIG_IR_RECEIVER_DIAGNOSTICS
    err = ir_diagnostics_start();
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "[IR][DIAG] Enabled; idle GPIO7=%d (normally 1); status every 5s",
             gpio_get_level(IR_RX_GPIO));
#endif

    if (xTaskCreate(ir_log_task, "ir_log", 4096, NULL, 2, &s_log_task) != pdPASS ||
        xTaskCreatePinnedToCore(ir_capture_task, "ir_capture", 4096, NULL, 9,
                                &s_capture_task, xPortGetCoreID()) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = rmt_receive(s_channel, s_buffers[0], sizeof(s_buffers[0]), &s_receive_config);
    if (err != ESP_OK) goto fail;
    s_started = true;
    xTaskNotifyGive(s_capture_task);
    ESP_LOGI(TAG, "[IR] Ready: GPIO7, active-low, 1us ticks, idle=%uus, %u buffers x %u symbols",
             (unsigned)(IR_RX_IDLE_NS / 1000), (unsigned)IR_RX_BUFFER_COUNT,
             (unsigned)IR_RX_BUFFER_SYMBOLS);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "[IR] Initialization failed: %s", esp_err_to_name(err));
    if (s_capture_task) { vTaskDelete(s_capture_task); s_capture_task = NULL; }
    if (s_log_task) { vTaskDelete(s_log_task); s_log_task = NULL; }
#if CONFIG_IR_RECEIVER_DIAGNOSTICS
    ir_diagnostics_stop();
#endif
    if (enabled) rmt_disable(s_channel);
    if (s_channel) { rmt_del_channel(s_channel); s_channel = NULL; }
    vQueueDelete(s_events);
    vQueueDelete(s_free_buffers);
    vQueueDelete(s_frames);
    return err;
}
