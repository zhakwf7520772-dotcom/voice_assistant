#include "tencent_sign.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mbedtls/md.h"
#include "tencent_credentials.h"

#define SHA_SIZE 32

static esp_err_t sha256(const void *data, size_t len, unsigned char out[SHA_SIZE])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md && mbedtls_md(md, data, len, out) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t hmac256(const void *key, size_t key_len, const void *data, size_t len,
                         unsigned char out[SHA_SIZE])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md && mbedtls_md_hmac(md, key, key_len, data, len, out) == 0 ? ESP_OK : ESP_FAIL;
}

static void hex_encode(const unsigned char *data, size_t len, char *out)
{
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = alphabet[data[i] >> 4];
        out[i * 2 + 1] = alphabet[data[i] & 0x0f];
    }
    out[len * 2] = 0;
}

esp_err_t tencent_sign_authorization(const char *payload, size_t payload_len, int64_t timestamp,
                                      char *authorization, size_t authorization_size)
{
    if (!payload || !authorization || !TENCENT_SECRET_ID[0] || !TENCENT_SECRET_KEY[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t seconds = (time_t)timestamp;
    struct tm utc;
    if (!gmtime_r(&seconds, &utc)) return ESP_FAIL;
    char date[11];
    if (strftime(date, sizeof(date), "%Y-%m-%d", &utc) != 10) return ESP_FAIL;

    unsigned char digest[SHA_SIZE], secret_date[SHA_SIZE], secret_service[SHA_SIZE], signing_key[SHA_SIZE];
    char payload_hash[SHA_SIZE * 2 + 1], canonical_hash[SHA_SIZE * 2 + 1], signature[SHA_SIZE * 2 + 1];
    if (sha256(payload, payload_len, digest) != ESP_OK) return ESP_FAIL;
    hex_encode(digest, SHA_SIZE, payload_hash);

    char canonical[384];
    int n = snprintf(canonical, sizeof(canonical),
                     "POST\n/\n\ncontent-type:%s\nhost:%s\n\ncontent-type;host\n%s",
                     TENCENT_CONTENT_TYPE, TENCENT_ASR_HOST, payload_hash);
    if (n < 0 || (size_t)n >= sizeof(canonical) || sha256(canonical, n, digest) != ESP_OK) return ESP_FAIL;
    hex_encode(digest, SHA_SIZE, canonical_hash);

    char scope[48], string_to_sign[192];
    n = snprintf(scope, sizeof(scope), "%s/asr/tc3_request", date);
    if (n < 0 || (size_t)n >= sizeof(scope)) return ESP_FAIL;
    n = snprintf(string_to_sign, sizeof(string_to_sign), "TC3-HMAC-SHA256\n%lld\n%s\n%s",
                 (long long)timestamp, scope, canonical_hash);
    if (n < 0 || (size_t)n >= sizeof(string_to_sign)) return ESP_FAIL;

    char initial_key[sizeof(TENCENT_SECRET_KEY) + 3];
    n = snprintf(initial_key, sizeof(initial_key), "TC3%s", TENCENT_SECRET_KEY);
    if (n < 0 || (size_t)n >= sizeof(initial_key)) return ESP_FAIL;
    esp_err_t err = hmac256(initial_key, n, date, strlen(date), secret_date);
    if (err == ESP_OK) err = hmac256(secret_date, SHA_SIZE, "asr", 3, secret_service);
    if (err == ESP_OK) err = hmac256(secret_service, SHA_SIZE, "tc3_request", 11, signing_key);
    if (err == ESP_OK) err = hmac256(signing_key, SHA_SIZE, string_to_sign, strlen(string_to_sign), digest);
    memset(initial_key, 0, sizeof(initial_key));
    memset(secret_date, 0, sizeof(secret_date));
    memset(secret_service, 0, sizeof(secret_service));
    memset(signing_key, 0, sizeof(signing_key));
    if (err != ESP_OK) return err;
    hex_encode(digest, SHA_SIZE, signature);
    n = snprintf(authorization, authorization_size,
                 "TC3-HMAC-SHA256 Credential=%s/%s, SignedHeaders=content-type;host, Signature=%s",
                 TENCENT_SECRET_ID, scope, signature);
    return n < 0 || (size_t)n >= authorization_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
