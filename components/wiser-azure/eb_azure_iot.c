/**
 * @file eb_azure_iot.c
 * @brief Azure IoT Hub Client Implementation
 *
 * Persistent MQTT connection to Azure IoT Hub using
 * azure-iot-middleware-freertos.
 *
 * Task flow:
 * 1. DPS provisioning (or load cached hub from NVS)
 * 2. TLS connect to IoT Hub:8883 with X.509 cert
 * 3. Subscribe to C2D messages, direct methods, device twin
 * 4. Main loop:
 *    a. ProcessLoop() - receive C2D, commands
 *    b. Check heartbeat timer -> send status telemetry
 *    c. If disconnected -> exponential backoff reconnect
 *
 * @author Syed S Mashaam (syed.shigarf@elecbits.in)
 * @company Elecbits
 * @date 2025
 */

#include "eb_azure_iot.h"
#include "eb_azure_dps.h"
#include "eb_nvs.h"
#include "eb_wifi.h"

#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_wifi.h"

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* Azure IoT Middleware */
#include "azure/core/az_version.h"
#include "azure_iot_hub_client.h"
#include "azure_iot_message.h"
#include "azure_iot_transport_interface.h"

#include "cJSON.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "eb_azure_iot";

// ============================================================================
// Embedded Certificates
// ============================================================================

extern const char device_cert_pem_start[] asm("_binary_device_cert_pem_start");
extern const char device_cert_pem_end[] asm("_binary_device_cert_pem_end");
extern const char device_key_pem_start[] asm("_binary_device_key_pem_start");
extern const char device_key_pem_end[] asm("_binary_device_key_pem_end");

// ============================================================================
// Module State
// ============================================================================

static TaskHandle_t s_azure_task = NULL;
static volatile bool s_connected = false;
static volatile bool s_running = false;

// Provisioning synchronization
static EventGroupHandle_t s_prov_event_group = NULL;
static volatile eb_azure_prov_status_t s_prov_status = EB_AZURE_PROV_PENDING;

// Event bits for provisioning status and WiFi readiness
#define PROV_BIT_COMPLETE                                                      \
  (1 << 0) // Provisioning attempt finished (success or fail)
#define WIFI_CONNECTED_BIT                                                     \
  (1 << 1) // WiFi has an IP address. Set by WiFi callback, cleared on
           // disconnect

// Task notification bits for waking azure_iot_task from wait loops
#define AZURE_NOTIFY_WIFI_CHANGED (1UL << 0) // WiFi state changed
#define AZURE_NOTIFY_STOP (1UL << 1)         // Task stop requested

// Callbacks
static eb_azure_c2d_cb_t s_c2d_callback = NULL;
static eb_azure_command_cb_t s_command_callback = NULL;

// Hub connection info (populated by DPS or NVS cache)
static char s_hub_hostname[EB_DPS_HOSTNAME_MAX_LEN];
static char s_device_id[EB_DPS_DEVICE_ID_MAX_LEN];

// MQTT buffer (single buffer for middleware)
#define IOT_MQTT_BUFFER_SIZE 4096
static uint8_t s_mqtt_buffer[IOT_MQTT_BUFFER_SIZE];

// Telemetry message queue for cross-task D2C sending
#define TELEMETRY_QUEUE_SIZE 10
#define TELEMETRY_MSG_MAX_LEN 1024

typedef struct {
  char payload[TELEMETRY_MSG_MAX_LEN];
  size_t len;
} telemetry_msg_t;

static QueueHandle_t s_telemetry_queue = NULL;

// ============================================================================
// TLS Transport (NetworkContext for Azure middleware)
// ============================================================================

typedef struct {
  esp_tls_t *tls_handle;
} TlsTransportCtx_t;

static TlsTransportCtx_t s_tls_ctx;

/* Define struct NetworkContext as required by the transport interface */
struct NetworkContext {
  TlsTransportCtx_t *pParams;
};

static struct NetworkContext s_iot_network_ctx;

static int32_t iot_tls_send(struct NetworkContext *pxNetworkContext,
                            const void *pvBuffer, size_t xBytesToSend) {
  TlsTransportCtx_t *ctx = pxNetworkContext->pParams;
  if (!ctx || !ctx->tls_handle)
    return -1;

  int ret = esp_tls_conn_write(ctx->tls_handle, pvBuffer, xBytesToSend);
  return (ret >= 0) ? ret : -1;
}

static int32_t iot_tls_recv(struct NetworkContext *pxNetworkContext,
                            void *pvBuffer, size_t xBytesToRecv) {
  TlsTransportCtx_t *ctx = pxNetworkContext->pParams;
  if (!ctx || !ctx->tls_handle)
    return -1;

  int ret = esp_tls_conn_read(ctx->tls_handle, pvBuffer, xBytesToRecv);
  if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
    return 0; // No data available yet
  }
  return (ret >= 0) ? ret : -1;
}

static esp_err_t iot_tls_connect(const char *hostname, uint16_t port) {
  esp_tls_cfg_t cfg = {
      .crt_bundle_attach = esp_crt_bundle_attach,
      .clientcert_buf = (const unsigned char *)device_cert_pem_start,
      .clientcert_bytes = device_cert_pem_end - device_cert_pem_start,
      .clientkey_buf = (const unsigned char *)device_key_pem_start,
      .clientkey_bytes = device_key_pem_end - device_key_pem_start,
      .non_block = true,
      .timeout_ms = 15000,
  };

  s_tls_ctx.tls_handle = esp_tls_init();
  if (!s_tls_ctx.tls_handle) {
    ESP_LOGE(TAG, "Failed to allocate esp_tls handle");
    return ESP_ERR_NO_MEM;
  }

  if (g_tls_handshake_mutex)
    xSemaphoreTake(g_tls_handshake_mutex, portMAX_DELAY);

  int ret;
  do {
    ret = esp_tls_conn_new_sync(hostname, strlen(hostname), port, &cfg,
                                s_tls_ctx.tls_handle);
    if (ret == 0) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  } while (ret == 0);

  if (g_tls_handshake_mutex)
    xSemaphoreGive(g_tls_handshake_mutex);

  if (ret < 0) {
    // Classify the failure so the caller can use the right retry strategy:
    //   ESP_ERR_NOT_FOUND  → DNS/network unreachable (no internet route)
    //   ESP_FAIL           → TLS handshake or certificate failure
    //
    // Two signals identify a DNS failure:
    //   1. esp_tls_code == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
    //   2. mbedtls_flags == 0 — means the TLS handshake never started,
    //      so the failure is below the TLS layer (network/DNS).
    esp_err_t classified = ESP_FAIL;
    esp_tls_error_handle_t err_handle = NULL;
    if (esp_tls_get_error_handle(s_tls_ctx.tls_handle, &err_handle) == ESP_OK &&
        err_handle != NULL) {
      int esp_tls_code = 0;
      int mbedtls_flags = 0;
      esp_tls_get_and_clear_last_error(err_handle, &esp_tls_code,
                                       &mbedtls_flags);
      if (esp_tls_code == (int)ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME ||
          (mbedtls_flags == 0 && esp_tls_code != 0 &&
           esp_tls_code != (int)ESP_ERR_NO_MEM)) {
        classified = ESP_ERR_NOT_FOUND; // no internet route
      }
    }
    ESP_LOGE(TAG, "TLS to %s:%d failed (ret=%d)", hostname, port, ret);
    esp_tls_conn_destroy(s_tls_ctx.tls_handle);
    s_tls_ctx.tls_handle = NULL;
    return classified;
  }

  ESP_LOGI(TAG, "TLS connected to %s:%d", hostname, port);
  return ESP_OK;
}

static void iot_tls_disconnect(void) {
  if (s_tls_ctx.tls_handle) {
    esp_tls_conn_destroy(s_tls_ctx.tls_handle);
    s_tls_ctx.tls_handle = NULL;
  }
}

// ============================================================================
// Time Callback (required by middleware - returns uint64_t)
// ============================================================================

static uint64_t get_unix_time(void) { return (uint64_t)time(NULL); }

// Forward declarations for functions used by C2D handler
static esp_err_t send_device_info_event(const char *event_type);
esp_err_t eb_azure_iot_send_device_details(void);

// ============================================================================
// C2D Message Handler
// ============================================================================

// Device added acknowledgment flag (set by cloud via AddAck command)
static volatile bool s_device_added_ack = false;

static void
on_c2d_message(AzureIoTHubClientCloudToDeviceMessageRequest_t *pMessage,
               void *pContext) {
  (void)pContext;

  ESP_LOGD(TAG, "C2D message received (len=%lu)",
           (unsigned long)pMessage->ulPayloadLength);

  // Log raw C2D payload for debugging
  ESP_LOGD(TAG, "C2D raw: %.*s", (int)pMessage->ulPayloadLength,
           (const char *)pMessage->pvMessagePayload);

  // Parse JSON payload
  cJSON *json = cJSON_ParseWithLength((const char *)pMessage->pvMessagePayload,
                                      pMessage->ulPayloadLength);
  if (!json) {
    ESP_LOGE(TAG, "Failed to parse C2D JSON");
    return;
  }

  const cJSON *dev_id_item = cJSON_GetObjectItem(json, "device_id");
  if (cJSON_IsString(dev_id_item) && dev_id_item->valuestring) {
    bool id_match = (strcmp(dev_id_item->valuestring, s_device_id) == 0);

    if (!id_match) {
      // Also check against the BLE-provisioned device_id
      char prov_dev_id[40] = {0};
      if (eb_nvs_load_prov_device_id(prov_dev_id, sizeof(prov_dev_id)) ==
              ESP_OK &&
          prov_dev_id[0] != '\0') {
        id_match = (strcmp(dev_id_item->valuestring, prov_dev_id) == 0);
      }
    }

    if (!id_match) {
      ESP_LOGW(TAG, "C2D device_id mismatch: got '%s', expected '%s'",
               dev_id_item->valuestring, s_device_id);
      cJSON_Delete(json);
      return;
    }
  }

  // Extract cmd_type (reference code uses "cmd_type")
  // Fallback to "msg_type" for EB-custom messages (e.g. device_list_sync)
  const cJSON *cmd_type_item = cJSON_GetObjectItem(json, "cmd_type");
  if (!cJSON_IsString(cmd_type_item)) {
    cmd_type_item = cJSON_GetObjectItem(json, "msg_type");
  }
  const char *cmd_type =
      cJSON_IsString(cmd_type_item) ? cmd_type_item->valuestring : "unknown";

  ESP_LOGI(TAG, "C2D cmd_type: %s", cmd_type);

  // Handle core onboarding commands internally
  if (strcmp(cmd_type, "AddAck") == 0) {
    // Cloud confirms device was added (reference: lines 785-790)
    const cJSON *ack_item = cJSON_GetObjectItem(json, "dev_add_ack");
    if (cJSON_IsNumber(ack_item) && ack_item->valueint == 1) {
      s_device_added_ack = true;
      ESP_LOGI(TAG, "Device addition acknowledged by cloud");
    }
    cJSON_Delete(json);
    return;

  } else if (strcmp(cmd_type, "get_device_details") == 0) {
    // Cloud/app requests device details (reference: lines 926-927)
    ESP_LOGI(TAG,
             "Cloud requested device details, sending DeviceDetails event");
    eb_azure_iot_send_device_details();
    cJSON_Delete(json);
    return;

  } else if (strcmp(cmd_type, "restart") == 0) {
    ESP_LOGW(TAG, "Remote restart requested, rebooting in 3s...");
    cJSON_Delete(json);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return;

  } else if (strcmp(cmd_type, "delete") == 0) {
    // NOTE: Auto-delete disabled during development to prevent infinite
    // erase-restart loop. The cloud is sending delete after DeviceAddedEvent
    // which needs cloud-side investigation.
    ESP_LOGW(TAG, "Remote delete received");
    ESP_LOGW(TAG, "To enable: uncomment eb_nvs_erase_all() + esp_restart()");
    eb_nvs_erase_all();
    vTaskDelay(pdMS_TO_TICKS(1000));
    // esp_restart();
    cJSON_Delete(json);
    return;
  }

  // TODO: Handle these commands in future phases:
  // - "OTA": Trigger firmware update (type: "certificate" or "firmware")
  // - "change_ap": Change WiFi AP credentials
  // - "update_scope_id": Update Azure DPS scope ID
  // - "update_certificates": Certificate rollover
  // Reference: prov_dev_client_ll_sample.c data_handle_json() lines 666-1004

  // Forward unhandled commands to application callback
  if (s_c2d_callback) {
    s_c2d_callback(cmd_type, json);
  } else {
    ESP_LOGW(TAG, "Unhandled C2D cmd_type '%s' (no callback registered)",
             cmd_type);
  }

  cJSON_Delete(json);
}

// ============================================================================
// Direct Method (Command) Handler
// ============================================================================

static void on_direct_method(AzureIoTHubClientCommandRequest_t *pCommand,
                             void *pContext) {
  AzureIoTHubClient_t *hub_client = (AzureIoTHubClient_t *)pContext;

  ESP_LOGI(TAG, "Direct method: %.*s", (int)pCommand->usCommandNameLength,
           pCommand->pucCommandName);

  // Default response
  static char response_buf[256];
  snprintf(response_buf, sizeof(response_buf), "{\"status\":\"ok\"}");
  uint32_t response_status = 200;

  if (s_command_callback) {
    // Parse command payload
    cJSON *json = NULL;
    if (pCommand->ulPayloadLength > 0) {
      json = cJSON_ParseWithLength((const char *)pCommand->pvMessagePayload,
                                   pCommand->ulPayloadLength);
    }

    // Create null-terminated command name
    char cmd_name[64];
    size_t name_len = pCommand->usCommandNameLength < sizeof(cmd_name) - 1
                          ? pCommand->usCommandNameLength
                          : sizeof(cmd_name) - 1;
    memcpy(cmd_name, pCommand->pucCommandName, name_len);
    cmd_name[name_len] = '\0';

    s_command_callback(cmd_name, json, response_buf, sizeof(response_buf));

    if (json)
      cJSON_Delete(json);
  }

  // Send command response back to IoT Hub
  AzureIoTResult_t az_result = AzureIoTHubClient_SendCommandResponse(
      hub_client, pCommand, response_status, (const uint8_t *)response_buf,
      (uint32_t)strlen(response_buf));

  if (az_result != eAzureIoTSuccess) {
    ESP_LOGW(TAG, "Failed to send command response: %d", az_result);
  }
}

// ============================================================================
// Status Heartbeat Builder
// ============================================================================

static char *build_status_json(void) {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return NULL;

  cJSON_AddStringToObject(root, "msg_type", "status");

  // Use BLE-provisioned device_id
  char status_device_id[40] = {0};
  if (eb_nvs_load_prov_device_id(status_device_id, sizeof(status_device_id)) !=
          ESP_OK ||
      status_device_id[0] == '\0') {
    strncpy(status_device_id, s_device_id, sizeof(status_device_id) - 1);
  }
  cJSON_AddStringToObject(root, "eb_device_id", status_device_id);
  cJSON_AddNumberToObject(root, "online_devices",
                          0); // TODO: Phase 6 device registry

  // WiFi RSSI
  wifi_ap_record_t ap_info;
  int8_t rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }
  cJSON_AddNumberToObject(root, "wifi_rssi", rssi);

  cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
  cJSON_AddNumberToObject(root, "uptime_sec",
                          (double)(xTaskGetTickCount() / configTICK_RATE_HZ));
  cJSON_AddStringToObject(root, "pipeline_state",
                          "idle"); // TODO: get actual state

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json_str;
}

// ============================================================================
// Azure IoT Task
// ============================================================================

static void azure_iot_task(void *arg) {
  ESP_LOGI(TAG, "Azure IoT task started");

  // Register this task with the Task Watchdog Timer (WDT)
  esp_task_wdt_add(NULL);

  // Step 1: DPS provisioning (or NVS cache) — retry until success
  esp_err_t err = ESP_FAIL;
  while (s_running) {
    err = eb_azure_dps_provision(s_hub_hostname, sizeof(s_hub_hostname),
                                 s_device_id, sizeof(s_device_id));
    if (err == ESP_OK) break;

    ESP_LOGE(TAG, "DPS provisioning failed: %s. Retrying in 30s...", esp_err_to_name(err));
    s_prov_status = EB_AZURE_PROV_DPS_FAILED;

    // Wait 30s in 5s chunks, resetting WDT — internet may not be ready yet
    for (int i = 0; i < 6 && s_running; i++) {
      esp_task_wdt_reset();
      uint32_t nv = 0;
      xTaskNotifyWait(0, ULONG_MAX, &nv, pdMS_TO_TICKS(5000));
    }
  }

  if (!s_running) {
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
    return;
  }

  if (s_prov_event_group) {
    xEventGroupSetBits(s_prov_event_group, PROV_BIT_COMPLETE);
  }

  uint32_t tls_retry_count =
      0; // consecutive TLS failures (resets on WiFi change or success)
  uint32_t short_conn_count =
      0; // consecutive connections that lasted < EB_AZURE_MIN_CONN_SEC
  bool first_connect_attempt = true;

  while (s_running) {
    // ----------------------------------------------------------------
    // Gate 1: Wait for WiFi before touching the network at all.
    // Clears the retry counter so the first attempt after a reconnect
    // is always tried immediately.
    // ----------------------------------------------------------------
    if (!eb_wifi_is_connected()) {
      ESP_LOGI(TAG, "Waiting for WiFi before connecting to IoT Hub...");
      // Use a bounded wait so we can reset the watchdog while waiting
      while (!s_running)
        break;
      while (s_running) {
        EventBits_t bits = xEventGroupWaitBits(
            s_prov_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(10000)); // 10 s timeout
        esp_task_wdt_reset();
        if (bits & WIFI_CONNECTED_BIT)
          break;
        if (!s_running)
          break;
      }
      if (!s_running)
        break;
      tls_retry_count = 0;
      ESP_LOGI(TAG, "WiFi restored, resuming IoT Hub connection");
    }

    // ----------------------------------------------------------------
    // Step 2b: TLS connect to IoT Hub
    // ----------------------------------------------------------------
    ESP_LOGI(TAG, "Connecting to IoT Hub: %s (attempt %lu/%d)", s_hub_hostname,
             (unsigned long)(tls_retry_count + 1), EB_AZURE_MAX_RETRIES);
    err = iot_tls_connect(s_hub_hostname, EB_AZURE_MQTT_PORT);

    if (err != ESP_OK) {
      if (first_connect_attempt) {
        first_connect_attempt = false;
        s_prov_status = EB_AZURE_PROV_HUB_FAILED;
        if (s_prov_event_group) {
          xEventGroupSetBits(s_prov_event_group, PROV_BIT_COMPLETE);
        }
      }

      if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No internet (DNS failed). Probing in 30 s...");
        for (int i = 0; i < 6 && s_running; i++) {
          esp_task_wdt_reset();
          uint32_t nv = 0;
          xTaskNotifyWait(0, ULONG_MAX, &nv, pdMS_TO_TICKS(5000));
        }
        tls_retry_count = 0; // restart retry budget once internet probe fires
      } else {
        tls_retry_count++;

        if (tls_retry_count >= EB_AZURE_MAX_RETRIES) {
          ESP_LOGW(TAG,
                   "IoT Hub TLS failed %lu/%d times. "
                   "Suspending for %d s (heap free: %lu).",
                   (unsigned long)tls_retry_count, EB_AZURE_MAX_RETRIES,
                   EB_AZURE_SUSPEND_SEC,
                   (unsigned long)esp_get_free_heap_size());
          tls_retry_count = 0;
          // Sleep in 5s chunks, resetting WDT each iteration
          int chunks = (EB_AZURE_SUSPEND_SEC * 1000) / 5000;
          for (int i = 0; i < chunks && s_running; i++) {
            esp_task_wdt_reset();
            uint32_t nv = 0;
            xTaskNotifyWait(0, ULONG_MAX, &nv, pdMS_TO_TICKS(5000));
          }
        } else {
          ESP_LOGW(TAG,
                   "Hub TLS connect failed (attempt %lu/%d). "
                   "Retry in %d ms.",
                   (unsigned long)tls_retry_count, EB_AZURE_MAX_RETRIES,
                   EB_AZURE_RETRY_DELAY_MS);
          vTaskDelay(pdMS_TO_TICKS(EB_AZURE_RETRY_DELAY_MS));
        }
      }
      esp_task_wdt_reset();
      continue;
    }

    // TLS succeeded — reset the retry counter for the next disconnect cycle.
    tls_retry_count = 0;

    // Set up transport interface (Azure middleware transport)
    s_iot_network_ctx.pParams = &s_tls_ctx;

    AzureIoTTransportInterface_t transport = {
        .xSend = iot_tls_send,
        .xRecv = iot_tls_recv,
        .pxNetworkContext = &s_iot_network_ctx,
    };

    // Step 3: Initialize IoT Hub client
    AzureIoTHubClient_t hub_client;
    AzureIoTResult_t az_result;

    AzureIoTHubClientOptions_t options;
    az_result = AzureIoTHubClient_OptionsInit(&options);
    if (az_result != eAzureIoTSuccess) {
      ESP_LOGE(TAG, "Hub client options init failed: %d", az_result);
      iot_tls_disconnect();
      vTaskDelay(pdMS_TO_TICKS(EB_AZURE_RETRY_DELAY_MS));
      continue;
    }

    az_result = AzureIoTHubClient_Init(
        &hub_client, (const uint8_t *)s_hub_hostname,
        (uint16_t)strlen(s_hub_hostname), (const uint8_t *)s_device_id,
        (uint16_t)strlen(s_device_id), &options, s_mqtt_buffer,
        sizeof(s_mqtt_buffer), get_unix_time, &transport);

    if (az_result != eAzureIoTSuccess) {
      ESP_LOGE(TAG, "Hub client init failed: %d", az_result);
      iot_tls_disconnect();
      vTaskDelay(pdMS_TO_TICKS(EB_AZURE_RETRY_DELAY_MS));
      continue;
    }

    // Connect MQTT (X.509 auth happens via TLS client cert)
    bool session_present = false;
    az_result = AzureIoTHubClient_Connect(&hub_client, false, &session_present,
                                          EB_AZURE_KEEPALIVE_SEC * 1000);
    if (az_result != eAzureIoTSuccess) {
      ESP_LOGE(TAG, "MQTT connect failed: %d", az_result);

      // Signal failure on first attempt so other modules can proceed
      if (first_connect_attempt) {
        first_connect_attempt = false;
        s_prov_status = EB_AZURE_PROV_HUB_FAILED;
        if (s_prov_event_group) {
          xEventGroupSetBits(s_prov_event_group, PROV_BIT_COMPLETE);
        }
      }

      AzureIoTHubClient_Deinit(&hub_client);
      iot_tls_disconnect();
      vTaskDelay(pdMS_TO_TICKS(EB_AZURE_RETRY_DELAY_MS));
      continue;
    }

    ESP_LOGI(TAG, "Connected to IoT Hub: %s (device: %s)", s_hub_hostname,
             s_device_id);
    s_connected = true;
    eb_connectivity_notify_azure(true);
    TickType_t conn_start_ticks = xTaskGetTickCount();

    // Signal provisioning success on first successful connection
    if (first_connect_attempt) {
      first_connect_attempt = false;
      s_prov_status = EB_AZURE_PROV_SUCCESS;
      if (s_prov_event_group) {
        xEventGroupSetBits(s_prov_event_group, PROV_BIT_COMPLETE);
      }

      // Send DeviceAddedEvent on first boot after provisioning
      if (!eb_nvs_is_device_added_event_sent()) {
        ESP_LOGI(
            TAG,
            "First connection after provisioning - sending DeviceAddedEvent");
        vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2s for connection to stabilize

        if (eb_azure_iot_send_device_added_event() == ESP_OK) {
          eb_nvs_mark_device_added_event_sent();
          ESP_LOGI(TAG, "DeviceAddedEvent sent and marked");
        } else {
          ESP_LOGW(TAG,
                   "Failed to send DeviceAddedEvent - will retry on next boot");
        }
      }
    }

    // Step 4: Subscribe to C2D, commands
    az_result = AzureIoTHubClient_SubscribeCloudToDeviceMessage(
        &hub_client, on_c2d_message, NULL, 10000);
    if (az_result != eAzureIoTSuccess) {
      ESP_LOGW(TAG, "C2D subscribe failed: %d", az_result);
    }

    // Pass &hub_client as context so command handler can send responses
    az_result = AzureIoTHubClient_SubscribeCommand(
        &hub_client, on_direct_method, &hub_client, 10000);
    if (az_result != eAzureIoTSuccess) {
      ESP_LOGW(TAG, "Command subscribe failed: %d", az_result);
    }

    {
      char *init_json = build_status_json();
      if (init_json) {
        uint16_t pkt_id = 0;
        az_result = AzureIoTHubClient_SendTelemetry(
            &hub_client, (const uint8_t *)init_json,
            (uint32_t)strlen(init_json), NULL, eAzureIoTHubMessageQoS1,
            &pkt_id);
        if (az_result == eAzureIoTSuccess) {
          ESP_LOGI(TAG, "Initial status sent (pkt=%u) — Hub idle timer reset",
                   pkt_id);
          // One pass to exchange PUBACK before entering the main loop.
          AzureIoTHubClient_ProcessLoop(&hub_client, 100);
        } else {
          ESP_LOGW(TAG, "Initial status send failed: %d", az_result);
        }
        free(init_json);
      }
    }

    // Step 5: Main loop
    TickType_t last_heartbeat = xTaskGetTickCount();
    TickType_t heartbeat_interval =
        pdMS_TO_TICKS(EB_AZURE_TELEMETRY_INTERVAL_SEC * 1000);

    while (s_running && s_connected) {
      esp_task_wdt_reset();
      if (!eb_wifi_is_connected()) {
        ESP_LOGW(TAG, "WiFi lost while connected to IoT Hub, disconnecting");
        s_connected = false;
        eb_connectivity_notify_azure(false);
        break;
      }

      az_result = AzureIoTHubClient_ProcessLoop(&hub_client, 10);
      if (az_result != eAzureIoTSuccess) {
        ESP_LOGW(TAG, "ProcessLoop error: %d", az_result);
        s_connected = false;
        eb_connectivity_notify_azure(false);
        break;
      }

      // Process queued telemetry messages (D2C)
      telemetry_msg_t queued_msg;
      while (xQueueReceive(s_telemetry_queue, &queued_msg, 0) == pdTRUE) {
        // Set content-type and encoding properties (required for Azure message
        // routing)
        AzureIoTMessageProperties_t xProperties;
        uint8_t ucPropertyBuf[128];

        AzureIoTResult_t xPropResult = AzureIoTMessage_PropertiesInit(
            &xProperties, ucPropertyBuf, 0, sizeof(ucPropertyBuf));

        AzureIoTMessageProperties_t *pxProps = NULL;
        if (xPropResult == eAzureIoTSuccess) {
          AzureIoTMessage_PropertiesAppend(
              &xProperties,
              (const uint8_t *)AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE,
              sizeof(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE) - 1,
              (const uint8_t *)"application%2Fjson",
              sizeof("application%2Fjson") - 1);

          AzureIoTMessage_PropertiesAppend(
              &xProperties,
              (const uint8_t *)AZ_IOT_MESSAGE_PROPERTIES_CONTENT_ENCODING,
              sizeof(AZ_IOT_MESSAGE_PROPERTIES_CONTENT_ENCODING) - 1,
              (const uint8_t *)"utf-8", sizeof("utf-8") - 1);

          pxProps = &xProperties;
        }

        uint16_t packet_id = 0;
        az_result = AzureIoTHubClient_SendTelemetry(
            &hub_client, (const uint8_t *)queued_msg.payload,
            (uint32_t)queued_msg.len, pxProps, eAzureIoTHubMessageQoS1,
            &packet_id);

        if (az_result != eAzureIoTSuccess) {
          ESP_LOGE(TAG, "Failed to send telemetry: %d", az_result);
        } else {
          ESP_LOGD(TAG, "Telemetry sent successfully (pkt=%u)", packet_id);
        }
      }

      // Heartbeat telemetry
      TickType_t now = xTaskGetTickCount();
      if ((now - last_heartbeat) >= heartbeat_interval) {
        char *status_json = build_status_json();
        if (status_json) {
          uint16_t packet_id = 0;
          az_result = AzureIoTHubClient_SendTelemetry(
              &hub_client, (const uint8_t *)status_json,
              (uint32_t)strlen(status_json), NULL, eAzureIoTHubMessageQoS1,
              &packet_id);

          if (az_result == eAzureIoTSuccess) {
            ESP_LOGD(TAG, "Heartbeat sent (pkt=%u)", packet_id);
          } else {
            ESP_LOGW(TAG, "Heartbeat send failed: %d", az_result);
          }

          free(status_json);
        }
        last_heartbeat = now;
      }

      // Yield and reset watchdog to prevent trigger
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Disconnect
    ESP_LOGI(TAG, "Disconnecting from IoT Hub...");
    s_connected = false;
    eb_connectivity_notify_azure(false);
    AzureIoTHubClient_Disconnect(&hub_client);
    AzureIoTHubClient_Deinit(&hub_client);
    iot_tls_disconnect();

    // ----------------------------------------------------------------
    // Short-connection throttle detection.
    // If the Hub drops us repeatedly within EB_AZURE_MIN_CONN_SEC
    // it is very likely applying connection-flood throttling.  After
    // EB_AZURE_SHORT_CONN_MAX such events, back off for
    // EB_AZURE_THROTTLE_BACKOFF_SEC so the Hub's throttle window
    // can expire before we attempt to reconnect again.
    // ----------------------------------------------------------------
    TickType_t conn_duration_ticks = xTaskGetTickCount() - conn_start_ticks;
    uint32_t conn_duration_sec =
        (uint32_t)(conn_duration_ticks / configTICK_RATE_HZ);

    if (conn_duration_sec < EB_AZURE_MIN_CONN_SEC) {
      short_conn_count++;
      ESP_LOGW(TAG, "Short connection: %lu s (count %lu/%d)",
               (unsigned long)conn_duration_sec,
               (unsigned long)short_conn_count, EB_AZURE_SHORT_CONN_MAX);

      if (short_conn_count >= EB_AZURE_SHORT_CONN_MAX) {
        short_conn_count = 0;
        ESP_LOGW(TAG, "Hub throttle suspected. Backing off for %d s.",
                 EB_AZURE_THROTTLE_BACKOFF_SEC);
        {
          uint32_t nv = 0;
          xTaskNotifyWait(0, ULONG_MAX, &nv,
                          pdMS_TO_TICKS(EB_AZURE_THROTTLE_BACKOFF_SEC * 1000));
        }
      }
    } else {
      short_conn_count = 0; // good connection — reset the counter
    }

    // Retry counter stays at 0 here (was reset on TLS success above).
    // The next iteration re-checks WiFi and attempts TLS immediately.
  }

  ESP_LOGI(TAG, "Azure IoT task stopped");
  s_azure_task = NULL;
  esp_task_wdt_delete(NULL);
  vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t eb_azure_iot_init(void) {
  // Initialize DPS module
  esp_err_t err = eb_azure_dps_init();
  if (err != ESP_OK) {
    return err;
  }

  // Create provisioning event group for synchronization
  if (!s_prov_event_group) {
    s_prov_event_group = xEventGroupCreate();
    if (!s_prov_event_group) {
      ESP_LOGE(TAG, "Failed to create provisioning event group");
      return ESP_ERR_NO_MEM;
    }
  }

  // Create telemetry queue for cross-task D2C message sending
  if (!s_telemetry_queue) {
    s_telemetry_queue =
        xQueueCreate(TELEMETRY_QUEUE_SIZE, sizeof(telemetry_msg_t));
    if (!s_telemetry_queue) {
      ESP_LOGE(TAG, "Failed to create telemetry queue");
      return ESP_ERR_NO_MEM;
    }
  }

  s_connected = false;
  s_running = false;
  s_prov_status = EB_AZURE_PROV_PENDING;
  memset(s_hub_hostname, 0, sizeof(s_hub_hostname));
  memset(s_device_id, 0, sizeof(s_device_id));

  // Check if WiFi is already connected and set event bit so task can proceed
  // immediately
  if (eb_wifi_is_connected()) {
    xEventGroupSetBits(s_prov_event_group, WIFI_CONNECTED_BIT);
  }

  ESP_LOGI(TAG, "Azure IoT module initialized");
  return ESP_OK;
}

esp_err_t eb_azure_iot_start(void) {
  if (s_azure_task) {
    ESP_LOGW(TAG, "Azure IoT task already running");
    return ESP_ERR_INVALID_STATE;
  }

  s_running = true;

  BaseType_t ret =
      xTaskCreate(azure_iot_task, "azure_iot", EB_AZURE_TASK_STACK_SIZE, NULL,
                  EB_AZURE_TASK_PRIORITY, &s_azure_task);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Azure IoT task");
    s_running = false;
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Azure IoT task started (stack=%d, prio=%d)",
           EB_AZURE_TASK_STACK_SIZE, EB_AZURE_TASK_PRIORITY);
  return ESP_OK;
}

esp_err_t eb_azure_iot_stop(void) {
  if (!s_running) {
    return ESP_OK;
  }

  s_running = false;
  s_connected = false;

  // Wake the task from any xTaskNotifyWait sleep so it exits promptly
  if (s_azure_task != NULL) {
    xTaskNotify(s_azure_task, AZURE_NOTIFY_STOP, eSetBits);
  }

  // Task will self-terminate on next loop iteration
  // Wait for cleanup
  int timeout = 50; // 5 seconds
  while (s_azure_task && timeout > 0) {
    vTaskDelay(pdMS_TO_TICKS(100));
    timeout--;
  }

  if (s_azure_task) {
    ESP_LOGW(TAG, "Azure task didn't stop cleanly, deleting");
    vTaskDelete(s_azure_task);
    s_azure_task = NULL;
  }

  iot_tls_disconnect();
  ESP_LOGI(TAG, "Azure IoT stopped");
  return ESP_OK;
}

bool eb_azure_iot_is_connected(void) { return s_connected; }

eb_azure_prov_status_t eb_azure_iot_wait_provisioning(uint32_t timeout_ms) {
  if (!s_prov_event_group) {
    ESP_LOGE(TAG, "Azure IoT not initialized");
    return EB_AZURE_PROV_DPS_FAILED;
  }

  // If already complete, return immediately
  if (s_prov_status != EB_AZURE_PROV_PENDING) {
    return s_prov_status;
  }

  ESP_LOGD(TAG, "Waiting for Azure provisioning to complete...");

  TickType_t wait_ticks =
      (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

  EventBits_t bits =
      xEventGroupWaitBits(s_prov_event_group, PROV_BIT_COMPLETE,
                          pdFALSE, // Don't clear bits
                          pdTRUE,  // Wait for all bits (just one in this case)
                          wait_ticks);

  if (bits & PROV_BIT_COMPLETE) {
    ESP_LOGI(TAG, "Azure provisioning completed with status: %d",
             s_prov_status);
    return s_prov_status;
  }

  // Timeout
  ESP_LOGW(TAG, "Azure provisioning wait timed out");
  return EB_AZURE_PROV_TIMEOUT;
}

eb_azure_prov_status_t eb_azure_iot_get_prov_status(void) {
  return s_prov_status;
}

esp_err_t eb_azure_iot_send_telemetry(const char *json_payload) {
  if (!json_payload) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t len = strlen(json_payload);
  if (len >= TELEMETRY_MSG_MAX_LEN) {
    ESP_LOGE(TAG, "Telemetry payload too large: %zu bytes", len);
    return ESP_ERR_INVALID_SIZE;
  }

  if (!s_telemetry_queue) {
    ESP_LOGE(TAG, "Telemetry queue not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  telemetry_msg_t msg = {0};
  strncpy(msg.payload, json_payload, sizeof(msg.payload) - 1);
  msg.len = len;

  if (xQueueSend(s_telemetry_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Telemetry queue full, dropping message");
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(TAG, "Telemetry queued: %.64s%s", json_payload,
           len > 64 ? "..." : "");
  return ESP_OK;
}

esp_err_t eb_azure_iot_send_status(void) {
  char *json = build_status_json();
  if (!json) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = eb_azure_iot_send_telemetry(json);
  free(json);
  return err;
}

esp_err_t eb_azure_iot_send_voice_command_report(const cJSON *report) {
  if (!report)
    return ESP_ERR_INVALID_ARG;

  char *json = cJSON_PrintUnformatted(report);
  if (!json)
    return ESP_ERR_NO_MEM;

  esp_err_t err = eb_azure_iot_send_telemetry(json);
  free(json);
  return err;
}

esp_err_t eb_azure_iot_send_command_ack(const char *request_id, bool success,
                                        const char *error,
                                        uint32_t latency_ms) {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return ESP_ERR_NO_MEM;

  cJSON_AddStringToObject(root, "msg_type", "command_ack");
  cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
  cJSON_AddBoolToObject(root, "success", success);
  if (error) {
    cJSON_AddStringToObject(root, "error", error);
  } else {
    cJSON_AddNullToObject(root, "error");
  }
  cJSON_AddNumberToObject(root, "latency_ms", (double)latency_ms);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!json)
    return ESP_ERR_NO_MEM;

  esp_err_t err = eb_azure_iot_send_telemetry(json);
  free(json);
  return err;
}

// Internal helper: maps esp_reset_reason() to a human-readable string
static const char *get_restart_reason_str(void) {
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON:
    return "PowerOn-Reset";
  case ESP_RST_EXT:
    return "External-Reset";
  case ESP_RST_SW:
    return "Software-Reset";
  case ESP_RST_PANIC:
    return "Panic-Reset";
  case ESP_RST_INT_WDT:
    return "IntWDT-Reset";
  case ESP_RST_TASK_WDT:
    return "TaskWDT-Reset";
  case ESP_RST_WDT:
    return "WDT-Reset";
  case ESP_RST_DEEPSLEEP:
    return "DeepSleep-Reset";
  case ESP_RST_BROWNOUT:
    return "Brownout-Reset";
  case ESP_RST_SDIO:
    return "SDIO-Reset";
  default:
    return "Unknown-Reset";
  }
}

// Internal helper: builds and sends device info event with specified event_type
static esp_err_t send_device_info_event(const char *event_type) {
  ESP_LOGD(TAG, "Building %s...", event_type);

  // Get provisioned device ID.
  // provisioned=true means the app assigned this device an ID via the
  // provisioning flow (e.g. BLE provisioning).
  char prov_device_id[40] = {0};
  bool ble_provisioned = false;
  esp_err_t ret =
      eb_nvs_load_prov_device_id(prov_device_id, sizeof(prov_device_id));
  if (ret == ESP_OK && prov_device_id[0] != '\0') {
    ble_provisioned = true;
  } else {
    // Fallback: use MAC-based identity if provisioning hasn't run yet
    ESP_LOGW(
        TAG,
        "Provisioned device_id not found, falling back to MAC identity");
    eb_device_identity_t identity;
    if (eb_nvs_get_device_identity(&identity) == ESP_OK) {
      strncpy(prov_device_id, identity.device_id, sizeof(prov_device_id) - 1);
    } else {
      ESP_LOGE(TAG, "Failed to get any device identity");
      return ESP_FAIL;
    }
  }
  ESP_LOGI(TAG, "Using device_id: %s (ble_provisioned=%d)", prov_device_id,
           ble_provisioned);

  // Get WiFi credentials for SSID
  eb_wifi_credentials_t wifi_creds = {0};
  ret = eb_nvs_load_wifi_credentials(&wifi_creds);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to load WiFi credentials: %s", esp_err_to_name(ret));
    strncpy(wifi_creds.ssid, "unknown", sizeof(wifi_creds.ssid) - 1);
  }

  // Get WiFi RSSI
  wifi_ap_record_t ap_info;
  int8_t rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }

  // ble_set: 1 if device was provisioned via BLE (has app-assigned device_id),
  // else 0. Do NOT use eb_nvs_get_prov_status() — it resets to 0 after
  // successful provisioning.
  uint8_t ble_prov_status = ble_provisioned ? 1 : 0;

  // Build JSON event
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "Failed to create JSON object");
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddStringToObject(root, "event_type", event_type);
  cJSON_AddStringToObject(root, "device_id", prov_device_id);
  cJSON_AddStringToObject(root, "ssid", wifi_creds.ssid);

// Device model and version from Kconfig (with defaults if not set)
#ifndef CONFIG_EB_DEVICE_MODEL
#define CONFIG_EB_DEVICE_MODEL "EB-DEVICE-001"
#endif
#ifndef CONFIG_EB_FIRMWARE_VERSION
#define CONFIG_EB_FIRMWARE_VERSION "1.0.0"
#endif

  cJSON_AddStringToObject(root, "model", CONFIG_EB_DEVICE_MODEL);
  cJSON_AddStringToObject(root, "version", CONFIG_EB_FIRMWARE_VERSION);

  // signal_strength as "<value>dBm" string to match reference code format
  char rssi_str[12];
  snprintf(rssi_str, sizeof(rssi_str), "%d", rssi);
  cJSON_AddStringToObject(root, "signal_strength", rssi_str);

  // restart_count -> azure_sdk_ver -> azure_c_sdk_ver
  cJSON_AddNumberToObject(root, "restart_count",
                          (double)eb_nvs_get_restart_count());

#ifndef CONFIG_EB_AZURE_SDK_VERSION
#define CONFIG_EB_AZURE_SDK_VERSION "1.1.0"
#endif
  cJSON_AddStringToObject(root, "azure_sdk_ver", CONFIG_EB_AZURE_SDK_VERSION);
  cJSON_AddStringToObject(root, "azure_c_sdk_ver", AZ_SDK_VERSION_STRING);

  cJSON_AddNumberToObject(root, "ble_set", ble_prov_status);

  // Chip information
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  const char *chip_name = "Unknown";
  switch (chip_info.model) {
  case CHIP_ESP32:
    chip_name = "ESP32";
    break;
  case CHIP_ESP32S2:
    chip_name = "ESP32S2";
    break;
  case CHIP_ESP32S3:
    chip_name = "ESP32S3";
    break;
  case CHIP_ESP32C3:
    chip_name = "ESP32C3";
    break;
  case CHIP_ESP32H2:
    chip_name = "ESP32H2";
    break;
  case CHIP_ESP32C2:
    chip_name = "ESP32C2";
    break;
  default:
    break;
  }
  cJSON_AddStringToObject(root, "chip_name", chip_name);

  // const char *chip_core_str;
  // switch (chip_info.cores) {
  // case 1:
  //   chip_core_str = "Single-Core";
  //   break;
  // case 2:
  //   chip_core_str = "Dual-Core";
  //   break;
  // default:
  //   chip_core_str = "Multi-Core";
  //   break;
  // }

  // with this single line:
  const char *chip_core_str = (chip_info.cores == 1) ? "1" : "2";
  cJSON_AddStringToObject(root, "chip_core", chip_core_str);

  char chip_rev_str[10];
  snprintf(chip_rev_str, sizeof(chip_rev_str), "v%d.%d",
           chip_info.revision / 100, chip_info.revision % 100);
  cJSON_AddStringToObject(root, "chip_rev", chip_rev_str);

  uint32_t flash_size = 0;
  esp_flash_get_size(NULL, &flash_size);
  char chip_flash_str[30];
snprintf(chip_flash_str, sizeof(chip_flash_str), "%lu MB %s",
         (unsigned long)(flash_size / (1024 * 1024)),
         (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "EMBD" : "embedded flash");
  cJSON_AddStringToObject(root, "chip_flash", chip_flash_str);

// Firmware type
#ifndef CONFIG_EB_FIRMWARE_TYPE
#define CONFIG_EB_FIRMWARE_TYPE "EBFirmware"
#endif
  cJSON_AddStringToObject(root, "h_ver", CONFIG_EB_FIRMWARE_TYPE);
  //cJSON_AddStringToObject(root, "restart_reason", get_restart_reason_str());

  // Serialize and send
  char *json_str = cJSON_PrintUnformatted(root);
  esp_err_t send_ret = ESP_FAIL;

  if (json_str) {
    ESP_LOGI(TAG, "%s: %s", event_type, json_str);
    send_ret = eb_azure_iot_send_telemetry(json_str);
    free(json_str);
  } else {
    ESP_LOGE(TAG, "Failed to serialize %s", event_type);
  }

  cJSON_Delete(root);
  return send_ret;
}

esp_err_t eb_azure_iot_send_device_added_event(void) {
  return send_device_info_event("DeviceAddedEvent");
}

esp_err_t eb_azure_iot_send_device_details(void) {
  return send_device_info_event("DeviceDetails");
}

void eb_azure_iot_set_c2d_callback(eb_azure_c2d_cb_t callback) {
  s_c2d_callback = callback;
}

void eb_azure_iot_set_command_callback(eb_azure_command_cb_t callback) {
  s_command_callback = callback;
}

void eb_azure_iot_notify_wifi_state(bool connected) {
  if (!s_prov_event_group)
    return;

  if (connected) {
    ESP_LOGI(TAG, "WiFi connected - enabling IoT Hub reconnect");
    xEventGroupSetBits(s_prov_event_group, WIFI_CONNECTED_BIT);
  } else {
    ESP_LOGI(TAG, "WiFi disconnected - pausing IoT Hub reconnect");
    xEventGroupClearBits(s_prov_event_group, WIFI_CONNECTED_BIT);
    // Clear the MQTT connected flag so the process loop exits cleanly
    s_connected = false;
    eb_connectivity_notify_azure(false);
  }

  // Wake the task from any xTaskNotifyWait sleep immediately
  if (s_azure_task != NULL) {
    xTaskNotify(s_azure_task, AZURE_NOTIFY_WIFI_CHANGED, eSetBits);
  }
}
