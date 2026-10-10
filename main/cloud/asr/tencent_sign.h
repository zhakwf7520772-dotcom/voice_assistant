#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TENCENT_ASR_HOST "asr.tencentcloudapi.com"
#define TENCENT_CONTENT_TYPE "application/json; charset=utf-8"

esp_err_t tencent_sign_authorization(const char *payload, size_t payload_len, int64_t timestamp,
                                      char *authorization, size_t authorization_size);
