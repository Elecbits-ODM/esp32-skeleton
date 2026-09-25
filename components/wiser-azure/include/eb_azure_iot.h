/**
 * @file eb_azure_iot.h
 * @brief Azure IoT Hub Client
 *
 * Provides persistent MQTT connection to Azure IoT Hub with:
 * - X.509 certificate authentication
 * - C2D (Cloud-to-Device) message handling
 * - D2C (Device-to-Cloud) telemetry
 * - Direct method (command) support
 * - Automatic reconnection with exponential backoff
 * - Periodic status heartbeat
 *
 * @author Syed S Mashaam (syed.shigarf@elecbits.in)
 * @company Elecbits
 * @date 2025
 */

#ifndef EB_AZURE_IOT_H
#define EB_AZURE_IOT_H

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration
// ============================================================================

#ifndef CONFIG_EB_AZURE_TELEMETRY_INTERVAL_SEC
#define CONFIG_EB_AZURE_TELEMETRY_INTERVAL_SEC                                 \
  60 // 60 s heartbeat; idle-close = 1.5×240 = 360 s
#endif

#define EB_AZURE_TELEMETRY_INTERVAL_SEC CONFIG_EB_AZURE_TELEMETRY_INTERVAL_SEC
#define EB_AZURE_TASK_STACK_SIZE (10 * 1024)
#define EB_AZURE_TASK_PRIORITY 7
#define EB_AZURE_MQTT_PORT 8883
#define EB_AZURE_KEEPALIVE_SEC 240   // 4 min — matches Azure C SDK default
#define EB_AZURE_MAX_RETRIES 3       // TLS attempts before suspension
#define EB_AZURE_RETRY_DELAY_MS 2000 // delay between each retry
#define EB_AZURE_SUSPEND_SEC 60      // suspension after max retries
#define EB_AZURE_MIN_CONN_SEC                                                  \
  60 // min "good" connection duration (< this = short)
#define EB_AZURE_SHORT_CONN_MAX 3 // short connections before throttle backoff
#define EB_AZURE_THROTTLE_BACKOFF_SEC                                          \
  300 // 5-minute backoff when throttle suspected
// Legacy aliases kept so existing code that references them still compiles
#define EB_AZURE_RECONNECT_MIN_DELAY_MS EB_AZURE_RETRY_DELAY_MS
#define EB_AZURE_RECONNECT_MAX_DELAY_MS (EB_AZURE_SUSPEND_SEC * 1000)

// ============================================================================
// Provisioning Status
// ============================================================================

/**
 * @brief Azure provisioning result
 */
typedef enum {
  EB_AZURE_PROV_PENDING = 0, /**< Provisioning not yet attempted */
  EB_AZURE_PROV_SUCCESS,     /**< DPS + IoT Hub connected successfully */
  EB_AZURE_PROV_DPS_FAILED,  /**< DPS provisioning failed */
  EB_AZURE_PROV_HUB_FAILED,  /**< IoT Hub connection failed after DPS success */
  EB_AZURE_PROV_TIMEOUT,     /**< Provisioning timed out */
} eb_azure_prov_status_t;

// ============================================================================
// Callbacks
// ============================================================================

/**
 * @brief Cloud-to-Device message callback
 * @param msg_type Message type from "msg_type" JSON field
 * @param payload Full JSON payload (caller must NOT free)
 */
typedef void (*eb_azure_c2d_cb_t)(const char *msg_type, const cJSON *payload);

/**
 * @brief Direct method (command) callback
 * @param command_name Method name
 * @param payload JSON payload from cloud
 * @param response Buffer to write JSON response into
 * @param response_len Size of response buffer
 */
typedef void (*eb_azure_command_cb_t)(const char *command_name,
                                      const cJSON *payload, char *response,
                                      size_t response_len);

// ============================================================================
// API Functions
// ============================================================================

/**
 * @brief Initialize Azure IoT module
 *
 * Initializes DPS, but does not connect. Call eb_azure_iot_start() to begin.
 *
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_init(void);

/**
 * @brief Start Azure IoT task
 *
 * Spawns a FreeRTOS task that handles:
 * - DPS provisioning (or NVS cache load)
 * - TLS connection to IoT Hub
 * - MQTT subscribe to C2D, commands, twin properties
 * - Periodic heartbeat telemetry
 * - Auto-reconnect on disconnect
 *
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_start(void);

/**
 * @brief Stop Azure IoT task
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_stop(void);

/**
 * @brief Check if connected to IoT Hub
 * @return true if MQTT connected
 */
bool eb_azure_iot_is_connected(void);

/**
 * @brief Wait for Azure provisioning attempt to complete
 *
 * Blocks until DPS provisioning and initial IoT Hub connection attempt
 * completes (either success or failure). Use this to sequence initialization
 * so that other modules start only after Azure connectivity is determined.
 *
 * @param timeout_ms Maximum time to wait (0 = wait forever)
 * @return Provisioning status result
 */
eb_azure_prov_status_t eb_azure_iot_wait_provisioning(uint32_t timeout_ms);

/**
 * @brief Get current provisioning status (non-blocking)
 * @return Current provisioning status
 */
eb_azure_prov_status_t eb_azure_iot_get_prov_status(void);

// ============================================================================
// D2C (Device to Cloud) - callable from any task
// ============================================================================

/**
 * @brief Send raw JSON telemetry to IoT Hub
 * @param json_payload JSON string to send
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t eb_azure_iot_send_telemetry(const char *json_payload);

/**
 * @brief Send device status heartbeat
 *
 * Sends: msg_type, eb_device_id, online_devices, wifi_rssi,
 *        free_heap, uptime_sec, pipeline_state
 *
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_send_status(void);

/**
 * @brief Send voice command report to IoT Hub
 * @param report cJSON object with command report fields
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_send_voice_command_report(const cJSON *report);

/**
 * @brief Send command acknowledgment (for remote_command C2D)
 * @param request_id Original request ID
 * @param success Whether command executed successfully
 * @param error Error message (NULL if success)
 * @param latency_ms Execution latency in ms
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_send_command_ack(const char *request_id, bool success,
                                        const char *error, uint32_t latency_ms);

/**
 * @brief Send DeviceAddedEvent with comprehensive device information
 *
 * Includes: device_id, model, version, SSID, WiFi RSSI, restart_count,
 *           chip details (name, cores, revision, flash), SDK versions,
 *           free heap, firmware type
 *
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_send_device_added_event(void);

/**
 * @brief Send DeviceDetails event (same payload as DeviceAddedEvent, different
 * event_type)
 *
 * Triggered by cloud via get_device_details C2D command.
 *
 * @return ESP_OK on success
 */
esp_err_t eb_azure_iot_send_device_details(void);

// ============================================================================
// Future Features (TODO Placeholders)
// ============================================================================

// TODO: Implement OTA firmware update support
// esp_err_t eb_azure_ota_update_from_url(const char *url);
// See reference: prov_dev_client_ll_sample.c lines 819-851
// Required features:
// - Download firmware from HTTP URL
// - Progress reporting (0-100%)
// - Automatic rollback on failure (ESP-IDF handles this)
// - Device restart on success

// TODO: Implement cloud command handlers for C2D messages
// Commands to support:
// - restart: Remote device reboot
// - delete: Factory reset and restart
// - OTA: Trigger firmware update
// - get_device_details: Request DeviceAddedEvent resend
// Reference: prov_dev_client_ll_sample.c data_handle_json() lines 666-1004

// TODO: Implement X.509 certificate rollover for expired certs
// Required features:
// - Receive new certificate PEM from cloud
// - Validate certificate chain
// - Store in NVS (replace embedded certs)
// - Reconnect to Azure with new certificate
// Reference: prov_dev_client_ll_sample.c certificate_role_over()
// WARNING: High risk - incorrect implementation can brick device

// ============================================================================
// Callback Registration
// ============================================================================

/**
 * @brief Notify the Azure IoT task of a WiFi connectivity change
 *
 * Must be called from the WiFi state callback whenever the device gains or
 * loses its IP address.  When @p connected is false the task stops attempting
 * TLS connections until WiFi is restored, preventing heap fragmentation from
 * repeated failed handshakes.
 *
 * @param connected true when WiFi has an IP, false when disconnected
 */
void eb_azure_iot_notify_wifi_state(bool connected);

/**
 * @brief Register C2D message callback
 * @param callback Function called on incoming C2D messages
 */
void eb_azure_iot_set_c2d_callback(eb_azure_c2d_cb_t callback);

/**
 * @brief Register direct method callback
 * @param callback Function called on incoming direct methods
 */
void eb_azure_iot_set_command_callback(eb_azure_command_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif // EB_AZURE_IOT_H
