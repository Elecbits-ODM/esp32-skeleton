/**
 * @file eb_azure_dps.h
 * @brief Azure Device Provisioning Service (DPS) client
 *
 * Handles X.509 certificate-based DPS provisioning to obtain
 * IoT Hub hostname and device ID. Caches result in NVS for
 * subsequent boots.
 *
 * @author Syed S Mashaam (syed.shigarf@elecbits.in)
 * @company Elecbits
 * @date 2025
 */

#ifndef EB_AZURE_DPS_H
#define EB_AZURE_DPS_H

#include "esp_err.h"
#include "sdkconfig.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// DPS Configuration (from Kconfig - use menuconfig to change)
// ============================================================================

#define EB_DPS_ENDPOINT         CONFIG_EB_AZURE_DPS_ENDPOINT
#define EB_DPS_ID_SCOPE         CONFIG_EB_AZURE_DPS_ID_SCOPE
#define EB_DPS_REG_ID_PREFIX    CONFIG_EB_AZURE_DPS_REG_ID_PREFIX

#define EB_DPS_PORT             8883
#define EB_DPS_REG_ID_MAX_LEN   64
#define EB_DPS_HOSTNAME_MAX_LEN 128
#define EB_DPS_DEVICE_ID_MAX_LEN 64

// ============================================================================
// API Functions
// ============================================================================

/**
 * @brief Initialize DPS module
 *
 * Builds registration ID from optional prefix + MAC address.
 * Registration ID must match the certificate CN for X.509 auth.
 *
 * @return ESP_OK on success
 */
esp_err_t eb_azure_dps_init(void);

/**
 * @brief Provision device via DPS
 *
 * Connects to DPS using X.509 client certificate, registers the device,
 * and returns the assigned IoT Hub hostname and device ID.
 * On success, caches the result in NVS.
 *
 * @param hub_hostname Output buffer for assigned IoT Hub hostname
 * @param hostname_len Size of hostname buffer
 * @param device_id Output buffer for assigned device ID
 * @param device_id_len Size of device_id buffer
 * @return ESP_OK on success
 * @return ESP_ERR_TIMEOUT if registration times out
 * @return ESP_FAIL on provisioning failure
 */
esp_err_t eb_azure_dps_provision(char *hub_hostname, size_t hostname_len,
                                  char *device_id, size_t device_id_len);

/**
 * @brief Get the DPS registration ID
 * @return Registration ID string (e.g. "98A316D7FB04" or "prefix-98A316D7FB04")
 */
const char *eb_azure_dps_get_registration_id(void);

#ifdef __cplusplus
}
#endif

#endif // EB_AZURE_DPS_H
