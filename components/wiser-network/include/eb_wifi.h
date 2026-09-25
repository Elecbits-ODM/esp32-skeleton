/**
 * @file eb_wifi.h
 * @brief WiFi Manager
 *
 * Provides WiFi STA functionality with:
 * - Automatic credential loading from NVS
 * - Event-driven connection management
 * - Reconnection handling
 * - Status callbacks
 *
 * @company Elecbits
 */

#ifndef EB_WIFI_H
#define EB_WIFI_H

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WiFi States
// ============================================================================

typedef enum {
    EB_WIFI_STATE_DISCONNECTED = 0,
    EB_WIFI_STATE_CONNECTING,
    EB_WIFI_STATE_CONNECTED,
    EB_WIFI_STATE_GOT_IP,
    EB_WIFI_STATE_ERROR
} eb_wifi_state_t;

// ============================================================================
// WiFi Status Information
// ============================================================================

typedef struct {
    eb_wifi_state_t state;
    char ssid[33];          // Current/last SSID
    int8_t rssi;            // Signal strength (dBm)
    uint32_t ip_addr;       // IP address (network byte order)
    uint8_t retry_count;    // Connection retry count
    bool credentials_valid; // Whether credentials are stored
} eb_wifi_status_t;

// ============================================================================
// Callbacks
// ============================================================================

/**
 * @brief WiFi state change callback
 * @param old_state Previous state
 * @param new_state Current state
 */
typedef void (*eb_wifi_state_cb_t)(eb_wifi_state_t old_state, eb_wifi_state_t new_state);

/**
 * @brief WiFi IP obtained callback
 * @param ip_addr IP address in network byte order
 */
typedef void (*eb_wifi_ip_cb_t)(uint32_t ip_addr);

// ============================================================================
// Configuration
// ============================================================================

#define EB_WIFI_MAX_RETRY           10      // Max connection retries before giving up
#define EB_WIFI_RETRY_INTERVAL_MS   5000    // Retry interval in milliseconds

// ============================================================================
// API Functions
// ============================================================================

/**
 * @brief Initialize WiFi subsystem
 *
 * Initializes WiFi in STA mode and loads credentials from NVS if available.
 * Does NOT start connection automatically.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t eb_wifi_init(void);

/**
 * @brief Deinitialize WiFi subsystem
 * @return ESP_OK on success
 */
esp_err_t eb_wifi_deinit(void);

/**
 * @brief Start WiFi connection
 *
 * Attempts to connect using credentials stored in NVS.
 * If no credentials are stored, returns ESP_ERR_NOT_FOUND.
 *
 * @return ESP_OK if connection started
 * @return ESP_ERR_NOT_FOUND if no credentials stored
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t eb_wifi_connect(void);

/**
 * @brief Connect with specific credentials
 *
 * Connects using provided credentials. Optionally saves to NVS.
 *
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @param save_to_nvs If true, saves credentials to NVS on successful connection
 * @return ESP_OK if connection started
 */
esp_err_t eb_wifi_connect_with_credentials(const char *ssid, const char *password, bool save_to_nvs);

/**
 * @brief Disconnect from WiFi
 * @return ESP_OK on success
 */
esp_err_t eb_wifi_disconnect(void);

/**
 * @brief Check if WiFi is connected
 * @return true if connected and has IP
 */
bool eb_wifi_is_connected(void);

/**
 * @brief Get current WiFi status
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
esp_err_t eb_wifi_get_status(eb_wifi_status_t *status);

/**
 * @brief Get current WiFi state
 * @return Current WiFi state
 */
eb_wifi_state_t eb_wifi_get_state(void);

/**
 * @brief Get WiFi RSSI
 * @return RSSI in dBm, or 0 if not connected
 */
int8_t eb_wifi_get_rssi(void);

/**
 * @brief Register state change callback
 * @param callback Function to call on state changes
 */
void eb_wifi_set_state_callback(eb_wifi_state_cb_t callback);

/**
 * @brief Register IP obtained callback
 * @param callback Function to call when IP is obtained
 */
void eb_wifi_set_ip_callback(eb_wifi_ip_cb_t callback);

/**
 * @brief Check if credentials are stored in NVS
 * @return true if credentials exist
 */
bool eb_wifi_has_credentials(void);

/**
 * @brief Erase stored WiFi credentials
 * @return ESP_OK on success
 */
esp_err_t eb_wifi_erase_credentials(void);

/**
 * @brief Start WiFi scan
 * @param max_ap Maximum number of APs to scan (1-20)
 * @return ESP_OK if scan started
 */
esp_err_t eb_wifi_scan_start(uint16_t max_ap);

/**
 * @brief Get scan results
 * @param ap_records Array to store AP records
 * @param num_records Pointer to max records (updated with actual count)
 * @return ESP_OK on success
 */
esp_err_t eb_wifi_scan_get_results(wifi_ap_record_t *ap_records, uint16_t *num_records);

// ============================================================================
// TLS Handshake Serialization
// ============================================================================

/** Take before any TLS handshake (cloud or WSS), give immediately after.
 *  Do NOT hold for the lifetime of the connection. */
extern SemaphoreHandle_t g_tls_handshake_mutex;

// ============================================================================
// Connectivity Event Group
// ============================================================================

#define EB_CONN_WIFI_BIT    (1 << 0)   /**< WiFi has an IP address */
#define EB_CONN_AZURE_BIT   (1 << 1)   /**< Azure IoT Hub MQTT connected */

/** Online = both bits set. Offline = WIFI_BIT only. No conn = no bits. */
extern EventGroupHandle_t g_connectivity;

typedef void (*eb_connectivity_cb_t)(bool wifi_up, bool azure_up);

/** Register callback invoked whenever WiFi or Azure connectivity changes. */
void eb_wifi_set_connectivity_callback(eb_connectivity_cb_t cb);

/** Called by the cloud/IoT layer when Hub MQTT connects or disconnects. */
void eb_connectivity_notify_azure(bool up);

/** true when both WIFI_BIT and AZURE_BIT are set. */
bool eb_connectivity_is_online(void);

/** true when WIFI_BIT is set (with or without Azure). */
bool eb_connectivity_is_wifi_up(void);

#ifdef __cplusplus
}
#endif

#endif // EB_WIFI_H
