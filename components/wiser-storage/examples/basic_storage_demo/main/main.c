/**
 * @file main.c
 * @brief Minimal example showing eb_storage usage.
 *
 * Initializes NVS, saves and reloads WiFi credentials, then logs the
 * device identity (MAC-derived) and persistent restart count.
 */

#include "eb_nvs.h"
#include "esp_log.h"

static const char *TAG = "basic_storage_demo";

void app_main(void)
{
    ESP_ERROR_CHECK(eb_nvs_init());

    // Save some example WiFi credentials.
    eb_wifi_credentials_t creds_to_save = {
        .ssid = "ExampleNetwork",
        .password = "ExamplePassword123",
        .configured = true,
    };

    esp_err_t err = eb_nvs_save_wifi_credentials(&creds_to_save);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
    }

    // Load them back.
    eb_wifi_credentials_t loaded_creds;
    err = eb_nvs_load_wifi_credentials(&loaded_creds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded WiFi credentials -> SSID: %s", loaded_creds.ssid);
    } else {
        ESP_LOGW(TAG, "No WiFi credentials found: %s", esp_err_to_name(err));
    }

    // Read device identity (MAC-derived device ID).
    eb_device_identity_t identity;
    err = eb_nvs_get_device_identity(&identity);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device identity: %s", identity.device_id);
    } else {
        ESP_LOGE(TAG, "Failed to get device identity: %s", esp_err_to_name(err));
    }

    // Read the persistent restart counter (incremented once per boot by
    // eb_nvs_init()).
    uint32_t restart_count = eb_nvs_get_restart_count();
    ESP_LOGI(TAG, "Restart count: %lu", (unsigned long)restart_count);
}
