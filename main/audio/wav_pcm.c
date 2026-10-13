#include "wav_pcm.h"
#include <stdbool.h>
#include <string.h>

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static esp_err_t need_more(size_t available, size_t total)
{
    return available < total ? ESP_ERR_NOT_FINISHED : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t wav_pcm_parse(const uint8_t *wav, size_t available, size_t total, wav_pcm_info_t *info)
{
    if (!wav || !info || available > total) return ESP_ERR_INVALID_ARG;
    if (total < 44) return ESP_ERR_INVALID_SIZE;
    if (available < 12) return need_more(available, total);
    if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return ESP_ERR_INVALID_RESPONSE;
    uint32_t riff_size = read32(wav + 4);
    if (riff_size < 36 || (size_t)riff_size > total - 8) return ESP_ERR_INVALID_SIZE;
    size_t end = (size_t)riff_size + 8;
    wav_pcm_info_t parsed = { 0 };
    bool format_found = false, data_found = false;
    for (size_t offset = 12; offset + 8 <= end;) {
        if (offset + 8 > available) return need_more(available, total);
        uint32_t chunk_size = read32(wav + offset + 4);
        size_t start = offset + 8;
        if (chunk_size > end - start) return ESP_ERR_INVALID_SIZE;
        if (!memcmp(wav + offset, "fmt ", 4)) {
            if (chunk_size < 16 || format_found) return ESP_ERR_INVALID_RESPONSE;
            if (start + 16 > available) return need_more(available, total);
            const uint8_t *fmt = wav + start;
            if (read16(fmt) != 1) return ESP_ERR_NOT_SUPPORTED;
            parsed.channels = read16(fmt + 2);
            parsed.sample_rate = read32(fmt + 4);
            if (parsed.channels < 1 || parsed.channels > 2 || read16(fmt + 14) != 16 ||
                parsed.sample_rate < 8000 || parsed.sample_rate > 48000 ||
                read16(fmt + 12) != parsed.channels * 2 ||
                read32(fmt + 8) != parsed.sample_rate * parsed.channels * 2)
                return ESP_ERR_NOT_SUPPORTED;
            format_found = true;
        } else if (!memcmp(wav + offset, "data", 4) && !data_found) {
            parsed.data_offset = start;
            parsed.data_bytes = chunk_size;
            data_found = true;
        }
        if (format_found && data_found && available < end) break;
        if ((size_t)chunk_size + (chunk_size & 1) > end - start) {
            if (!(start + chunk_size == end && (chunk_size & 1))) return ESP_ERR_INVALID_SIZE;
            offset = end;
        } else {
            offset = start + chunk_size + (chunk_size & 1);
        }
    }
    if (!format_found || !data_found || !parsed.data_bytes || parsed.data_bytes % (parsed.channels * 2))
        return ESP_ERR_INVALID_RESPONSE;
    *info = parsed;
    return ESP_OK;
}
