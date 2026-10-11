#include "wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED BIT0

static const char *TAG = "wifi_sta";
static EventGroupHandle_t s_events;
static esp_err_t s_init_error = ESP_OK;
static bool s_init_attempted;
static bool s_started;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "connecting to configured Wi-Fi");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d; retrying", event ? event->reason : -1);
        xEventGroupClearBits(s_events, WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        if (event) ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_CONNECTED);
    }
}

static esp_err_t wifi_sta_init(void)
{
    s_init_attempted = true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    if ((err = esp_netif_init()) != ESP_OK) return err;
    if ((err = esp_event_loop_create_default()) != ESP_OK) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL)) != ESP_OK) return err;
    wifi_config_t config = { 0 };
    size_t ssid_len = strlen(CONFIG_VOICE_WIFI_SSID);
    size_t pass_len = strlen(CONFIG_VOICE_WIFI_PASSWORD);
    if (ssid_len > sizeof(config.sta.ssid) || pass_len >= sizeof(config.sta.password)) return ESP_ERR_INVALID_ARG;
    memcpy(config.sta.ssid, CONFIG_VOICE_WIFI_SSID, ssid_len);
    memcpy(config.sta.password, CONFIG_VOICE_WIFI_PASSWORD, pass_len);
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    s_started = true;
    return ESP_OK;
}

bool wifi_sta_is_connected(void)
{
    return s_started && s_events && (xEventGroupGetBits(s_events) & WIFI_CONNECTED) != 0;
}

esp_err_t wifi_sta_start(void)
{
    if (s_started) return ESP_OK;
    if (!CONFIG_VOICE_WIFI_SSID[0]) {
        ESP_LOGE(TAG, "Wi-Fi SSID not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_init_attempted) return s_init_error;
    s_init_error = wifi_sta_init();
    if (s_init_error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(s_init_error));
    }
    return s_init_error;
}

esp_err_t wifi_sta_wait_connected(void)
{
    esp_err_t err = wifi_sta_start();
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_VOICE_HTTP_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED)) ESP_LOGE(TAG, "Wi-Fi did not obtain IP within %d ms", CONFIG_VOICE_HTTP_TIMEOUT_MS);
    return (bits & WIFI_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
