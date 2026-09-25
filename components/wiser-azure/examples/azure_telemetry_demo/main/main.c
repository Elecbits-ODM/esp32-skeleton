/**
 * @file main.c
 * @brief eb_azure telemetry demo
 *
 * Minimal example showing the real production init sequence used by
 * consumers of eb_azure: bring up storage + WiFi (via the sibling
 * eb_storage / eb_network components), start Azure IoT, wait for the
 * initial provisioning attempt, then periodically send telemetry.
 *
 * WiFi bring-up itself is intentionally brief here — see the eb_network
 * component's own example for the full connect/retry flow. This demo
 * focuses on the eb_azure API surface.
 */

#include "eb_azure_iot.h"
#include "eb_nvs.h"
#include "eb_wifi.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "azure_demo";

/* Forward the eb_network WiFi state change into eb_azure so it can pause/
 * resume its reconnect loop while WiFi is down. See eb_network's own
 * example for how on_wifi_state_changed is normally wired up in full. */
static void on_wifi_state_changed(eb_wifi_state_t old_state,
                                   eb_wifi_state_t new_state) {
  (void)old_state;
  bool connected = (new_state == EB_WIFI_STATE_GOT_IP);
  ESP_LOGI(TAG, "WiFi state changed: %s", connected ? "connected" : "lost");
  eb_azure_iot_notify_wifi_state(connected);
}

/* Handle Cloud-to-Device messages that eb_azure_iot doesn't already handle
 * internally (restart / delete / get_device_details / AddAck). */
static void on_azure_c2d_message(const char *msg_type, const cJSON *payload) {
  ESP_LOGI(TAG, "C2D message received, msg_type=%s", msg_type);
  (void)payload; /* Inspect fields with cJSON_GetObjectItem() as needed */
}

void app_main(void) {
  // --- Storage + WiFi bring-up (provided by eb_storage / eb_network) -------
  eb_nvs_init();
  eb_wifi_init();
  eb_wifi_set_state_callback(on_wifi_state_changed);
  eb_wifi_connect(); // Connects using credentials cached by eb_storage/eb_network
                      // (see eb_network's example for provisioning credentials)

  // Give the station a moment to associate before starting Azure IoT.
  // A production app should instead wait on eb_network's own "connected"
  // event/callback rather than a fixed delay.
  vTaskDelay(pdMS_TO_TICKS(2000));

  // --- Azure IoT ------------------------------------------------------------
  if (eb_azure_iot_init() == ESP_OK) {
    eb_azure_iot_set_c2d_callback(on_azure_c2d_message);
    eb_azure_iot_start();

    // Block until the first DPS + IoT Hub connection attempt resolves, so
    // the rest of app startup can react to whether Azure is reachable.
    eb_azure_prov_status_t status = eb_azure_iot_wait_provisioning(30000);
    ESP_LOGI(TAG, "Azure provisioning result: %d", status);
  } else {
    ESP_LOGE(TAG, "eb_azure_iot_init() failed");
  }

  // --- Periodic telemetry loop ----------------------------------------------
  uint32_t tick = 0;
  while (1) {
    if (eb_azure_iot_is_connected()) {
      char json_str[128];
      snprintf(json_str, sizeof(json_str),
               "{\"msg_type\":\"demo_telemetry\",\"tick\":%lu}",
               (unsigned long)tick);
      eb_azure_iot_send_telemetry(json_str);
    }

    tick++;
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
