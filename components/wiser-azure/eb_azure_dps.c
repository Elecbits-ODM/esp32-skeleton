/**
 * @file eb_azure_dps.c
 * @brief Azure Device Provisioning Service Implementation
 *
 * Uses azure-iot-middleware-freertos to perform X.509 certificate-based
 * DPS provisioning. The flow:
 * 1. Build registration ID from MAC address
 * 2. Connect to DPS via TLS with X.509 client cert
 * 3. Register device with scope_id + reg_id
 * 4. Receive assigned IoT Hub hostname + device_id
 * 5. Cache in NVS for subsequent boots
 *
 * @author Syed S Mashaam (syed.shigarf@elecbits.in)
 * @company Elecbits
 * @date 2025
 */

#include "eb_azure_dps.h"
#include "eb_nvs.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_tls.h"

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Azure IoT Middleware */
#include "azure_iot_provisioning_client.h"
#include "azure_iot_transport_interface.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "eb_azure_dps";

// ============================================================================
// Embedded Certificates (from main/certs/ via EMBED_TXTFILES)
// ============================================================================

extern const char device_cert_pem_start[] asm("_binary_device_cert_pem_start");
extern const char device_cert_pem_end[] asm("_binary_device_cert_pem_end");
extern const char device_key_pem_start[] asm("_binary_device_key_pem_start");
extern const char device_key_pem_end[] asm("_binary_device_key_pem_end");

// ============================================================================
// Module State
// ============================================================================

static char s_registration_id[EB_DPS_REG_ID_MAX_LEN];
static char s_scope_id[32];
static bool s_initialized = false;

// MQTT buffer (single buffer for middleware)
#define DPS_MQTT_BUFFER_SIZE 4096
static uint8_t s_mqtt_buffer[DPS_MQTT_BUFFER_SIZE];

// Output buffers for hostname and device_id
static uint8_t s_hub_hostname_buf[EB_DPS_HOSTNAME_MAX_LEN];
static uint8_t s_device_id_buf[EB_DPS_DEVICE_ID_MAX_LEN];

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

static struct NetworkContext s_dps_network_ctx;

static int32_t tls_transport_send(struct NetworkContext *pxNetworkContext,
                                  const void *pvBuffer, size_t xBytesToSend) {
  TlsTransportCtx_t *ctx = pxNetworkContext->pParams;
  if (!ctx || !ctx->tls_handle)
    return -1;

  int ret = esp_tls_conn_write(ctx->tls_handle, pvBuffer, xBytesToSend);
  return (ret >= 0) ? ret : -1;
}

static int32_t tls_transport_recv(struct NetworkContext *pxNetworkContext,
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

static esp_err_t tls_connect(const char *hostname, uint16_t port) {
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

  int ret;
  do {
    ret = esp_tls_conn_new_sync(hostname, strlen(hostname), port, &cfg,
                                s_tls_ctx.tls_handle);
    if (ret == 0) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  } while (ret == 0);

  if (ret < 0) {
    ESP_LOGE(TAG, "TLS connection to %s:%d failed (ret=%d)", hostname, port,
             ret);
    esp_tls_conn_destroy(s_tls_ctx.tls_handle);
    s_tls_ctx.tls_handle = NULL;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "TLS connected to %s:%d", hostname, port);
  return ESP_OK;
}

static void tls_disconnect(void) {
  if (s_tls_ctx.tls_handle) {
    esp_tls_conn_destroy(s_tls_ctx.tls_handle);
    s_tls_ctx.tls_handle = NULL;
  }
}

// ============================================================================
// Time Callback (required by middleware - returns uint64_t)
// ============================================================================

static uint64_t get_unix_time(void) { return (uint64_t)time(NULL); }

// ============================================================================
// Public API
// ============================================================================

esp_err_t eb_azure_dps_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }

  // NOTE: Registration ID MUST match the certificate CN for X.509 auth
  uint8_t mac[6];
  esp_err_t err = esp_efuse_mac_get_default(mac);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get MAC: %s", esp_err_to_name(err));
    return err;
  }

  // TODO: For production, each device needs a certificate with CN matching its
  // MAC
#if 0 // Set to 0 to use MAC-based registration ID, Set to 1 to use hardcoded ID
      // matching cert CN
  strncpy(s_registration_id, "80F1B23F9D04", sizeof(s_registration_id) - 1);
  s_registration_id[sizeof(s_registration_id) - 1] = '\0';
  ESP_LOGW(TAG, "Using registration ID matching device cert CN: %s",
           s_registration_id);
#else
  snprintf(s_registration_id, sizeof(s_registration_id),
           "%s%02X%02X%02X%02X%02X%02X", EB_DPS_REG_ID_PREFIX, mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
#endif

  // Load scope_id from BLE provisioning
  char provisioned_scope_id[32] = {0};
  if (eb_nvs_load_scope_id(provisioned_scope_id,
                           sizeof(provisioned_scope_id)) == ESP_OK &&
      strlen(provisioned_scope_id) > 0) {
    strncpy(s_scope_id, provisioned_scope_id, sizeof(s_scope_id) - 1);
    ESP_LOGI(TAG, "Using scope_id from BLE provisioning: %s", s_scope_id);
  } else {
    strncpy(s_scope_id, EB_DPS_ID_SCOPE, sizeof(s_scope_id) - 1);
    ESP_LOGI(TAG, "Using scope_id from Kconfig (default): %s", s_scope_id);
  }

  ESP_LOGI(TAG, "DPS Registration ID: %s", s_registration_id);
  ESP_LOGD(TAG, "DPS Endpoint: %s", EB_DPS_ENDPOINT);
  // ESP_LOGI(TAG, "DPS Registration ID: %s", s_registration_id);
  // ESP_LOGI(TAG, "DPS Endpoint: %s", EB_DPS_ENDPOINT);
  // ESP_LOGI(TAG, "DPS ID Scope: %s", EB_DPS_ID_SCOPE);

  s_initialized = true;
  return ESP_OK;
}

esp_err_t eb_azure_dps_provision(char *hub_hostname, size_t hostname_len,
                                 char *device_id, size_t device_id_len) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  // Check if already provisioned (cached in NVS)
  eb_azure_config_t azure_cfg;
  if (eb_nvs_load_azure_config(&azure_cfg) == ESP_OK && azure_cfg.provisioned) {
    ESP_LOGI(TAG, "Using cached DPS result from NVS");
    strncpy(hub_hostname, azure_cfg.hub_hostname, hostname_len - 1);
    hub_hostname[hostname_len - 1] = '\0';
    strncpy(device_id, azure_cfg.device_id, device_id_len - 1);
    device_id[device_id_len - 1] = '\0';
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Starting DPS provisioning...");
  ESP_LOGI(TAG, "  Registration ID: %s", s_registration_id);
  ESP_LOGI(TAG, "  ID Scope: %s", s_scope_id);
  ESP_LOGI(TAG, "  Endpoint: %s", EB_DPS_ENDPOINT);

  // Connect TLS to DPS endpoint
  esp_err_t err = tls_connect(EB_DPS_ENDPOINT, EB_DPS_PORT);
  if (err != ESP_OK) {
    return err;
  }

  // Set up transport interface (Azure middleware transport)
  s_dps_network_ctx.pParams = &s_tls_ctx;

  AzureIoTTransportInterface_t transport = {
      .xSend = tls_transport_send,
      .xRecv = tls_transport_recv,
      .pxNetworkContext = &s_dps_network_ctx,
  };

  // Initialize Azure DPS provisioning client
  AzureIoTProvisioningClient_t dps_client;
  AzureIoTResult_t az_result;

  az_result = AzureIoTProvisioningClient_Init(
      &dps_client, (const uint8_t *)EB_DPS_ENDPOINT,
      (uint32_t)strlen(EB_DPS_ENDPOINT), (const uint8_t *)s_scope_id,
      (uint32_t)strlen(s_scope_id), (const uint8_t *)s_registration_id,
      (uint32_t)strlen(s_registration_id), NULL, /* No special options */
      s_mqtt_buffer, sizeof(s_mqtt_buffer), get_unix_time, &transport);

  if (az_result != eAzureIoTSuccess) {
    ESP_LOGE(TAG, "DPS client init failed: %d", az_result);
    tls_disconnect();
    return ESP_FAIL;
  }

  // Register the device (X.509 auth happens via TLS client cert)
  ESP_LOGD(TAG, "Registering device with DPS...");

  // The middleware's Register call might return eAzureIoTErrorPending if the
  // server is still assigning the device. We should loop until success or fatal
  // error.
  uint32_t register_attempts = 0;
  do {
    az_result = AzureIoTProvisioningClient_Register(&dps_client, 10000);
    if (az_result == eAzureIoTErrorPending) {
      register_attempts++;
      ESP_LOGI(TAG, "Registration pending (attempt %u)...",
               (unsigned int)register_attempts);

      // Explicitly reset WDT while waiting for server response
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } while (az_result == eAzureIoTErrorPending && register_attempts < 10);

  if (az_result != eAzureIoTSuccess) {
    ESP_LOGE(TAG, "DPS registration failed: %d", az_result);
    AzureIoTProvisioningClient_Deinit(&dps_client);
    tls_disconnect();
    return ESP_FAIL;
  }

  // Get assigned hub + device ID (lengths are input/output - must be
  // initialized to buffer sizes)
  uint32_t out_hostname_len = sizeof(s_hub_hostname_buf);
  uint32_t out_device_id_len = sizeof(s_device_id_buf);

  az_result = AzureIoTProvisioningClient_GetDeviceAndHub(
      &dps_client, s_hub_hostname_buf, &out_hostname_len, s_device_id_buf,
      &out_device_id_len);

  if (az_result != eAzureIoTSuccess) {
    ESP_LOGE(TAG, "Failed to get hub/device from DPS: %d", az_result);
    AzureIoTProvisioningClient_Deinit(&dps_client);
    tls_disconnect();
    return ESP_FAIL;
  }

  // Copy results to output buffers
  size_t copy_len = (out_hostname_len < hostname_len - 1) ? out_hostname_len
                                                          : hostname_len - 1;
  memcpy(hub_hostname, s_hub_hostname_buf, copy_len);
  hub_hostname[copy_len] = '\0';

  copy_len = (out_device_id_len < device_id_len - 1) ? out_device_id_len
                                                     : device_id_len - 1;
  memcpy(device_id, s_device_id_buf, copy_len);
  device_id[copy_len] = '\0';

  ESP_LOGI(TAG, "DPS provisioned: Hub=%s, DeviceID=%s", hub_hostname,
           device_id);

  // Cache in NVS
  eb_azure_config_t config = {0};
  strncpy(config.scope_id, s_scope_id, sizeof(config.scope_id) - 1);
  strncpy(config.registration_id, s_registration_id,
          sizeof(config.registration_id) - 1);
  strncpy(config.hub_hostname, hub_hostname, sizeof(config.hub_hostname) - 1);
  strncpy(config.device_id, device_id, sizeof(config.device_id) - 1);
  config.provisioned = true;
  eb_nvs_save_azure_config(&config);

  AzureIoTProvisioningClient_Deinit(&dps_client);
  tls_disconnect();
  return ESP_OK;
}

const char *eb_azure_dps_get_registration_id(void) { return s_registration_id; }
