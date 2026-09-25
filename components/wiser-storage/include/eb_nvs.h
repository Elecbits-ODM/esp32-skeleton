/**
 * @file eb_nvs.h
 * @brief NVS Storage Layer
 *
 * Provides persistent storage for:
 * - WiFi credentials
 * - Azure IoT configuration
 * - Device identity
 * - X.509 certificates
 * - Device registry
 *
 * @author Syed S Mashaam (syed.shigarf@elecbits.in)
 * @company Elecbits
 * @date 2025
 */

#ifndef EB_NVS_H
#define EB_NVS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// NVS Namespaces
// ============================================================================

#define EB_NVS_NS_WIFI      "eb_wifi"       // WiFi credentials
#define EB_NVS_NS_AZURE     "eb_azure"      // Azure IoT configuration
#define EB_NVS_NS_CERTS     "eb_certs"      // X.509 certificates
#define EB_NVS_NS_DEVICE    "eb_device"     // Device identity
#define EB_NVS_NS_REGISTRY  "eb_registry"   // Device registry

// ============================================================================
// WiFi Credentials
// ============================================================================

#define EB_WIFI_SSID_MAX_LEN        32
#define EB_WIFI_PASSWORD_MAX_LEN    64

typedef struct {
    char ssid[EB_WIFI_SSID_MAX_LEN + 1];
    char password[EB_WIFI_PASSWORD_MAX_LEN + 1];
    bool configured;
} eb_wifi_credentials_t;

// ============================================================================
// Azure IoT Configuration
// ============================================================================

#define EB_AZURE_SCOPE_ID_MAX_LEN       16
#define EB_AZURE_REG_ID_MAX_LEN         64
#define EB_AZURE_HUB_HOST_MAX_LEN       128
#define EB_AZURE_DEVICE_ID_MAX_LEN      64

typedef struct {
    char scope_id[EB_AZURE_SCOPE_ID_MAX_LEN + 1];       // DPS ID Scope
    char registration_id[EB_AZURE_REG_ID_MAX_LEN + 1];  // DPS Registration ID
    char hub_hostname[EB_AZURE_HUB_HOST_MAX_LEN + 1];   // IoT Hub hostname (cached)
    char device_id[EB_AZURE_DEVICE_ID_MAX_LEN + 1];     // Assigned device ID
    bool provisioned;                                    // DPS provisioning complete
} eb_azure_config_t;

// ============================================================================
// Device Identity
// ============================================================================

#define EB_DEVICE_ID_LEN    13  // 12 hex chars + null

typedef struct {
    char device_id[EB_DEVICE_ID_LEN];   // MAC-based device ID (e.g., "A0764EF40960")
    uint8_t mac[6];                      // Raw MAC address
} eb_device_identity_t;

// ============================================================================
// Certificate Storage (max sizes)
// ============================================================================

#define EB_CERT_MAX_SIZE        2048    // Max PEM certificate size
#define EB_KEY_MAX_SIZE         2048    // Max PEM private key size

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize NVS storage
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_init(void);

/**
 * @brief Erase all NVS storage (factory reset)
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_erase_all(void);

// ============================================================================
// WiFi Credentials API
// ============================================================================

/**
 * @brief Save WiFi credentials to NVS
 * @param creds Pointer to credentials structure
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_wifi_credentials(const eb_wifi_credentials_t *creds);

/**
 * @brief Load WiFi credentials from NVS
 * @param creds Pointer to credentials structure to fill
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t eb_nvs_load_wifi_credentials(eb_wifi_credentials_t *creds);

/**
 * @brief Check if WiFi credentials are stored
 * @return true if credentials exist
 */
bool eb_nvs_wifi_credentials_exist(void);

/**
 * @brief Erase WiFi credentials
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_erase_wifi_credentials(void);

// ============================================================================
// Azure Configuration API
// ============================================================================

/**
 * @brief Save Azure IoT configuration to NVS
 * @param config Pointer to config structure
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_azure_config(const eb_azure_config_t *config);

/**
 * @brief Load Azure IoT configuration from NVS
 * @param config Pointer to config structure to fill
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not configured
 */
esp_err_t eb_nvs_load_azure_config(eb_azure_config_t *config);

/**
 * @brief Update Azure hub hostname (after DPS provisioning)
 * @param hostname IoT Hub hostname
 * @param device_id Assigned device ID
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_update_azure_provisioning(const char *hostname, const char *device_id);

/**
 * @brief Check if Azure is configured
 * @return true if configuration exists
 */
bool eb_nvs_azure_config_exists(void);

/**
 * @brief Erase Azure configuration
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_erase_azure_config(void);

// ============================================================================
// Device Identity API
// ============================================================================

/**
 * @brief Get device identity (MAC-based)
 * @param identity Pointer to identity structure to fill
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_get_device_identity(eb_device_identity_t *identity);

// ============================================================================
// BLE Provisioning NVS API
// ============================================================================

#define EB_NVS_NS_BLE_PROV  "eb_ble_prov"   // BLE provisioning state

/**
 * @brief Get BLE provisioning status from NVS
 * @param status Pointer to store status (0=normal, 1=change-AP, 2=timed)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t eb_nvs_get_prov_status(uint8_t *status);

/**
 * @brief Set BLE provisioning status and timeout
 * @param status Provisioning status (0=normal, 1=change-AP, 2=timed)
 * @param timeout_sec Timeout in seconds (clamped to 60-600)
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_set_prov_status(uint8_t status, uint32_t timeout_sec);

/**
 * @brief Get BLE provisioning timeout
 * @param timeout_sec Pointer to store timeout in seconds (default 180)
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_get_prov_time(uint32_t *timeout_sec);

/**
 * @brief Get BLE pair failure status
 * @param status Pointer to store pair status
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_get_pair_status(uint8_t *status);

/**
 * @brief Set BLE pair failure status
 * @param status Pair status value
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_set_pair_status(uint8_t status);

/**
 * @brief Save OTA URL to NVS
 * @param url OTA firmware URL string
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_ota_url(const char *url);

/**
 * @brief Load OTA URL from NVS
 * @param url Buffer to store URL
 * @param max_len Buffer size
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t eb_nvs_load_ota_url(char *url, size_t max_len);

/**
 * @brief Save provisioned device ID to NVS
 * @param device_id Device ID string from app
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_prov_device_id(const char *device_id);

/**
 * @brief Load provisioned device ID from NVS
 * @param device_id Buffer to store device ID
 * @param max_len Buffer size
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_load_prov_device_id(char *device_id, size_t max_len);

/**
 * @brief Save Azure DPS scope ID to NVS
 * @param scope_id Scope ID string from app
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_scope_id(const char *scope_id);

/**
 * @brief Load Azure DPS scope ID from NVS
 * @param scope_id Buffer to store scope ID
 * @param max_len Buffer size
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_load_scope_id(char *scope_id, size_t max_len);

/**
 * @brief Check if DeviceAddedEvent has been sent
 * @return true if event already sent, false if first boot
 */
bool eb_nvs_is_device_added_event_sent(void);

/**
 * @brief Mark DeviceAddedEvent as sent
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_mark_device_added_event_sent(void);

/**
 * @brief Get persistent restart counter
 * @return Restart count (0 if never set)
 */
uint32_t eb_nvs_get_restart_count(void);

/**
 * @brief Increment persistent restart counter (call once per boot)
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_increment_restart_count(void);

// ============================================================================
// Certificate Storage API
// ============================================================================

/**
 * @brief Save device certificate to NVS
 * @param cert_pem PEM-encoded certificate string
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_device_cert(const char *cert_pem);

/**
 * @brief Load device certificate from NVS
 * @param cert_pem Buffer to store certificate (min EB_CERT_MAX_SIZE)
 * @param max_len Buffer size
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored
 */
esp_err_t eb_nvs_load_device_cert(char *cert_pem, size_t max_len);

/**
 * @brief Save device private key to NVS
 * @param key_pem PEM-encoded private key string
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_device_key(const char *key_pem);

/**
 * @brief Load device private key from NVS
 * @param key_pem Buffer to store key (min EB_KEY_MAX_SIZE)
 * @param max_len Buffer size
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored
 */
esp_err_t eb_nvs_load_device_key(char *key_pem, size_t max_len);

/**
 * @brief Save root CA certificate to NVS
 * @param ca_pem PEM-encoded CA certificate string
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_save_root_ca(const char *ca_pem);

/**
 * @brief Load root CA certificate from NVS
 * @param ca_pem Buffer to store certificate (min EB_CERT_MAX_SIZE)
 * @param max_len Buffer size
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not stored
 */
esp_err_t eb_nvs_load_root_ca(char *ca_pem, size_t max_len);

/**
 * @brief Check if certificates are stored
 * @return true if device cert, key, and root CA all exist
 */
bool eb_nvs_certificates_exist(void);

/**
 * @brief Erase all certificates
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_erase_certificates(void);

// ============================================================================
// Generic Key-Value API (for extensibility)
// ============================================================================

/**
 * @brief Save a string value to NVS
 * @param namespace NVS namespace
 * @param key Key name
 * @param value String value
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_set_string(const char *namespace, const char *key, const char *value);

/**
 * @brief Load a string value from NVS
 * @param namespace NVS namespace
 * @param key Key name
 * @param value Buffer to store value
 * @param max_len Buffer size
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t eb_nvs_get_string(const char *namespace, const char *key, char *value, size_t max_len);

/**
 * @brief Save a blob to NVS
 * @param namespace NVS namespace
 * @param key Key name
 * @param data Blob data
 * @param len Data length
 * @return ESP_OK on success
 */
esp_err_t eb_nvs_set_blob(const char *namespace, const char *key, const void *data, size_t len);

/**
 * @brief Load a blob from NVS
 * @param namespace NVS namespace
 * @param key Key name
 * @param data Buffer to store data
 * @param len Pointer to buffer size (updated with actual size)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t eb_nvs_get_blob(const char *namespace, const char *key, void *data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif // EB_NVS_H
