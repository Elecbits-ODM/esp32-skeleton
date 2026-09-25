/**
 * @file eb_ble_prov.h
 * @brief BLE WiFi Provisioning (Custom Bluedroid GATT Server)
 *
 * Provides BLE-based WiFi provisioning using a custom Bluedroid GATT server
 * compatible with the Wiser mobile app protocol:
 * - BLE Secure Connections pairing
 * - "BLAZE" authentication key
 * - JSON-based credential exchange
 *
 * @company Elecbits
 */

#ifndef EB_BLE_PROV_H
#define EB_BLE_PROV_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BLE Provisioning States
// ============================================================================

typedef enum {
    EB_BLE_PROV_STATE_IDLE = 0,
    EB_BLE_PROV_STATE_STARTING,
    EB_BLE_PROV_STATE_ADVERTISING,
    EB_BLE_PROV_STATE_CONNECTED,
    EB_BLE_PROV_STATE_PAIRED,
    EB_BLE_PROV_STATE_AUTHENTICATED,
    EB_BLE_PROV_STATE_PROVISIONING,
    EB_BLE_PROV_STATE_SUCCESS,
    EB_BLE_PROV_STATE_FAILED,
    EB_BLE_PROV_STATE_STOPPED
} eb_ble_prov_state_t;

// ============================================================================
// Provisioning Status (NVS-persisted)
// ============================================================================

typedef enum {
    EB_PROV_STATUS_NORMAL = 0,      // Normal boot, existing credentials
    EB_PROV_STATUS_CHANGE_AP = 1,   // Change AP requested (first-time / re-prov)
    EB_PROV_STATUS_TIMED = 2        // Timed provisioning window
} eb_prov_status_t;

// ============================================================================
// Provisioning Credentials (received from app)
// ============================================================================

typedef struct {
    char ssid[33];
    char password[65];
    char device_id[40];
    char scope_id[32];
    char ota_url[256];
} eb_ble_prov_credentials_t;

// ============================================================================
// Configuration
// ============================================================================

#define EB_BLE_PROV_DEVICE_NAME_PREFIX  "SE-PRO-EVSO"  // Wiser Switch 16A name (for app compatibility)
#define EB_BLE_PROV_AUTH_KEY            "BLAZE"
#define EB_BLE_PROV_DEFAULT_TIMEOUT_SEC 180

// ============================================================================
// Callbacks
// ============================================================================

/**
 * @brief BLE provisioning state change callback
 * @param state New provisioning state
 */
typedef void (*eb_ble_prov_state_cb_t)(eb_ble_prov_state_t state);

/**
 * @brief BLE provisioning success callback
 * @param creds Pointer to received credentials
 */
typedef void (*eb_ble_prov_success_cb_t)(const eb_ble_prov_credentials_t *creds);

// ============================================================================
// API Functions
// ============================================================================

/**
 * @brief Initialize BLE provisioning (Bluedroid + GATT server)
 *
 * Initializes BT controller, Bluedroid stack, registers GATTS/GAP callbacks,
 * and creates the verify_connection monitoring task.
 *
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_init(void);

/**
 * @brief Deinitialize BLE provisioning
 *
 * Stops advertising, disables Bluedroid and BT controller.
 *
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_deinit(void);

/**
 * @brief Start BLE provisioning with status from NVS
 *
 * Reads provisioning status from NVS and starts accordingly.
 *
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_start(void);

/**
 * @brief Start BLE provisioning with explicit status
 *
 * Starts BLE advertising. Device name format:
 * - Normal/Timed: "SE-VC(AABBCCDDEEFF)"
 * - Change AP:    "CA_SE-VC(DDEEFF)"
 *
 * @param status Provisioning status controlling behavior
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_start_with_status(eb_prov_status_t status);

/**
 * @brief Stop BLE provisioning
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_stop(void);

/**
 * @brief Check if BLE provisioning is active
 * @return true if advertising or connected
 */
bool eb_ble_prov_is_active(void);

/**
 * @brief Get current provisioning state
 * @return Current BLE provisioning state
 */
eb_ble_prov_state_t eb_ble_prov_get_state(void);

/**
 * @brief Get the BLE device name being advertised
 * @param name Buffer to store device name (min 48 bytes)
 * @param len Buffer length
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_get_device_name(char *name, size_t len);

/**
 * @brief Send a notification to the connected BLE client
 * @param data String data to send via Char B notify
 * @return ESP_OK on success
 */
esp_err_t eb_ble_prov_notify_client(const char *data);

/**
 * @brief Register state change callback
 * @param callback Function to call on state changes
 */
void eb_ble_prov_set_state_callback(eb_ble_prov_state_cb_t callback);

/**
 * @brief Register provisioning success callback
 * @param callback Function to call when provisioning succeeds
 */
void eb_ble_prov_set_success_callback(eb_ble_prov_success_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif // EB_BLE_PROV_H
