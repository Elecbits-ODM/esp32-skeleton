/**
 * @file main.c
 * @brief Minimal example: eb_network WiFi connect + BLE provisioning fallback.
 *
 * Demonstrates the real init sequence used by Elecbits products:
 *   1. Initialize NVS storage (eb_storage).
 *   2. Initialize WiFi and register state/connectivity callbacks.
 *   3. If credentials are already stored, connect directly.
 *      Otherwise, start BLE provisioning and wait for the companion app
 *      to deliver WiFi (+ cloud) credentials.
 *
 * Requires the sibling "eb_storage" component to be available via
 * EXTRA_COMPONENT_DIRS (see ../CMakeLists.txt).
 */

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "eb_nvs.h"
#include "eb_wifi.h"
#include "eb_ble_prov.h"

static const char *TAG = "wifi_connect_demo";

// ============================================================================
// Callbacks
// ============================================================================

static void on_wifi_state_changed(eb_wifi_state_t old_state, eb_wifi_state_t new_state)
{
    ESP_LOGI(TAG, "WiFi state changed: %d -> %d", old_state, new_state);

    switch (new_state) {
    case EB_WIFI_STATE_CONNECTING:
        ESP_LOGI(TAG, "Connecting to WiFi...");
        break;
    case EB_WIFI_STATE_GOT_IP:
        ESP_LOGI(TAG, "WiFi connected, IP acquired");
        break;
    case EB_WIFI_STATE_DISCONNECTED:
    case EB_WIFI_STATE_ERROR:
        ESP_LOGW(TAG, "WiFi disconnected / error");
        break;
    default:
        break;
    }
}

static void on_connectivity_changed(bool wifi_up, bool azure_up)
{
    ESP_LOGI(TAG, "Connectivity changed: wifi_up=%d azure_up=%d", wifi_up, azure_up);
}

static void on_ble_prov_state_changed(eb_ble_prov_state_t state)
{
    ESP_LOGI(TAG, "BLE provisioning state changed: %d", state);

    if (state == EB_BLE_PROV_STATE_SUCCESS) {
        ESP_LOGI(TAG, "Provisioning succeeded — restarting to apply credentials");
        esp_restart();
    } else if (state == EB_BLE_PROV_STATE_STOPPED) {
        ESP_LOGW(TAG, "BLE provisioning window timed out");
    }
}

static void on_ble_prov_success(const eb_ble_prov_credentials_t *creds)
{
    ESP_LOGI(TAG, "Received credentials from app: ssid=%s device_id=%s",
             creds->ssid, creds->device_id);
}

// ============================================================================
// app_main
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "eb_network wifi_connect_demo starting...");

    // eb_nvs_init() wraps nvs_flash_init() and prepares the eb_storage
    // namespaces used by eb_wifi / eb_ble_prov.
    eb_nvs_init();

    esp_err_t ret = eb_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "eb_wifi_init failed: %s", esp_err_to_name(ret));
        return;
    }

    eb_wifi_set_state_callback(on_wifi_state_changed);
    eb_wifi_set_connectivity_callback(on_connectivity_changed);

    bool has_creds = eb_wifi_has_credentials();
    if (has_creds) {
        ESP_LOGI(TAG, "Stored WiFi credentials found — connecting directly");
        eb_wifi_connect();
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — starting BLE provisioning");
        eb_ble_prov_set_state_callback(on_ble_prov_state_changed);
        eb_ble_prov_set_success_callback(on_ble_prov_success);
        eb_ble_prov_start(); // reads provisioning status from NVS and advertises
    }
}
