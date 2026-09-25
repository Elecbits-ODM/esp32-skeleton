/**
 * @file eb_ble_prov.c
 * @brief BLE WiFi Provisioning — Custom Bluedroid GATT Server
 *
 * Implements a custom BLE GATT server compatible with the Wiser mobile app.
 * Protocol: BLE SC pairing -> "BLAZE" auth key -> JSON credential exchange.
 *
 * Based on Wiser Switch 16A reference (on_boarding_wifi.c).
 *
 * @company Elecbits
 */

#include "eb_ble_prov.h"
#include "eb_nvs.h"
#include "eb_wifi.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "eb_ble_prov";

// ============================================================================
// GATT Table Indices
// ============================================================================

enum {
    IDX_SVC,
    IDX_CHAR_A,
    IDX_CHAR_VAL_A,
    IDX_CHAR_CFG_A,
    IDX_CHAR_B,
    IDX_CHAR_VAL_B,
    IDX_CHAR_CFG_B,
    HRS_IDX_NB,
};

// ============================================================================
// Constants
// ============================================================================

#define PROFILE_NUM                 1
#define PROFILE_APP_IDX             0
#define ESP_APP_ID                  0x55
#define SVC_INST_ID                 0

#define GATTS_CHAR_VAL_LEN_MAX      500
#define PREPARE_BUF_MAX_SIZE        1024
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))

#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

#define DEFAULT_SCAN_LIST_SIZE      15

// JSON keys matching the Wiser app protocol
#define JSON_KEY_SSID       "SSID"
#define JSON_KEY_PASS       "PASSWORD"
#define JSON_KEY_DEV_ID     "DEVICE_ID"
#define JSON_KEY_OTA_URL    "URL"
#define JSON_KEY_SCOPE_ID   "SCOPE_ID"

// ============================================================================
// UUIDs (exact match with Wiser Switch 16A reference)
// ============================================================================

// Advertising UUID (app scans for 0x00FF)
static uint8_t service_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

// GATT Service UUID
static uint8_t GATTS_SERVICE_UUID[16] = {
    0xbe, 0x10, 0x14, 0x43, 0x22, 0x39, 0x3e, 0xb1,
    0xe5, 0xd3, 0xa5, 0xd5, 0x00, 0x08, 0x3e, 0xd4
};

// Char A UUID (auth key characteristic) — byte[12] = 0x11
static uint8_t GATTS_CHAR_UUID_A[16] = {
    0xbe, 0x10, 0x14, 0x43, 0x22, 0x39, 0x3e, 0xb1,
    0xe5, 0xd3, 0xa5, 0xd5, 0x11, 0x08, 0x3e, 0xd4
};

// Char B UUID (data characteristic) — byte[12] = 0x22
static uint8_t GATTS_CHAR_UUID_B[16] = {
    0xbe, 0x10, 0x14, 0x43, 0x22, 0x39, 0x3e, 0xb1,
    0xe5, 0xd3, 0xa5, 0xd5, 0x22, 0x08, 0x3e, 0xd4
};

// ============================================================================
// GATT Attribute Table Constants
// ============================================================================

static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid    = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid  = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  char_prop_write_notify        = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t  data_ccc[2]                   = {0x00, 0x00};
static const uint8_t  char_value[4]                 = {0x11, 0x22, 0x33, 0x44};

// Full GATT Database (7 entries)
static const esp_gatts_attr_db_t gatt_db[HRS_IDX_NB] = {
    // Service Declaration
    [IDX_SVC] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ_ENCRYPTED,
      sizeof(uint16_t), sizeof(GATTS_SERVICE_UUID), (uint8_t *)&GATTS_SERVICE_UUID}},

    // Char A Declaration
    [IDX_CHAR_A] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ_ENCRYPTED,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write_notify}},

    // Char A Value
    [IDX_CHAR_VAL_A] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)&GATTS_CHAR_UUID_A, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      GATTS_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // Char A CCCD
    [IDX_CHAR_CFG_A] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      sizeof(uint16_t), sizeof(data_ccc), (uint8_t *)data_ccc}},

    // Char B Declaration
    [IDX_CHAR_B] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ_ENCRYPTED,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write_notify}},

    // Char B Value
    [IDX_CHAR_VAL_B] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)&GATTS_CHAR_UUID_B, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      GATTS_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // Char B CCCD
    [IDX_CHAR_CFG_B] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      sizeof(uint16_t), sizeof(data_ccc), (uint8_t *)data_ccc}},
};

// ============================================================================
// Advertising Configuration
// ============================================================================

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp        = false,
    .include_name        = false,
    .include_txpower     = false,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(service_uuid),
    .p_service_uuid      = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// Scan response: ONLY device name (25 bytes) - keeps under 31-byte limit
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = false,
    .min_interval        = 0,          // Don't include interval range
    .max_interval        = 0,          // Don't include interval range
    .appearance          = 0,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag = 0,                         // Flags only in adv_data, not scan_rsp
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min         = 0x20,
    .adv_int_max         = 0x40,
    .adv_type            = ADV_TYPE_IND,
    .own_addr_type       = BLE_ADDR_TYPE_PUBLIC,
    .channel_map         = ADV_CHNL_ALL,
    .adv_filter_policy   = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// ============================================================================
// Prepare Write Buffer
// ============================================================================

typedef struct {
    uint8_t  *prepare_buf;
    int       prepare_len;
} prepare_type_env_t;

static prepare_type_env_t s_prepare_write_env;

// ============================================================================
// GATTS Profile Instance
// ============================================================================

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param);

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

static struct gatts_profile_inst s_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

// ============================================================================
// State Variables
// ============================================================================

static bool s_initialized = false;
static volatile eb_ble_prov_state_t s_prov_state = EB_BLE_PROV_STATE_IDLE;
static char s_device_name[48] = {0};

static uint16_t s_handle_table[HRS_IDX_NB];
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static esp_bd_addr_t s_remote_bda = {0};

static char s_pair_status = 0;          // 0=unpaired, 1=paired, 2=addr-mismatch
static char s_ble_auth_key = 0;         // 0=none, 1=failed, 2=BLAZE-ok
static bool s_adv_flag = false;
static unsigned int s_onboard_count = 0;
static unsigned int s_restart_count = 0;
static bool s_wifi_init_flag = false;
static bool s_change_ap = false;
static uint32_t s_ble_provision_time = 0;
static eb_prov_status_t s_current_prov_status = EB_PROV_STATUS_NORMAL;

static uint8_t s_adv_config_done = 0;

// WiFi disconnect reason tracking
static bool s_ap_not_found = false;
static bool s_auth_fail = false;

// Credential globals for verify_connection task
static char s_ssid[33] = {0};
static char s_password[65] = {0};
static char s_device_id[40] = {0};
static char s_scope_id_buf[32] = {0};
static char s_ota_url_buf[256] = {0};

// Callbacks
static eb_ble_prov_state_cb_t s_state_callback = NULL;
static eb_ble_prov_success_cb_t s_success_callback = NULL;

// ============================================================================
// Forward Declarations
// ============================================================================

static void set_prov_state(eb_ble_prov_state_t new_state);
static void generate_device_name(eb_prov_status_t prov_status);
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static void process_wifi_credentials(char *data_json);
static void prepare_write_event(esp_gatt_if_t gatts_if, prepare_type_env_t *env, esp_ble_gatts_cb_param_t *param);
static void exec_write_event(prepare_type_env_t *env, esp_ble_gatts_cb_param_t *param);
static void client_notify(const char *wifi_status);
static void verify_connection_task(void *arg);
static void wifi_disconnect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void show_bonded_devices(void);
static void remove_all_bonded_devices(void);
static void enable_pairing_security(void);
static void do_wifi_scan_and_notify(void);

// ============================================================================
// Internal: State Management
// ============================================================================

static void set_prov_state(eb_ble_prov_state_t new_state)
{
    if (s_prov_state != new_state) {
        s_prov_state = new_state;

        static const char *state_names[] = {
            "IDLE", "STARTING", "ADVERTISING", "CONNECTED",
            "PAIRED", "AUTHENTICATED", "PROVISIONING",
            "SUCCESS", "FAILED", "STOPPED"
        };
        ESP_LOGI(TAG, "State: %s", state_names[new_state]);

        if (s_state_callback != NULL) {
            s_state_callback(new_state);
        }
    }
}

// ============================================================================
// Internal: Device Name Generation
// ============================================================================

static void generate_device_name(eb_prov_status_t prov_status)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (prov_status == EB_PROV_STATUS_CHANGE_AP) {
        // "CA_SE-VC(DDEEFF)" — last 6 hex chars of MAC
        snprintf(s_device_name, sizeof(s_device_name), "CA_%s(%s)",
                 EB_BLE_PROV_DEVICE_NAME_PREFIX, &mac_str[6]);
    } else {
        // "SE-VC(AABBCCDDEEFF)" — full MAC
        snprintf(s_device_name, sizeof(s_device_name), "%s(%s)",
                 EB_BLE_PROV_DEVICE_NAME_PREFIX, mac_str);
    }

    ESP_LOGD(TAG, "BLE device name: %s", s_device_name);
}

// ============================================================================
// Internal: Bonded Device Management
// ============================================================================

static void show_bonded_devices(void)
{
    int dev_num = esp_ble_get_bond_device_num();
    ESP_LOGD(TAG, "Bonded devices: %d", dev_num);

    if (dev_num > 0) {
        esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
        if (dev_list) {
            esp_ble_get_bond_device_list(&dev_num, dev_list);
            for (int i = 0; i < dev_num; i++) {
                ESP_LOG_BUFFER_HEX(TAG, (void *)dev_list[i].bd_addr, sizeof(esp_bd_addr_t));
            }
            free(dev_list);
        }
    }
}

static void remove_all_bonded_devices(void)
{
    int dev_num = esp_ble_get_bond_device_num();
    ESP_LOGD(TAG, "Removing %d bonded devices", dev_num);

    if (dev_num > 0) {
        esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
        if (dev_list) {
            esp_ble_get_bond_device_list(&dev_num, dev_list);
            for (int i = 0; i < dev_num; i++) {
                esp_ble_remove_bond_device(dev_list[i].bd_addr);
            }
            free(dev_list);
        }
    }
}

// ============================================================================
// Internal: Security Setup
// ============================================================================

static void enable_pairing_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    ESP_LOGD(TAG, "BLE pairing security configured (SC MITM Bond)");
}

// ============================================================================
// Internal: BLE Notification
// ============================================================================

// Sends a status notification to the connected Wiser app via Char B.
// All status strings are wrapped as: {"wifi_status": "<wifi_status>"}
// Common values: "connected", "ap_not_found", "auth_failed", "missing parameter",
//                "dev_id mismatched", "scan_failed".
static void client_notify(const char *wifi_status)
{
    if (s_gatts_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "Cannot notify: no GATTS interface");
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return;

    cJSON_AddStringToObject(obj, "wifi_status", wifi_status);
    char *json_str = cJSON_PrintUnformatted(obj);
    if (json_str) {
        uint16_t len = strlen(json_str);
        ESP_LOGD(TAG, "Notifying client: %s", json_str);
        esp_err_t err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
            s_handle_table[IDX_CHAR_VAL_B], len, (uint8_t *)json_str, false);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "Notification sent successfully (%d bytes)", len);
        } else {
            ESP_LOGE(TAG, "BLE notify failed: %s", esp_err_to_name(err));
        }
        free(json_str);
    } else {
        ESP_LOGE(TAG, "Failed to create notification JSON");
    }
    cJSON_Delete(obj);
}

// ============================================================================
// Internal: WiFi Scan and BLE Notify
// ============================================================================

// Runs WiFi scan and sends results to the Wiser app via BLE notification.
// Must run in its own task — blocking the BT GATT event callback for ~5s stalls
// all BLE event processing and causes the coexistence scheduler to starve WiFi.
//
// BLE coexistence note: the default 7.5–20ms connection interval gives WiFi only
// ~6–8ms RF windows, too short for active-scan probe responses. We first request
// a 100–200ms interval so WiFi gets adequate timeslots, then start the scan.
static void wifi_scan_task(void *arg)
{
    // Widen the BLE connection interval before scanning so the coexistence
    // scheduler allocates longer WiFi timeslots (default 7.5ms is too narrow).
    esp_ble_conn_update_params_t conn_params = {
        .min_int = 0x0050,   //  80 × 1.25ms = 100ms
        .max_int = 0x00A0,   // 160 × 1.25ms = 200ms
        .latency = 0,
        .timeout = 1000,     // 10s supervision timeout (>> 6 × 200ms)
    };
    memcpy(conn_params.bda, s_remote_bda, sizeof(esp_bd_addr_t));
    esp_err_t cp_err = esp_ble_gap_update_conn_params(&conn_params);
    ESP_LOGD(TAG, "Conn param update requested (min=100ms max=200ms): %s",
             esp_err_to_name(cp_err));
    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for phone to accept new params

    // Disable WiFi power-save so WiFi is active every coexistence slot.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Active scan (probe requests): more reliable than passive scan under BLE
    // coexistence because it does not depend on periodic beacon timing.
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    ESP_LOGD(TAG, "Starting active WiFi scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        client_notify("scan_failed");
        vTaskDelete(NULL);
        return;
    }

    uint16_t number = DEFAULT_SCAN_LIST_SIZE;
    wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * number);
    if (!ap_info) {
        client_notify("scan_failed");
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(wifi_ap_record_t) * number);

    // IMPORTANT: call get_ap_num BEFORE get_ap_records.
    // In ESP-IDF 5.x, esp_wifi_scan_get_ap_records() frees the internal AP
    // list after copying it, so any get_ap_num() call afterwards returns 0.
    // number (in/out) is set to the actual records copied by get_ap_records.
    esp_wifi_scan_get_ap_num(&ap_count);
    esp_wifi_scan_get_ap_records(&number, ap_info);
    // esp_wifi_clear_ap_list() not needed — get_ap_records already frees it.

    ESP_LOGI(TAG, "Scan found %d APs (%d returned)", ap_count, number);

    // Format: {"event_type":"scan_list", "data":{"SSID1":"rssi1", "SSID2":"rssi2", ...}}
    cJSON *root = cJSON_CreateObject();
    cJSON *data_obj = cJSON_CreateObject();
    if (root && data_obj) {
        // Use number (actual records in ap_info), not ap_count (total found,
        // may exceed DEFAULT_SCAN_LIST_SIZE and would be the same or larger).
        uint16_t count = number;

        for (int i = 0; i < count; i++) {
            char rssi_str[8];
            snprintf(rssi_str, sizeof(rssi_str), "%d", ap_info[i].rssi);
            cJSON_AddStringToObject(data_obj, (char *)ap_info[i].ssid, rssi_str);
        }

        cJSON_AddStringToObject(root, "event_type", "scan_list");
        cJSON_AddItemToObject(root, "data", data_obj);

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            uint16_t len = strlen(json_str);
            esp_err_t notify_err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                s_handle_table[IDX_CHAR_VAL_B], len, (uint8_t *)json_str, false);
            ESP_LOGD(TAG, "Scan notify sent (%d bytes): %s", len, esp_err_to_name(notify_err));
            free(json_str);
        }
        cJSON_Delete(root);
    } else {
        if (root) cJSON_Delete(root);
        if (data_obj) cJSON_Delete(data_obj);
    }

    free(ap_info);
    vTaskDelete(NULL);
}

static void do_wifi_scan_and_notify(void)
{
    ESP_LOGI(TAG, "WiFi scan triggered via BLE");
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL);
}

// ============================================================================
// Internal: JSON Command and Credential Processing
// ============================================================================

// Dispatches all Char-B payloads received from the Wiser app.
//
// Two payload shapes are handled:
//   1. Commands  — JSON contains "cmd_type": "scan_list" | "restart" | "mesh_support"
//   2. Credentials — JSON contains "SSID" and "PASSWORD" (DEVICE_ID optional but saved
//      if present; SCOPE_ID and URL are also stored to NVS if provided).
//
// On change-AP flows (s_change_ap == true) the incoming DEVICE_ID must match the
// one saved from the previous provisioning session, otherwise the request is rejected.
static void process_wifi_credentials(char *data_json)
{
    ESP_LOGD(TAG, "Processing WiFi credentials JSON");

    ESP_LOGD(TAG, "RX JSON (len=%d): %.120s", (int)strlen(data_json), data_json);

    cJSON *root = cJSON_Parse(data_json);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON data");
        client_notify("Invalid JSON data");
        return;
    }

    cJSON *ssid_obj     = cJSON_GetObjectItem(root, JSON_KEY_SSID);
    cJSON *pass_obj     = cJSON_GetObjectItem(root, JSON_KEY_PASS);
    cJSON *dev_id_obj   = cJSON_GetObjectItem(root, JSON_KEY_DEV_ID);
    cJSON *server_url   = cJSON_GetObjectItem(root, JSON_KEY_OTA_URL);
    cJSON *scope_id     = cJSON_GetObjectItem(root, JSON_KEY_SCOPE_ID);
    cJSON *command_type = cJSON_GetObjectItem(root, "cmd_type");

    // On change-AP, verify the incoming DEVICE_ID matches what was saved at first provision.
    if (s_change_ap && cJSON_IsString(dev_id_obj) && dev_id_obj->valuestring) {
        char saved_dev_id[40] = {0};
        if (eb_nvs_load_prov_device_id(saved_dev_id, sizeof(saved_dev_id)) == ESP_OK) {
            if (strcmp(saved_dev_id, dev_id_obj->valuestring) != 0) {
                ESP_LOGW(TAG, "Device ID mismatch: saved=%s, received=%s",
                         saved_dev_id, dev_id_obj->valuestring);
                client_notify("dev_id mismatched");
                s_restart_count = 1;
                cJSON_Delete(root);
                return;
            }
        }
    } else if (s_change_ap && !dev_id_obj) {
        client_notify("dev_id not available");
        cJSON_Delete(root);
        return;
    }

    // Save OTA URL if provided
    if (cJSON_IsString(server_url) && server_url->valuestring) {
        ESP_LOGI(TAG, "OTA URL received: %s", server_url->valuestring);
        snprintf(s_ota_url_buf, sizeof(s_ota_url_buf), "%s", server_url->valuestring);
        eb_nvs_save_ota_url(server_url->valuestring);
    }

    // Save Scope ID if provided
    if (cJSON_IsString(scope_id) && scope_id->valuestring) {
        ESP_LOGI(TAG, "Scope ID received: %s", scope_id->valuestring);
        snprintf(s_scope_id_buf, sizeof(s_scope_id_buf), "%s", scope_id->valuestring);
        eb_nvs_save_scope_id(scope_id->valuestring);
    }

    // Credential path: SSID + PASSWORD required; DEVICE_ID optional (saved if present).
    if (cJSON_IsString(ssid_obj) && ssid_obj->valuestring) {
        int wifi_details = 0;
        char ssid_wifi[33] = {0};
        char password_wifi[65] = {0};
        char dev_id[40] = {0};

        snprintf(ssid_wifi, sizeof(ssid_wifi), "%s", ssid_obj->valuestring);
        if (strlen(ssid_wifi) < 33) {
            wifi_details++;
        }

        if (cJSON_IsString(pass_obj) && pass_obj->valuestring) {
            snprintf(password_wifi, sizeof(password_wifi), "%s", pass_obj->valuestring);
            if (strlen(password_wifi) < 65) {
                wifi_details++;
            }
        }

        if (cJSON_IsString(dev_id_obj) && dev_id_obj->valuestring) {
            snprintf(dev_id, sizeof(dev_id), "%s", dev_id_obj->valuestring);
            wifi_details++;
        }

        if (wifi_details >= 2) {
            ESP_LOGI(TAG, "WiFi creds valid: SSID=%s, DevID=%s (details=%d)", ssid_wifi, dev_id, wifi_details);

            // Pass credentials to the verify_connection task via module-level globals.
            strncpy(s_ssid, ssid_wifi, sizeof(s_ssid) - 1);
            strncpy(s_password, password_wifi, sizeof(s_password) - 1);
            strncpy(s_device_id, dev_id, sizeof(s_device_id) - 1);

            s_wifi_init_flag = true;
            set_prov_state(EB_BLE_PROV_STATE_PROVISIONING);
            eb_wifi_connect_with_credentials(ssid_wifi, password_wifi, false);
        } else {
            ESP_LOGW(TAG, "Missing WiFi parameters (got %d/3)", wifi_details);
            client_notify("missing parameter");
        }
    }
    // Command path: JSON has "cmd_type" but no "SSID".
    else if (cJSON_IsString(command_type) && command_type->valuestring) {
        ESP_LOGD(TAG, "Command received: %s", command_type->valuestring);

        if (strcmp(command_type->valuestring, "scan_list") == 0) {
            do_wifi_scan_and_notify();
        } else if (strcmp(command_type->valuestring, "restart") == 0) {
            ESP_LOGW(TAG, "Restart command received");
            esp_restart();
        } else if (strcmp(command_type->valuestring, "mesh_support") == 0) {
            // Reference firmware (on_boarding_wifi.c) always responds {"event_type":"mesh_enabled"}
            // regardless of actual mesh capability — the Wiser app expects this exact format.
            const char *response = "{\"event_type\":\"mesh_enabled\"}";
            esp_err_t err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                s_handle_table[IDX_CHAR_VAL_B], strlen(response), (uint8_t *)response, false);
            ESP_LOGD(TAG, "Mesh response sent: %s (%s)", response, esp_err_to_name(err));
        }
    }

    cJSON_Delete(root);
}

// ============================================================================
// Internal: Prepare Write Handler
// ============================================================================

static void prepare_write_event(esp_gatt_if_t gatts_if, prepare_type_env_t *env,
                                esp_ble_gatts_cb_param_t *param)
{
    esp_gatt_status_t status = ESP_GATT_OK;

    if (env->prepare_buf == NULL) {
        env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE);
        env->prepare_len = 0;
        if (env->prepare_buf == NULL) {
            ESP_LOGE(TAG, "Prepare write: out of memory");
            status = ESP_GATT_NO_RESOURCES;
        }
    } else {
        if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
            status = ESP_GATT_INVALID_OFFSET;
        } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
            status = ESP_GATT_INVALID_ATTR_LEN;
        }
    }

    if (param->write.need_rsp) {
        esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
        if (gatt_rsp) {
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                         param->write.trans_id, status, gatt_rsp);
            free(gatt_rsp);
        }
    }

    if (status != ESP_GATT_OK) {
        return;
    }

    memcpy(env->prepare_buf + param->write.offset, param->write.value, param->write.len);
    env->prepare_len += param->write.len;
}

// ============================================================================
// Internal: Execute Write Handler
// ============================================================================

static void exec_write_event(prepare_type_env_t *env, esp_ble_gatts_cb_param_t *param)
{
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && env->prepare_buf) {
        // Null-terminate the buffer
        if (env->prepare_len < PREPARE_BUF_MAX_SIZE) {
            env->prepare_buf[env->prepare_len] = '\0';
        } else {
            env->prepare_buf[PREPARE_BUF_MAX_SIZE - 1] = '\0';
        }

        ESP_LOGD(TAG, "Exec write data (len=%d)", env->prepare_len);

        if (s_ble_auth_key == 2 && s_pair_status == 1) {
            process_wifi_credentials((char *)env->prepare_buf);
        } else {
            ESP_LOGW(TAG, "Exec write rejected: not authenticated");
            s_ble_auth_key = 1;
        }
    } else {
        ESP_LOGD(TAG, "Exec write cancelled");
    }

    if (env->prepare_buf) {
        free(env->prepare_buf);
        env->prepare_buf = NULL;
    }
    env->prepare_len = 0;
}

// ============================================================================
// Internal: WiFi Disconnect Event Handler
// ============================================================================

static void wifi_disconnect_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;

        if (!s_wifi_init_flag) {
            return;  // Not in provisioning mode
        }

        switch (disconn->reason) {
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
                ESP_LOGW(TAG, "AP not found (reason=%d)", disconn->reason);
                s_ap_not_found = true;
                break;
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                ESP_LOGW(TAG, "Auth failed (reason=%d)", disconn->reason);
                s_auth_fail = true;
                break;
            default:
                break;
        }
    }
}

// ============================================================================
// Internal: verify_connection Task
// ============================================================================

// Monitors WiFi connection progress after credentials are received and notifies
// the Wiser app of the outcome via Char B ("connected" / "ap_not_found" / "auth_failed").
// On success: saves credentials + device identity to NVS, fires the success callback,
//             and triggers a device restart (via s_restart_count reaching threshold).
// Timeouts: 30s for pairing, 60s for WiFi connection after credentials are supplied.
static void verify_connection_task(void *arg)
{
    bool got_router_config = false;

    ESP_LOGD(TAG, "verify_connection task started");

    while (1) {
        // Timeout logic:
        // - Auth failed (key=1): disconnect immediately
        // - Waiting for auth (key=0): timeout after 30s (allow time for pairing)
        // - Auth OK (key=2), waiting for WiFi: timeout after 60s
        bool should_timeout = false;
        if (s_ble_auth_key == 1) {
            // Auth explicitly failed
            ESP_LOGW(TAG, "Auth failed, closing BLE");
            should_timeout = true;
        } else if (s_ble_auth_key == 0 && s_onboard_count >= 3000) {
            // No auth after 30s
            ESP_LOGW(TAG, "Pairing timeout (%u), closing BLE", s_onboard_count);
            should_timeout = true;
        } else if (s_ble_auth_key == 2 && s_wifi_init_flag && s_onboard_count >= 6000) {
            // Auth OK but WiFi not connecting after 60s
            ESP_LOGW(TAG, "WiFi connection timeout (%u), closing BLE", s_onboard_count);
            should_timeout = true;
        }

        if (should_timeout) {
            s_onboard_count = 0;
            esp_ble_gatts_close(s_gatts_if, s_conn_id);
            s_adv_flag = false;
        }

        if (s_wifi_init_flag) {
            // WiFi connected successfully
            if (eb_wifi_is_connected() && !got_router_config) {
                ESP_LOGI(TAG, "WiFi connected during provisioning");

                // Save credentials to NVS
                eb_wifi_credentials_t creds = {0};
                strncpy(creds.ssid, s_ssid, sizeof(creds.ssid) - 1);
                strncpy(creds.password, s_password, sizeof(creds.password) - 1);
                creds.configured = true;
                eb_nvs_save_wifi_credentials(&creds);

                // Save device ID
                if (s_device_id[0] != '\0') {
                    eb_nvs_save_prov_device_id(s_device_id);
                }

                // Reset provisioning status to normal
                eb_nvs_set_prov_status(0, 180);

                // Notify BLE client
                client_notify("connected");

                // Call success callback
                if (s_success_callback) {
                    eb_ble_prov_credentials_t prov_creds = {0};
                    strncpy(prov_creds.ssid, s_ssid, sizeof(prov_creds.ssid) - 1);
                    strncpy(prov_creds.password, s_password, sizeof(prov_creds.password) - 1);
                    strncpy(prov_creds.device_id, s_device_id, sizeof(prov_creds.device_id) - 1);
                    strncpy(prov_creds.scope_id, s_scope_id_buf, sizeof(prov_creds.scope_id) - 1);
                    strncpy(prov_creds.ota_url, s_ota_url_buf, sizeof(prov_creds.ota_url) - 1);
                    s_success_callback(&prov_creds);
                }

                set_prov_state(EB_BLE_PROV_STATE_SUCCESS);
                got_router_config = true;
                s_restart_count = 1;
            }
            // AP not found
            else if (s_ap_not_found) {
                ESP_LOGW(TAG, "AP not found — notifying client");
                client_notify("ap_not_found");
                s_ap_not_found = false;
                s_restart_count = 1;
            }
            // Auth failure
            else if (s_auth_fail) {
                ESP_LOGW(TAG, "Auth failed — notifying client");
                client_notify("auth_failed");
                s_auth_fail = false;
                s_restart_count = 1;
            }

            // After enough delay, close BLE and restart
            if (s_restart_count >= 5) {
                esp_ble_gatts_close(s_gatts_if, s_conn_id);
                esp_wifi_stop();
                s_wifi_init_flag = false;
                s_restart_count = 0;
                if (got_router_config) {
                    ESP_LOGI(TAG, "Provisioning complete — restarting device");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }

            // Increment restart count
            if (s_restart_count >= 1) {
                s_restart_count++;
            }
        }

        // Increment onboard count (connection timeout tracking)
        if (s_onboard_count >= 1) {
            s_onboard_count++;
        }

        // Provisioning advertising timeout — 3 minutes (180s)
        // Counts in 1-second ticks (loop runs every 10ms, so 100 loops = 1s).
        // Only active while advertising and not yet authenticated (key != 2).
#define BLE_PROV_ADV_TIMEOUT_SEC  180
        static uint32_t s_adv_tick = 0;
        if (s_prov_state == EB_BLE_PROV_STATE_ADVERTISING && s_ble_auth_key != 2) {
            s_adv_tick++;
            if (s_adv_tick >= (BLE_PROV_ADV_TIMEOUT_SEC * 100U)) {
                s_adv_tick = 0;
                ESP_LOGW(TAG, "BLE advertising timed out after %ds — stopping", BLE_PROV_ADV_TIMEOUT_SEC);
                esp_ble_gap_stop_advertising();
                // set_prov_state already fires s_state_callback — do NOT call it again
                set_prov_state(EB_BLE_PROV_STATE_STOPPED);
            }
        } else {
            s_adv_tick = 0; // reset if connected or not advertising
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// GAP Event Handler
// ============================================================================

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= (~ADV_CONFIG_FLAG);
            ESP_LOGD(TAG, "ADV data configured (status=0x%x, config_done=0x%x)",
                     param->adv_data_cmpl.status, s_adv_config_done);
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            s_adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
            ESP_LOGD(TAG, "Scan RSP configured (status=0x%x, config_done=0x%x)",
                     param->scan_rsp_data_cmpl.status, s_adv_config_done);
            // Advertising is started explicitly in eb_ble_prov_start_with_status()
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising start failed: 0x%x", param->adv_start_cmpl.status);
            } else {
                ESP_LOGI(TAG, "Advertising started");
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising stop failed");
            } else {
                ESP_LOGI(TAG, "Advertising stopped");
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGD(TAG, "Conn params applied: interval=%d (%.1fms), latency=%d, timeout=%d",
                     param->update_conn_params.conn_int,
                     param->update_conn_params.conn_int * 1.25f,
                     param->update_conn_params.latency,
                     param->update_conn_params.timeout);
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
            ESP_LOGD(TAG, "Numeric comparison: %" PRIu32, param->ble_security.key_notif.passkey);
            break;

        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            ESP_LOGD(TAG, "Passkey request event");
            break;

        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            ESP_LOGD(TAG, "Passkey notify: %06" PRIu32, param->ble_security.key_notif.passkey);
            break;

        case ESP_GAP_BLE_KEY_EVT:
            ESP_LOGD(TAG, "BLE key event, type=%d", param->ble_security.ble_key.key_type);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            esp_bd_addr_t bd_addr;
            memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "Auth complete: %s, addr_type=%d",
                     param->ble_security.auth_cmpl.success ? "success" : "fail",
                     param->ble_security.auth_cmpl.addr_type);

            if (!param->ble_security.auth_cmpl.success) {
                ESP_LOGW(TAG, "Auth fail reason: 0x%x", param->ble_security.auth_cmpl.fail_reason);
                s_pair_status = 0;
                esp_ble_gap_disconnect(bd_addr);
                if (param->ble_security.auth_cmpl.addr_type != 1) {
                    remove_all_bonded_devices();
                    s_pair_status = 2;
                }
            } else {
                s_pair_status = 1;
                set_prov_state(EB_BLE_PROV_STATE_PAIRED);
            }
            show_bonded_devices();
            break;
        }

        case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
            ESP_LOGD(TAG, "Bond device removed");
            break;

        case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
            if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Local privacy config failed: 0x%x", param->local_privacy_cmpl.status);
                break;
            }
            ESP_LOGD(TAG, "Local privacy configured, setting up advertising...");
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret) {
                ESP_LOGE(TAG, "Config adv data failed: 0x%x", ret);
            } else {
                s_adv_config_done |= ADV_CONFIG_FLAG;
            }
            ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
            if (ret) {
                ESP_LOGE(TAG, "Config scan rsp failed: 0x%x", ret);
            } else {
                s_adv_config_done |= SCAN_RSP_CONFIG_FLAG;
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// GATTS Profile Event Handler
// ============================================================================

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
                                        esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param)

{
    switch (event) {
        case ESP_GATTS_REG_EVT: {
            ESP_LOGD(TAG, "GATTS REG, app_id=0x%04x, status=%d", param->reg.app_id, param->reg.status);

            // Generate device name based on provisioning status
            generate_device_name(s_current_prov_status);

            esp_err_t set_name_ret = esp_ble_gap_set_device_name(s_device_name);
            if (set_name_ret) {
                ESP_LOGE(TAG, "Set device name failed: 0x%x", set_name_ret);
            }

            esp_ble_gap_config_local_privacy(true);

            esp_err_t create_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HRS_IDX_NB, SVC_INST_ID);
            if (create_ret) {
                ESP_LOGE(TAG, "Create attr table failed: 0x%x", create_ret);
            }
            break;
        }

        case ESP_GATTS_READ_EVT:
            break;

        case ESP_GATTS_WRITE_EVT: {
            ESP_LOGD(TAG, "WRITE_EVT: handle=0x%04x len=%d is_prep=%d",
                     param->write.handle, param->write.len, param->write.is_prep);
            if (!param->write.is_prep) {
                // Direct write (fits in single BLE packet)
                char data_buf[GATTS_CHAR_VAL_LEN_MAX + 1];
                int copy_len = param->write.len < GATTS_CHAR_VAL_LEN_MAX
                    ? param->write.len : GATTS_CHAR_VAL_LEN_MAX;
                memcpy(data_buf, param->write.value, copy_len);
                data_buf[copy_len] = '\0';

                // Char A: Authentication key
                if (s_handle_table[IDX_CHAR_VAL_A] == param->write.handle) {
                    if (strcmp(data_buf, EB_BLE_PROV_AUTH_KEY) == 0 && s_pair_status == 1) {
                        ESP_LOGI(TAG, "BLAZE auth successful");
                        s_ble_auth_key = 2;
                        s_onboard_count = 0;
                        set_prov_state(EB_BLE_PROV_STATE_AUTHENTICATED);
                    } else {
                        ESP_LOGW(TAG, "Auth failed: wrong key or not paired");
                        s_ble_auth_key = 1;
                    }
                }
                // Char B: Data (WiFi credentials / commands)
                else if (s_handle_table[IDX_CHAR_VAL_B] == param->write.handle) {
                    ESP_LOGD(TAG, "Char B write: len=%d auth=%d pair=%d",
                             param->write.len, s_ble_auth_key, s_pair_status);
                    if (s_ble_auth_key == 2 && s_pair_status == 1) {
                        process_wifi_credentials(data_buf);
                    } else {
                        ESP_LOGW(TAG, "Data write rejected: not authenticated");
                        s_ble_auth_key = 1;
                    }
                }
                // Char A CCCD
                else if (s_handle_table[IDX_CHAR_CFG_A] == param->write.handle && param->write.len == 2) {
                    uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];
                    ESP_LOGD(TAG, "CCCD A: 0x%04x (%s)", descr_value,
                             descr_value == 0x0001 ? "notify" : descr_value == 0x0002 ? "indicate" : "off");
                }
                // Char B CCCD
                else if (s_handle_table[IDX_CHAR_CFG_B] == param->write.handle && param->write.len == 2) {
                    uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];
                    ESP_LOGD(TAG, "CCCD B: 0x%04x (%s)", descr_value,
                             descr_value == 0x0001 ? "notify" : descr_value == 0x0002 ? "indicate" : "off");
                }

                // Send response if needed
                if (param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                 param->write.trans_id, ESP_GATT_OK, NULL);
                }
            } else {
                // Prepare write (long data spanning multiple packets)
                prepare_write_event(gatts_if, &s_prepare_write_env, param);
            }
            break;
        }

        case ESP_GATTS_EXEC_WRITE_EVT:
            ESP_LOGD(TAG, "EXEC_WRITE_EVT: flag=%d",
                     param->exec_write.exec_write_flag);
            exec_write_event(&s_prepare_write_env, param);
            break;

        case ESP_GATTS_MTU_EVT:
            ESP_LOGD(TAG, "MTU negotiated: %d bytes", param->mtu.mtu);
            break;

        case ESP_GATTS_CONF_EVT:
            break;

        case ESP_GATTS_START_EVT:
            ESP_LOGD(TAG, "Service started, handle=%d", param->start.service_handle);
            break;

        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "Client connected, conn_id=%d", param->connect.conn_id);
            esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
            ESP_LOG_BUFFER_HEX(TAG, param->connect.remote_bda, 6);
            s_gatts_if = gatts_if;
            s_conn_id = param->connect.conn_id;
            memcpy(s_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            s_onboard_count = 1;
            set_prov_state(EB_BLE_PROV_STATE_CONNECTED);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Client disconnected, reason=0x%x", param->disconnect.reason);

            if (s_pair_status == 2) {
                // Address mismatch: save status and restart
                eb_nvs_set_pair_status(1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGW(TAG, "Pair status=2, restarting...");
                esp_restart();
            }

            s_pair_status = 0;
            s_onboard_count = 0;
            s_adv_flag = false;

            // Restart advertising
            s_adv_flag = true;
            esp_ble_gap_start_advertising(&adv_params);
            set_prov_state(EB_BLE_PROV_STATE_ADVERTISING);

            uint32_t prov_time = 0;
            eb_nvs_get_prov_time(&prov_time);
            s_ble_provision_time = prov_time;
            s_ble_auth_key = 0;
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Create attr table failed: 0x%x", param->add_attr_tab.status);
            } else if (param->add_attr_tab.num_handle != HRS_IDX_NB) {
                ESP_LOGE(TAG, "Attr table handle count mismatch: %d != %d",
                         param->add_attr_tab.num_handle, HRS_IDX_NB);
            } else {
                memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
                esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
            }
            break;

        case ESP_GATTS_STOP_EVT:
        case ESP_GATTS_OPEN_EVT:
        case ESP_GATTS_CANCEL_OPEN_EVT:
        case ESP_GATTS_CLOSE_EVT:
        case ESP_GATTS_LISTEN_EVT:
        case ESP_GATTS_CONGEST_EVT:
        case ESP_GATTS_UNREG_EVT:
        case ESP_GATTS_DELETE_EVT:
        default:
            break;
    }
}

// ============================================================================
// GATTS Event Dispatcher
// ============================================================================

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            s_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        } else {
            ESP_LOGE(TAG, "GATTS app register failed: status=%d, app_id=0x%04x",
                     param->reg.status, param->reg.app_id);
            return;
        }
    }

    for (int idx = 0; idx < PROFILE_NUM; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE || gatts_if == s_profile_tab[idx].gatts_if) {
            if (s_profile_tab[idx].gatts_cb) {
                s_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

// ============================================================================
// Public API: Initialize
// ============================================================================

esp_err_t eb_ble_prov_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing BLE provisioning (Bluedroid)...");

    // Release classic BT memory
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize Bluedroid
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register callbacks
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GATTS register callback failed: 0x%x", ret);
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GAP register callback failed: 0x%x", ret);
        return ret;
    }

    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "GATTS app register failed: 0x%x", ret);
        return ret;
    }

    // Configure security
    enable_pairing_security();

    // Register WiFi disconnect handler for provisioning
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               &wifi_disconnect_handler, NULL);

    // Create verify_connection task
    xTaskCreate(verify_connection_task, "verify_conn", 1024 * 4, NULL,
                configMAX_PRIORITIES - 4, NULL);

    // Check pair status from NVS (handle addr-mismatch recovery)
    uint8_t pair_st = 0;
    if (eb_nvs_get_pair_status(&pair_st) == ESP_OK && pair_st == 1) {
        ESP_LOGW(TAG, "Previous pair failure detected, removing bonded devices");
        eb_nvs_set_pair_status(0);
        remove_all_bonded_devices();
    }

    s_initialized = true;
    set_prov_state(EB_BLE_PROV_STATE_STARTING);

    ESP_LOGI(TAG, "BLE provisioning initialized");
    return ESP_OK;
}

// ============================================================================
// Public API: Deinitialize
// ============================================================================

esp_err_t eb_ble_prov_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing BLE provisioning...");

    eb_ble_prov_stop();

    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                 &wifi_disconnect_handler);

    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_initialized = false;
    s_prov_state = EB_BLE_PROV_STATE_IDLE;

    ESP_LOGI(TAG, "BLE provisioning deinitialized");
    return ESP_OK;
}

// ============================================================================
// Public API: Start
// ============================================================================

esp_err_t eb_ble_prov_start(void)
{
    uint8_t prov_status = 0;
    eb_nvs_get_prov_status(&prov_status);
    return eb_ble_prov_start_with_status((eb_prov_status_t)prov_status);
}

esp_err_t eb_ble_prov_start_with_status(eb_prov_status_t status)
{
    s_current_prov_status = status;
    s_change_ap = (status == EB_PROV_STATUS_CHANGE_AP);

    ESP_LOGD(TAG, "Starting BLE provisioning (status=%d, change_ap=%d)",
             status, s_change_ap);

    if (!s_initialized) {
        esp_err_t err = eb_ble_prov_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    // If already advertising, just update and restart
    if (s_prov_state == EB_BLE_PROV_STATE_ADVERTISING) {
        return ESP_OK;
    }

    // Start advertising (adv data already configured during init)
    s_adv_flag = true;
    if (status == EB_PROV_STATUS_TIMED) {
        eb_nvs_get_prov_time(&s_ble_provision_time);
    }

    ESP_LOGI(TAG, "Starting advertising now (prov_status=%d)...", status);
    esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start advertising failed: 0x%x", ret);
        return ret;
    }
    set_prov_state(EB_BLE_PROV_STATE_ADVERTISING);

    return ESP_OK;
}

// ============================================================================
// Public API: Stop
// ============================================================================

esp_err_t eb_ble_prov_stop(void)
{
    if (s_prov_state == EB_BLE_PROV_STATE_IDLE ||
        s_prov_state == EB_BLE_PROV_STATE_STOPPED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping BLE provisioning...");
    esp_ble_gap_stop_advertising();
    set_prov_state(EB_BLE_PROV_STATE_STOPPED);

    return ESP_OK;
}

// ============================================================================
// Public API: Queries
// ============================================================================

bool eb_ble_prov_is_active(void)
{
    return (s_prov_state == EB_BLE_PROV_STATE_ADVERTISING ||
            s_prov_state == EB_BLE_PROV_STATE_CONNECTED ||
            s_prov_state == EB_BLE_PROV_STATE_PAIRED ||
            s_prov_state == EB_BLE_PROV_STATE_AUTHENTICATED ||
            s_prov_state == EB_BLE_PROV_STATE_PROVISIONING);
}

eb_ble_prov_state_t eb_ble_prov_get_state(void)
{
    return s_prov_state;
}

esp_err_t eb_ble_prov_get_device_name(char *name, size_t len)
{
    if (!name || len == 0) return ESP_ERR_INVALID_ARG;

    if (s_device_name[0] == '\0') {
        generate_device_name(s_current_prov_status);
    }

    strncpy(name, s_device_name, len - 1);
    name[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t eb_ble_prov_notify_client(const char *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    client_notify(data);
    return ESP_OK;
}

// ============================================================================
// Public API: Callbacks
// ============================================================================

void eb_ble_prov_set_state_callback(eb_ble_prov_state_cb_t callback)
{
    s_state_callback = callback;
}

void eb_ble_prov_set_success_callback(eb_ble_prov_success_cb_t callback)
{
    s_success_callback = callback;
}
