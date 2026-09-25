#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

/* TODO(WIP): eb_device_registry.h does not exist yet — this offline-mode
 * feature is incomplete and intentionally excluded from the build. See
 * README.md "Known limitations". */
#include "eb_device_registry.h"
#include "eb_offline.h"
#include "eb_wifi.h"

static const char *TAG = "EB_OFFLINE";

// Internal Handshake Mutex (Shared with other TLS tasks)
extern SemaphoreHandle_t g_tls_handshake_mutex;

// Persistent connection management
#define MAX_PERSISTENT_CONNS 2

typedef struct {
  esp_websocket_client_handle_t client;
  char device_id[64];
  bool connected;
  bool authenticated;
  uint32_t last_activity;
  int slot_index;
} persistent_conn_t;

static persistent_conn_t conns[MAX_PERSISTENT_CONNS];

typedef struct {
  char device_id[64];
  uint8_t ctl_type;
  bool state;
} offline_cmd_t;

static QueueHandle_t cmd_queue;

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                             void *event_data) {
  persistent_conn_t *c = (persistent_conn_t *)arg;
  if (!c)
    return;
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    c->connected = true;
    c->authenticated = false;
    ESP_LOGI(TAG, "[Slot %d] Socket Connected", c->slot_index);
    break;
  case WEBSOCKET_EVENT_DATA:
    if (data->data_len >= 7 &&
        strncmp((char *)data->data_ptr, "Success", 7) == 0) {
      c->authenticated = true;
      ESP_LOGI(TAG, "[Slot %d] Auth Success", c->slot_index);
    }
    break;
  case WEBSOCKET_EVENT_DISCONNECTED:
    c->connected = false;
    c->authenticated = false;
    ESP_LOGW(TAG, "[Slot %d] Socket Disconnected", c->slot_index);
    break;
  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGE(TAG, "[Slot %d] Error Event", c->slot_index);
    break;
  }
}

static void generate_auth_header(char *buffer, size_t len,
                                 eb_device_entry_t *dev) {
  char date_str[16];
  time_t now_t = time(NULL);
  if (now_t < 1000000000L) {
    strncpy(date_str, "09-03-2026", sizeof(date_str));
  } else {
    struct tm t;
    gmtime_r(&now_t, &t);
    strftime(date_str, sizeof(date_str), "%d-%m-%Y", &t);
  }
  char secret_key[128], mac_upper[16];
  strncpy(mac_upper, dev->mac, 15);
  mac_upper[15] = '\0';
  for (int i = 0; mac_upper[i]; i++)
    if (mac_upper[i] >= 'a' && mac_upper[i] <= 'z')
      mac_upper[i] -= 32;
  snprintf(secret_key, sizeof(secret_key), "%s|%s_", mac_upper, dev->device_id);

  unsigned char hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret_key,
                         strlen(secret_key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)date_str,
                         strlen(date_str));
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  char hex[65] = {0};
  for (int i = 0; i < 32; i++)
    sprintf(hex + (i * 2), "%02x", hmac[i]);
  snprintf(buffer, len, "auth:%s:%s", date_str, hex);
}

static void offline_control_task(void *arg) {
  offline_cmd_t cmd;

  ESP_LOGI(TAG, "Offline Task Running [Dual-Link Mode]. Initial Heap: %lu",
           (unsigned long)esp_get_free_heap_size());

  for (int i = 0; i < MAX_PERSISTENT_CONNS; i++) {
    memset(&conns[i], 0, sizeof(persistent_conn_t));
    conns[i].slot_index = i;
  }

  while (1) {
    if (xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (!eb_connectivity_is_wifi_up()) {
        ESP_LOGE(TAG, "WiFi down. Command %s aborted.", cmd.device_id);
        continue;
      }

      int slot = -1;
      for (int i = 0; i < MAX_PERSISTENT_CONNS; i++) {
        if (conns[i].client && strcmp(conns[i].device_id, cmd.device_id) == 0) {
          slot = i;
          break;
        }
      }

      if (slot != -1 && conns[slot].client) {
        if (!conns[slot].connected || !conns[slot].authenticated) {
          ESP_LOGW(TAG, "[Slot %d] Re-opening dead link to %s", slot,
                   cmd.device_id);
          esp_websocket_client_stop(conns[slot].client);
          esp_websocket_client_destroy(conns[slot].client);
          conns[slot].client = NULL;
          conns[slot].connected = false;
          conns[slot].authenticated = false;
        }
      }

      if (slot == -1 || !conns[slot].client) {
        if (slot == -1) {
          for (int i = 0; i < MAX_PERSISTENT_CONNS; i++) {
            if (!conns[i].client) {
              slot = i;
              break;
            }
          }
        }
        if (slot == -1) {
          uint32_t oldest = 0xFFFFFFFF;
          for (int i = 0; i < MAX_PERSISTENT_CONNS; i++) {
            if (conns[i].last_activity < oldest) {
              oldest = conns[i].last_activity;
              slot = i;
            }
          }
          ESP_LOGI(TAG, "Evicting Slot %d for %s", slot, cmd.device_id);
          esp_websocket_client_stop(conns[slot].client);
          esp_websocket_client_destroy(conns[slot].client);
          conns[slot].client = NULL;
          conns[slot].connected = false;
          conns[slot].authenticated = false;
        }

        eb_device_entry_t *dev =
            eb_device_registry_get_by_device_id(cmd.device_id);
        if (dev) {
          if (esp_get_free_heap_size() < 42000) {
            ESP_LOGE(TAG, "Memory low (%lu). Connection skipped.",
                     (unsigned long)esp_get_free_heap_size());
            continue;
          }

          char uri[128], auth[256];
          snprintf(uri, sizeof(uri), "wss://%s.local:443/ws", dev->device_id);
          generate_auth_header(auth, sizeof(auth), dev);

          ESP_LOGI(TAG, "[Slot %d] Connecting to %s", slot, cmd.device_id);
          esp_websocket_client_config_t cfg = {
              .uri = uri,
              .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
              .buffer_size = 512,
              .task_stack = 4096,
              .skip_cert_common_name_check = true,
              .keep_alive_enable = true,
              .ping_interval_sec = 20,
              .reconnect_timeout_ms = 10000,
          };

          conns[slot].client = esp_websocket_client_init(&cfg);
          if (conns[slot].client) {
            strncpy(conns[slot].device_id, cmd.device_id, 63);
            esp_websocket_client_append_header(conns[slot].client, "hmac_token",
                                               auth);
            esp_websocket_register_events(conns[slot].client,
                                          WEBSOCKET_EVENT_ANY, ws_event_handler,
                                          &conns[slot]);

            if (g_tls_handshake_mutex)
              xSemaphoreTake(g_tls_handshake_mutex, pdMS_TO_TICKS(15000));
            esp_websocket_client_start(conns[slot].client);

            int cycles = 0;
            while (!conns[slot].authenticated && cycles++ < 120)
              vTaskDelay(pdMS_TO_TICKS(100));
            if (g_tls_handshake_mutex)
              xSemaphoreGive(g_tls_handshake_mutex);
          }
        }
      }

      if (slot != -1 && !conns[slot].authenticated) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // Grace period
      }

      if (slot != -1 && conns[slot].authenticated) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id", cmd.device_id);
        cJSON_AddStringToObject(root, "cmd_type", "SwitchEvent");
        cJSON *datap = cJSON_CreateObject();
        cJSON_AddStringToObject(datap, "state", cmd.state ? "on" : "off");
        cJSON_AddNumberToObject(datap, "ctl_type", (int)cmd.ctl_type);
        cJSON_AddItemToObject(root, "data", datap);

        char *json = cJSON_PrintUnformatted(root);
        esp_websocket_client_send_text(conns[slot].client, json, strlen(json),
                                       pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, ">>> SUCCESS: Sent to %s", cmd.device_id);
        free(json);
        cJSON_Delete(root);
        conns[slot].last_activity = xTaskGetTickCount();
      } else {
        ESP_LOGE(TAG, ">>> FAIL: %s session failed", cmd.device_id);
      }
    }
  }
}

void eb_offline_init(void) {
  cmd_queue = xQueueCreate(10, sizeof(offline_cmd_t));
  xTaskCreate(offline_control_task, "offline_ctl", 8192, NULL, 5, NULL);
}

void eb_offline_send_device_cmd(const char *device_id, uint8_t ctl_type,
                                bool state) {
  offline_cmd_t cmd;
  strncpy(cmd.device_id, device_id, 63);
  cmd.ctl_type = ctl_type;
  cmd.state = state;
  xQueueSend(cmd_queue, &cmd, 0);
}
