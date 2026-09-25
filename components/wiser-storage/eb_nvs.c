/**
 * @file eb_nvs.c
 * @brief NVS Storage Layer Implementation
 */

#include "eb_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "eb_nvs";

// NVS Keys
#define KEY_WIFI_SSID       "ssid"
#define KEY_WIFI_PASS       "password"
#define KEY_WIFI_CONFIGURED "configured"

#define KEY_AZURE_SCOPE     "scope_id"
#define KEY_AZURE_REG_ID    "reg_id"
#define KEY_AZURE_HUB       "hub_host"
#define KEY_AZURE_DEV_ID    "device_id"
#define KEY_AZURE_PROV      "provisioned"

#define KEY_CERT_DEVICE     "dev_cert"
#define KEY_CERT_KEY        "dev_key"
#define KEY_CERT_ROOT_CA    "root_ca"

#define KEY_PROV_STATUS     "prov_st"
#define KEY_PROV_TIME       "prov_time"
#define KEY_PAIR_STATUS     "pair_st"
#define KEY_OTA_URL         "ota_url"
#define KEY_PROV_DEVICE_ID  "prov_dev_id"
#define KEY_PROV_SCOPE_ID   "prov_scope"

#define KEY_DEVICE_ADDED    "dev_added"
#define KEY_RESTART_COUNT   "rst_count"

// ============================================================================
// Initialization
// ============================================================================

esp_err_t eb_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized successfully");
        eb_nvs_increment_restart_count();
    } else {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t eb_nvs_erase_all(void)
{
    ESP_LOGW(TAG, "Erasing all NVS storage (factory reset)");

    esp_err_t ret = nvs_flash_erase();
    if (ret == ESP_OK) {
        ret = nvs_flash_init();
    }

    return ret;
}

// ============================================================================
// WiFi Credentials
// ============================================================================

esp_err_t eb_nvs_save_wifi_credentials(const eb_wifi_credentials_t *creds)
{
    if (!creds) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_WIFI, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", EB_NVS_NS_WIFI, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, KEY_WIFI_SSID, creds->ssid);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(handle, KEY_WIFI_PASS, creds->password);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_u8(handle, KEY_WIFI_CONFIGURED, 1);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved (SSID: %s)", creds->ssid);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_load_wifi_credentials(eb_wifi_credentials_t *creds)
{
    if (!creds) return ESP_ERR_INVALID_ARG;

    memset(creds, 0, sizeof(eb_wifi_credentials_t));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_WIFI, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t configured = 0;
    ret = nvs_get_u8(handle, KEY_WIFI_CONFIGURED, &configured);
    if (ret != ESP_OK || configured == 0) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = sizeof(creds->ssid);
    ret = nvs_get_str(handle, KEY_WIFI_SSID, creds->ssid, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(creds->password);
    ret = nvs_get_str(handle, KEY_WIFI_PASS, creds->password, &len);
    if (ret != ESP_OK) goto cleanup;

    creds->configured = true;
    ESP_LOGI(TAG, "WiFi credentials loaded (SSID: %s)", creds->ssid);

cleanup:
    nvs_close(handle);
    return ret;
}

bool eb_nvs_wifi_credentials_exist(void)
{
    nvs_handle_t handle;
    if (nvs_open(EB_NVS_NS_WIFI, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t configured = 0;
    esp_err_t ret = nvs_get_u8(handle, KEY_WIFI_CONFIGURED, &configured);
    nvs_close(handle);

    return (ret == ESP_OK && configured == 1);
}

esp_err_t eb_nvs_erase_wifi_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_WIFI, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_all(handle);
    ret = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "WiFi credentials erased");
    return ret;
}

// ============================================================================
// Azure Configuration
// ============================================================================

esp_err_t eb_nvs_save_azure_config(const eb_azure_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_AZURE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", EB_NVS_NS_AZURE, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, KEY_AZURE_SCOPE, config->scope_id);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(handle, KEY_AZURE_REG_ID, config->registration_id);
    if (ret != ESP_OK) goto cleanup;

    if (config->hub_hostname[0] != '\0') {
        ret = nvs_set_str(handle, KEY_AZURE_HUB, config->hub_hostname);
        if (ret != ESP_OK) goto cleanup;
    }

    if (config->device_id[0] != '\0') {
        ret = nvs_set_str(handle, KEY_AZURE_DEV_ID, config->device_id);
        if (ret != ESP_OK) goto cleanup;
    }

    ret = nvs_set_u8(handle, KEY_AZURE_PROV, config->provisioned ? 1 : 0);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Azure config saved (scope: %s, reg_id: %s)", config->scope_id, config->registration_id);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_load_azure_config(eb_azure_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    memset(config, 0, sizeof(eb_azure_config_t));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_AZURE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = sizeof(config->scope_id);
    ret = nvs_get_str(handle, KEY_AZURE_SCOPE, config->scope_id, &len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    len = sizeof(config->registration_id);
    nvs_get_str(handle, KEY_AZURE_REG_ID, config->registration_id, &len);

    len = sizeof(config->hub_hostname);
    nvs_get_str(handle, KEY_AZURE_HUB, config->hub_hostname, &len);

    len = sizeof(config->device_id);
    nvs_get_str(handle, KEY_AZURE_DEV_ID, config->device_id, &len);

    uint8_t prov = 0;
    nvs_get_u8(handle, KEY_AZURE_PROV, &prov);
    config->provisioned = (prov == 1);

    nvs_close(handle);

    ESP_LOGI(TAG, "Azure config loaded (scope: %s, provisioned: %d)", config->scope_id, config->provisioned);
    return ESP_OK;
}

esp_err_t eb_nvs_update_azure_provisioning(const char *hostname, const char *device_id)
{
    if (!hostname || !device_id) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_AZURE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, KEY_AZURE_HUB, hostname);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(handle, KEY_AZURE_DEV_ID, device_id);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_u8(handle, KEY_AZURE_PROV, 1);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Azure provisioning updated (hub: %s, device: %s)", hostname, device_id);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

bool eb_nvs_azure_config_exists(void)
{
    nvs_handle_t handle;
    if (nvs_open(EB_NVS_NS_AZURE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    char scope[EB_AZURE_SCOPE_ID_MAX_LEN + 1];
    size_t len = sizeof(scope);
    esp_err_t ret = nvs_get_str(handle, KEY_AZURE_SCOPE, scope, &len);
    nvs_close(handle);

    return (ret == ESP_OK && scope[0] != '\0');
}

esp_err_t eb_nvs_erase_azure_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_AZURE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_all(handle);
    ret = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Azure config erased");
    return ret;
}

// ============================================================================
// Device Identity
// ============================================================================

esp_err_t eb_nvs_get_device_identity(eb_device_identity_t *identity)
{
    if (!identity) return ESP_ERR_INVALID_ARG;

    memset(identity, 0, sizeof(eb_device_identity_t));

    // Get MAC address
    esp_err_t ret = esp_efuse_mac_get_default(identity->mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    // Format as device ID (uppercase hex, no separators)
    snprintf(identity->device_id, sizeof(identity->device_id),
             "%02X%02X%02X%02X%02X%02X",
             identity->mac[0], identity->mac[1], identity->mac[2],
             identity->mac[3], identity->mac[4], identity->mac[5]);

    ESP_LOGI(TAG, "Device identity: %s", identity->device_id);
    return ESP_OK;
}

// ============================================================================
// BLE Provisioning NVS
// ============================================================================

esp_err_t eb_nvs_get_prov_status(uint8_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_BLE_PROV, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    ret = nvs_get_u8(handle, KEY_PROV_STATUS, status);
    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_set_prov_status(uint8_t status, uint32_t timeout_sec)
{
    // Clamp timeout to 60-600 range (matching reference)
    if (timeout_sec < 60) timeout_sec = 60;
    if (timeout_sec > 600) timeout_sec = 600;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_BLE_PROV, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", EB_NVS_NS_BLE_PROV, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u8(handle, KEY_PROV_STATUS, status);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_u32(handle, KEY_PROV_TIME, timeout_sec);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Prov status=%u, timeout=%lu saved", status, (unsigned long)timeout_sec);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_get_prov_time(uint32_t *timeout_sec)
{
    if (!timeout_sec) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_BLE_PROV, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        *timeout_sec = 180;  // Default timeout
        return ESP_OK;
    }

    ret = nvs_get_u32(handle, KEY_PROV_TIME, timeout_sec);
    nvs_close(handle);

    if (ret != ESP_OK) {
        *timeout_sec = 180;  // Default timeout
        ret = ESP_OK;
    }

    return ret;
}

esp_err_t eb_nvs_get_pair_status(uint8_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_BLE_PROV, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    ret = nvs_get_u8(handle, KEY_PAIR_STATUS, status);
    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_set_pair_status(uint8_t status)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_BLE_PROV, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", EB_NVS_NS_BLE_PROV, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u8(handle, KEY_PAIR_STATUS, status);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_save_ota_url(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    return eb_nvs_set_string(EB_NVS_NS_BLE_PROV, KEY_OTA_URL, url);
}

esp_err_t eb_nvs_load_ota_url(char *url, size_t max_len)
{
    if (!url || max_len == 0) return ESP_ERR_INVALID_ARG;
    return eb_nvs_get_string(EB_NVS_NS_BLE_PROV, KEY_OTA_URL, url, max_len);
}

esp_err_t eb_nvs_save_prov_device_id(const char *device_id)
{
    if (!device_id) return ESP_ERR_INVALID_ARG;
    return eb_nvs_set_string(EB_NVS_NS_BLE_PROV, KEY_PROV_DEVICE_ID, device_id);
}

esp_err_t eb_nvs_load_prov_device_id(char *device_id, size_t max_len)
{
    if (!device_id || max_len == 0) return ESP_ERR_INVALID_ARG;
    return eb_nvs_get_string(EB_NVS_NS_BLE_PROV, KEY_PROV_DEVICE_ID, device_id, max_len);
}

esp_err_t eb_nvs_save_scope_id(const char *scope_id)
{
    if (!scope_id) return ESP_ERR_INVALID_ARG;
    return eb_nvs_set_string(EB_NVS_NS_BLE_PROV, KEY_PROV_SCOPE_ID, scope_id);
}

esp_err_t eb_nvs_load_scope_id(char *scope_id, size_t max_len)
{
    if (!scope_id || max_len == 0) return ESP_ERR_INVALID_ARG;
    return eb_nvs_get_string(EB_NVS_NS_BLE_PROV, KEY_PROV_SCOPE_ID, scope_id, max_len);
}

// ============================================================================
// Certificate Storage
// ============================================================================

esp_err_t eb_nvs_save_device_cert(const char *cert_pem)
{
    if (!cert_pem) return ESP_ERR_INVALID_ARG;
    return eb_nvs_set_string(EB_NVS_NS_CERTS, KEY_CERT_DEVICE, cert_pem);
}

esp_err_t eb_nvs_load_device_cert(char *cert_pem, size_t max_len)
{
    if (!cert_pem) return ESP_ERR_INVALID_ARG;
    return eb_nvs_get_string(EB_NVS_NS_CERTS, KEY_CERT_DEVICE, cert_pem, max_len);
}

esp_err_t eb_nvs_save_device_key(const char *key_pem)
{
    if (!key_pem) return ESP_ERR_INVALID_ARG;
    return eb_nvs_set_string(EB_NVS_NS_CERTS, KEY_CERT_KEY, key_pem);
}

esp_err_t eb_nvs_load_device_key(char *key_pem, size_t max_len)
{
    if (!key_pem) return ESP_ERR_INVALID_ARG;
    return eb_nvs_get_string(EB_NVS_NS_CERTS, KEY_CERT_KEY, key_pem, max_len);
}

esp_err_t eb_nvs_save_root_ca(const char *ca_pem)
{
    if (!ca_pem) return ESP_ERR_INVALID_ARG;
    return eb_nvs_set_string(EB_NVS_NS_CERTS, KEY_CERT_ROOT_CA, ca_pem);
}

esp_err_t eb_nvs_load_root_ca(char *ca_pem, size_t max_len)
{
    if (!ca_pem) return ESP_ERR_INVALID_ARG;
    return eb_nvs_get_string(EB_NVS_NS_CERTS, KEY_CERT_ROOT_CA, ca_pem, max_len);
}

bool eb_nvs_certificates_exist(void)
{
    nvs_handle_t handle;
    if (nvs_open(EB_NVS_NS_CERTS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t cert_len = 0, key_len = 0, ca_len = 0;

    nvs_get_str(handle, KEY_CERT_DEVICE, NULL, &cert_len);
    nvs_get_str(handle, KEY_CERT_KEY, NULL, &key_len);
    nvs_get_str(handle, KEY_CERT_ROOT_CA, NULL, &ca_len);

    nvs_close(handle);

    return (cert_len > 1 && key_len > 1 && ca_len > 1);
}

esp_err_t eb_nvs_erase_certificates(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_CERTS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_erase_all(handle);
    ret = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Certificates erased");
    return ret;
}

// ============================================================================
// Generic Key-Value API
// ============================================================================

esp_err_t eb_nvs_set_string(const char *namespace, const char *key, const char *value)
{
    if (!namespace || !key || !value) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", namespace, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_get_string(const char *namespace, const char *key, char *value, size_t max_len)
{
    if (!namespace || !key || !value || max_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(namespace, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = max_len;
    ret = nvs_get_str(handle, key, value, &len);
    nvs_close(handle);

    return ret;
}

esp_err_t eb_nvs_set_blob(const char *namespace, const char *key, const void *data, size_t len)
{
    if (!namespace || !key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", namespace, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, key, data, len);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t eb_nvs_get_blob(const char *namespace, const char *key, void *data, size_t *len)
{
    if (!namespace || !key || !data || !len || *len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(namespace, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    ret = nvs_get_blob(handle, key, data, len);
    nvs_close(handle);

    return ret;
}

// ============================================================================
// DeviceAddedEvent Flag (for Phase 3)
// ============================================================================

bool eb_nvs_is_device_added_event_sent(void)
{
    nvs_handle_t handle;
    if (nvs_open(EB_NVS_NS_DEVICE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t flag = 0;
    nvs_get_u8(handle, KEY_DEVICE_ADDED, &flag);
    nvs_close(handle);

    return (flag == 1);
}

esp_err_t eb_nvs_mark_device_added_event_sent(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_DEVICE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s",
                 EB_NVS_NS_DEVICE, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u8(handle, KEY_DEVICE_ADDED, 1);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "DeviceAddedEvent flag marked as sent");
        }
    }

    nvs_close(handle);
    return ret;
}

uint32_t eb_nvs_get_restart_count(void)
{
    nvs_handle_t handle;
    if (nvs_open(EB_NVS_NS_DEVICE, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }

    uint32_t count = 0;
    nvs_get_u32(handle, KEY_RESTART_COUNT, &count);
    nvs_close(handle);
    return count;
}

esp_err_t eb_nvs_increment_restart_count(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(EB_NVS_NS_DEVICE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t count = 0;
    nvs_get_u32(handle, KEY_RESTART_COUNT, &count);  // ignore error (first boot = 0)
    count++;

    ret = nvs_set_u32(handle, KEY_RESTART_COUNT, count);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Restart count: %lu", (unsigned long)count);
    return ret;
}
