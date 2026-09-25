/**
 * @file eb_wifi.c
 * @brief WiFi Manager Implementation
 *
 * @company Elecbits
 */

#include "eb_wifi.h"
#include "eb_nvs.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include <string.h>

static const char *TAG = "eb_wifi";

// ============================================================================
// State Variables
// ============================================================================

static bool s_initialized = false;
static esp_netif_t *s_sta_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;

// Event bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_GOT_IP_BIT BIT2

// State tracking
static volatile eb_wifi_state_t s_wifi_state = EB_WIFI_STATE_DISCONNECTED;
static volatile uint8_t s_retry_count = 0;
static char s_current_ssid[33] = {0};
static uint32_t s_ip_addr = 0;
static bool s_save_on_connect = false;
static char s_pending_ssid[33] = {0};
static char s_pending_password[65] = {0};

// Callbacks
static eb_wifi_state_cb_t s_state_callback = NULL;
static eb_wifi_ip_cb_t s_ip_callback = NULL;

// Reconnect timer — fires after max retries to retry WiFi periodically
static TimerHandle_t s_reconnect_timer = NULL;
#define WIFI_RECONNECT_INTERVAL_MS 30000 // 30 seconds between retry bursts

// Connectivity
SemaphoreHandle_t g_tls_handshake_mutex = NULL;
EventGroupHandle_t g_connectivity = NULL;
static eb_connectivity_cb_t s_connectivity_cb = NULL;

// ============================================================================
// Forward Declarations
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void set_wifi_state(eb_wifi_state_t new_state);

/**
 * @brief Timer callback: retry WiFi connection after max retries backoff.
 * Runs in timer-daemon context.
 */
static void wifi_reconnect_timer_cb(TimerHandle_t xTimer) {
  (void)xTimer;
  ESP_LOGI(TAG, "Reconnect timer fired — retrying WiFi connection");
  s_retry_count = 0;
  set_wifi_state(EB_WIFI_STATE_CONNECTING);
  esp_wifi_connect();
}

// ============================================================================
// Event Handlers
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_STA_START) {
    ESP_LOGD(TAG, "WiFi STA started");
    // Don't auto-connect here, wait for explicit connect call

  } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    wifi_event_sta_connected_t *event =
        (wifi_event_sta_connected_t *)event_data;
    memcpy(s_current_ssid, event->ssid, event->ssid_len);
    s_current_ssid[event->ssid_len] = '\0';
    ESP_LOGI(TAG, "Connected to SSID: %s", s_current_ssid);
    set_wifi_state(EB_WIFI_STATE_CONNECTED);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    s_retry_count = 0;

    // Stop the reconnect timer — we are connected now
    if (s_reconnect_timer && xTimerIsTimerActive(s_reconnect_timer)) {
      xTimerStop(s_reconnect_timer, 0);
    }

  } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "Disconnected from WiFi (reason: %d)", event->reason);

    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
    s_ip_addr = 0;

    // Handle reconnection
    if (s_retry_count < EB_WIFI_MAX_RETRY) {
      s_retry_count++;
      set_wifi_state(EB_WIFI_STATE_CONNECTING);
      ESP_LOGD(TAG, "Retry %d/%d...", s_retry_count, EB_WIFI_MAX_RETRY);
      esp_wifi_connect();
    } else {
      ESP_LOGE(TAG, "Max retries reached — will retry in %d ms",
               WIFI_RECONNECT_INTERVAL_MS);
      set_wifi_state(EB_WIFI_STATE_ERROR);
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

      // Start (or restart) the backoff timer so we keep trying
      if (s_reconnect_timer) {
        xTimerReset(s_reconnect_timer, 0);
      }
    }

  } else if (event_id == WIFI_EVENT_SCAN_DONE) {
    ESP_LOGD(TAG, "WiFi scan completed");
  }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
  if (event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    s_ip_addr = event->ip_info.ip.addr;
    ESP_LOGI(TAG, "Got IP: " IPSTR,
             IP2STR(&event->ip_info.ip)); // Keep this - important

    set_wifi_state(EB_WIFI_STATE_GOT_IP);
    xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);

    // Save credentials if requested
    if (s_save_on_connect && strlen(s_pending_ssid) > 0) {
      eb_wifi_credentials_t creds = {0};
      strncpy(creds.ssid, s_pending_ssid, EB_WIFI_SSID_MAX_LEN);
      strncpy(creds.password, s_pending_password, EB_WIFI_PASSWORD_MAX_LEN);
      creds.configured = true;

      esp_err_t err = eb_nvs_save_wifi_credentials(&creds);
      if (err == ESP_OK) {
        ESP_LOGD(TAG, "WiFi credentials saved to NVS");
      } else {
        ESP_LOGW(TAG, "Failed to save WiFi credentials: %s",
                 esp_err_to_name(err));
      }

      // Clear pending credentials
      memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
      memset(s_pending_password, 0, sizeof(s_pending_password));
      s_save_on_connect = false;
    }

    // Call IP callback
    if (s_ip_callback != NULL) {
      s_ip_callback(s_ip_addr);
    }

  } else if (event_id == IP_EVENT_STA_LOST_IP) {
    ESP_LOGW(TAG, "Lost IP address");
    s_ip_addr = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    set_wifi_state(EB_WIFI_STATE_CONNECTED);
  }
}

// ============================================================================
// Internal Functions
// ============================================================================

static void _fire_connectivity_cb(void) {
  if (!s_connectivity_cb || !g_connectivity)
    return;
  EventBits_t b = xEventGroupGetBits(g_connectivity);
  s_connectivity_cb(!!(b & EB_CONN_WIFI_BIT), !!(b & EB_CONN_AZURE_BIT));
}

static void set_wifi_state(eb_wifi_state_t new_state) {
  if (s_wifi_state != new_state) {
    eb_wifi_state_t old_state = s_wifi_state;
    s_wifi_state = new_state;

    const char *state_names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED",
                                 "GOT_IP", "ERROR"};
    ESP_LOGD(TAG, "State: %s -> %s", state_names[old_state],
             state_names[new_state]);

    if (s_state_callback != NULL) {
      s_state_callback(old_state, new_state);
    }

    if (g_connectivity) {
      if (new_state == EB_WIFI_STATE_GOT_IP) {
        xEventGroupSetBits(g_connectivity, EB_CONN_WIFI_BIT);
        _fire_connectivity_cb();
      } else if (new_state == EB_WIFI_STATE_DISCONNECTED ||
                 new_state == EB_WIFI_STATE_ERROR) {
        xEventGroupClearBits(g_connectivity,
                             EB_CONN_WIFI_BIT | EB_CONN_AZURE_BIT);
        _fire_connectivity_cb();
      }
    }
  }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t eb_wifi_init(void) {
  if (s_initialized) {
    ESP_LOGW(TAG, "WiFi already initialized");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Initializing WiFi...");

  // Create event group
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    ESP_LOGE(TAG, "Failed to create event group");
    return ESP_ERR_NO_MEM;
  }

  // Create reconnect timer (one-shot; reset on each max-retry event)
  s_reconnect_timer =
      xTimerCreate("wifi_reconn", pdMS_TO_TICKS(WIFI_RECONNECT_INTERVAL_MS),
                   pdFALSE, NULL, wifi_reconnect_timer_cb);
  if (s_reconnect_timer == NULL) {
    ESP_LOGW(TAG, "Failed to create reconnect timer (non-fatal)");
  }

  // Create TLS handshake mutex and connectivity event group
  g_tls_handshake_mutex = xSemaphoreCreateBinary();
  if (g_tls_handshake_mutex)
    xSemaphoreGive(g_tls_handshake_mutex);

  g_connectivity = xEventGroupCreate();

  // Initialize TCP/IP stack
  ESP_ERROR_CHECK(esp_netif_init());

  // Create default event loop (may already exist)
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
    return err;
  }

  // Create default WiFi STA netif
  s_sta_netif = esp_netif_create_default_wifi_sta();
  if (s_sta_netif == NULL) {
    ESP_LOGE(TAG, "Failed to create WiFi STA netif");
    return ESP_FAIL;
  }

  // Initialize WiFi with default config
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Register event handlers
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

  // Set WiFi mode to STA
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // Start WiFi
  ESP_ERROR_CHECK(esp_wifi_start());

   esp_wifi_set_max_tx_power(64);      // Reduce TX power → less current spike
   esp_wifi_set_ps(WIFI_PS_NONE); // Modem sleep → reduces average current

  s_initialized = true;
  ESP_LOGI(TAG, "WiFi initialized successfully");

  return ESP_OK;
}

esp_err_t eb_wifi_deinit(void) {
  if (!s_initialized) {
    return ESP_OK;
  }

  esp_wifi_stop();
  esp_wifi_deinit();

  if (s_wifi_event_group != NULL) {
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;
  }

  if (s_sta_netif != NULL) {
    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = NULL;
  }

  s_initialized = false;
  s_wifi_state = EB_WIFI_STATE_DISCONNECTED;

  ESP_LOGI(TAG, "WiFi deinitialized");
  return ESP_OK;
}

esp_err_t eb_wifi_connect(void) {
  if (!s_initialized) {
    ESP_LOGE(TAG, "WiFi not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  // Load credentials from NVS
  eb_wifi_credentials_t creds;
  esp_err_t err = eb_nvs_load_wifi_credentials(&creds);
  if (err != ESP_OK || !creds.configured) {
    ESP_LOGW(TAG, "No WiFi credentials stored");
    return ESP_ERR_NOT_FOUND;
  }

  return eb_wifi_connect_with_credentials(creds.ssid, creds.password, false);
}

esp_err_t eb_wifi_connect_with_credentials(const char *ssid,
                                           const char *password,
                                           bool save_to_nvs) {
  if (!s_initialized) {
    ESP_LOGE(TAG, "WiFi not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (ssid == NULL || strlen(ssid) == 0) {
    ESP_LOGE(TAG, "Invalid SSID");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

  // Store pending credentials if save requested
  if (save_to_nvs) {
    s_save_on_connect = true;
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    if (password != NULL) {
      strncpy(s_pending_password, password, sizeof(s_pending_password) - 1);
    }
  }

  // Configure WiFi
  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
  if (password != NULL) {
    strncpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);
  }

  // Set security threshold
  wifi_config.sta.threshold.authmode =
      (password != NULL && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK
                                                 : WIFI_AUTH_OPEN;

  // Enable PMF (Protected Management Frames)
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  // Reset retry count and state
  s_retry_count = 0;
  set_wifi_state(EB_WIFI_STATE_CONNECTING);

  // Start connection — if WiFi was stopped (e.g. after previous auth fail
  // during provisioning), restart it first before connecting.
  esp_err_t start_err = esp_wifi_start();
  if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
    // ESP_ERR_INVALID_STATE means it was already started — that's fine
    ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
    set_wifi_state(EB_WIFI_STATE_ERROR);
    return start_err;
  }

  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    set_wifi_state(EB_WIFI_STATE_ERROR);
    return err;
  }

  return ESP_OK;
}

esp_err_t eb_wifi_disconnect(void) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  s_retry_count = EB_WIFI_MAX_RETRY; // Prevent auto-reconnect
  esp_err_t err = esp_wifi_disconnect();
  set_wifi_state(EB_WIFI_STATE_DISCONNECTED);

  return err;
}

bool eb_wifi_is_connected(void) { return s_wifi_state == EB_WIFI_STATE_GOT_IP; }

esp_err_t eb_wifi_get_status(eb_wifi_status_t *status) {
  if (status == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  status->state = s_wifi_state;
  strncpy(status->ssid, s_current_ssid, sizeof(status->ssid));
  status->ip_addr = s_ip_addr;
  status->retry_count = s_retry_count;
  status->credentials_valid = eb_nvs_wifi_credentials_exist();

  // Get RSSI if connected
  if (s_wifi_state == EB_WIFI_STATE_CONNECTED ||
      s_wifi_state == EB_WIFI_STATE_GOT_IP) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      status->rssi = ap_info.rssi;
    } else {
      status->rssi = 0;
    }
  } else {
    status->rssi = 0;
  }

  return ESP_OK;
}

eb_wifi_state_t eb_wifi_get_state(void) { return s_wifi_state; }

int8_t eb_wifi_get_rssi(void) {
  if (s_wifi_state != EB_WIFI_STATE_CONNECTED &&
      s_wifi_state != EB_WIFI_STATE_GOT_IP) {
    return 0;
  }

  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return ap_info.rssi;
  }

  return 0;
}

void eb_wifi_set_state_callback(eb_wifi_state_cb_t callback) {
  s_state_callback = callback;
}

void eb_wifi_set_ip_callback(eb_wifi_ip_cb_t callback) {
  s_ip_callback = callback;
}

bool eb_wifi_has_credentials(void) { return eb_nvs_wifi_credentials_exist(); }

esp_err_t eb_wifi_erase_credentials(void) {
  return eb_nvs_erase_wifi_credentials();
}

esp_err_t eb_wifi_scan_start(uint16_t max_ap) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = true,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active.min = 100,
      .scan_time.active.max = 300,
  };

  return esp_wifi_scan_start(&scan_config, false);
}

esp_err_t eb_wifi_scan_get_results(wifi_ap_record_t *ap_records,
                                   uint16_t *num_records) {
  if (ap_records == NULL || num_records == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  return esp_wifi_scan_get_ap_records(num_records, ap_records);
}

// ============================================================================
// Connectivity API
// ============================================================================

void eb_wifi_set_connectivity_callback(eb_connectivity_cb_t cb) {
  s_connectivity_cb = cb;
}

void eb_connectivity_notify_azure(bool up) {
  if (!g_connectivity)
    return;
  if (up)
    xEventGroupSetBits(g_connectivity, EB_CONN_AZURE_BIT);
  else
    xEventGroupClearBits(g_connectivity, EB_CONN_AZURE_BIT);
  ESP_LOGI(TAG, "Azure %s", up ? "CONNECTED" : "DISCONNECTED");
  _fire_connectivity_cb();
}

bool eb_connectivity_is_online(void) {
  if (!g_connectivity)
    return false;
  EventBits_t b = xEventGroupGetBits(g_connectivity);
  return (b & (EB_CONN_WIFI_BIT | EB_CONN_AZURE_BIT)) ==
         (EB_CONN_WIFI_BIT | EB_CONN_AZURE_BIT);
}

bool eb_connectivity_is_wifi_up(void) {
  if (!g_connectivity)
    return false;
  return !!(xEventGroupGetBits(g_connectivity) & EB_CONN_WIFI_BIT);
}
