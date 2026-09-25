# eb_azure

## Overview

`eb_azure` is a reusable ESP-IDF component providing Azure IoT Hub
connectivity: X.509 DPS provisioning, a persistent MQTT session to IoT Hub,
device-to-cloud (D2C) telemetry, and cloud-to-device (C2D) message /
direct-method handling. It's built on top of Microsoft's
[azure-iot-middleware-freertos](https://github.com/Azure/azure-iot-middleware-freertos)
and depends on `eb_storage` and `eb_network`.

## Supported Platform

| | |
|---|---|
| Platform | Espressif ESP32 family, any target with WiFi + enough flash for the Azure SDK + TLS (see "Limitations") |
| MCU | Build-verified on **ESP32-C3** |
| SDK Version | ESP-IDF **>= 5.0** (build-verified against ESP-IDF v5.5.3) |

## Features

1. **DPS provisioning** (`eb_azure_dps`) — connects to Azure Device
   Provisioning Service over TLS using an X.509 client certificate,
   registers the device (registration ID = certificate CN), and receives
   the assigned IoT Hub hostname + device ID. Cached in NVS (via
   `eb_storage`) so subsequent boots skip DPS entirely.
2. **IoT Hub client** (`eb_azure_iot`) — a dedicated FreeRTOS task that:
   - Waits for WiFi (via `eb_network`), then provisions via DPS (or loads
     the NVS cache).
   - Opens a persistent TLS/MQTT session to IoT Hub (X.509 auth).
   - Subscribes to Cloud-to-Device (C2D) messages and Direct Methods.
   - Sends a periodic status heartbeat and any queued telemetry.
   - Reconnects automatically with backoff, including throttle detection
     for Hubs that drop repeated short-lived connections.

## Folder Structure

```
eb_azure/
  CMakeLists.txt          Component build script (EMBED_TXTFILES for certs)
  Kconfig                 "EB Azure IoT Configuration" menu
  idf_component.yml       Component manifest
  README.md
  LICENSE
  certs/
    device_cert.pem       PLACEHOLDER — replace before building for real
    device_key.pem        PLACEHOLDER — replace before building for real
  include/
    eb_azure_dps.h         DPS client — public API
    eb_azure_iot.h         IoT Hub client — public API
  eb_azure_dps.c           DPS client implementation
  eb_azure_iot.c           IoT Hub client implementation
  examples/
    azure_telemetry_demo/  Standalone, buildable ESP-IDF example project
      CMakeLists.txt
      sdkconfig.defaults
      main/
        CMakeLists.txt
        main.c
```

## Dependency

| Dependency | Provides | Declared in (file) | Layout |
|---|---|---|---|
| `eb_storage` | `eb_nvs_init()`, `eb_nvs_load_wifi_credentials()`, `eb_nvs_save_azure_config()`, `eb_nvs_load_azure_config()`, `eb_nvs_update_azure_provisioning()`, `eb_nvs_get_device_identity()`, `eb_nvs_load_scope_id()`, `eb_nvs_get_restart_count()`, and the other `eb_nvs_*` calls used for caching DPS results / device identity | `eb_storage/include/eb_nvs.h` | sibling component directory |
| `eb_network` | `eb_wifi_init()`, `eb_wifi_connect()`, `eb_wifi_is_connected()`, `eb_wifi_set_state_callback()` | `eb_network/include/eb_wifi.h` | sibling component directory |
| `eb_network` | `g_tls_handshake_mutex` (extern, defined in `eb_wifi.c`) | `eb_network/include/eb_wifi.h` | sibling component directory |
| `azure-iot-middleware-freertos` | `AzureIoTProvisioningClient_Init/Deinit/Register/GetDeviceAndHub`, `AzureIoTProvisioningClient_t` | `azure-iot-middleware-freertos/source/include/azure_iot_provisioning_client.h` | vendored component |
| `azure-iot-middleware-freertos` | `AzureIoTHubClient_Init/Deinit/Connect/Disconnect/OptionsInit/ProcessLoop`, `AzureIoTHubClient_SendTelemetry`, `AzureIoTHubClient_SendCommandResponse`, `AzureIoTHubClient_SubscribeCloudToDeviceMessage`, `AzureIoTHubClient_SubscribeCommand`, `AzureIoTHubClient_t` | `azure-iot-middleware-freertos/source/include/azure_iot_hub_client.h` | vendored component |
| `azure-iot-middleware-freertos` | `AzureIoTMessage_PropertiesInit`, `AzureIoTMessage_PropertiesAppend` | `azure-iot-middleware-freertos/source/include/azure_iot_message.h` | vendored component |
| `azure-iot-middleware-freertos` | Transport interface struct used to bridge the SDK to ESP-TLS (`esp_tls`) | `azure-iot-middleware-freertos/source/interface/azure_iot_transport_interface.h` | vendored component |

Upstream: https://github.com/Azure/azure-iot-middleware-freertos

None of the three are fetched automatically — `eb_storage`/`eb_network`
aren't yet in the IDF Component Manager registry, and the Azure SDK isn't
part of this library. Typical sibling layout:

```
your_workspace/
  eb_azure/                         <- this repo
  eb_storage/
  eb_network/
  azure-iot-middleware-freertos/
  your_project/
    CMakeLists.txt                  <- EXTRA_COMPONENT_DIRS to the three above
    main/
```

## Getting Started

**Add it to a project**, alongside `eb_storage`, `eb_network`, and a clone
of the Azure SDK:
```bash
git submodule add https://github.com/elecbitstech/eb_azure.git components/eb_azure
git submodule add https://github.com/elecbitstech/eb_network.git components/eb_network
git submodule add https://github.com/elecbitstech/eb_storage.git components/eb_storage
git clone --recursive https://github.com/Azure/azure-iot-middleware-freertos.git components/azure-iot-middleware-freertos
```

**Certificate provisioning** — this component uses X.509 auth for both DPS
and IoT Hub, no symmetric keys. `certs/device_cert.pem` and
`certs/device_key.pem` in this repo are placeholders that will fail the TLS
handshake as-is. Before building for real:
1. Generate/obtain a device certificate + private key whose CN matches the
   device's DPS registration ID.
2. Replace the two placeholder files (or repoint `EMBED_TXTFILES` in
   `CMakeLists.txt`).
3. **Never commit a real private key.** Prefer injecting certs at
   build/flash time from a secrets store or factory provisioning pipeline
   over committing real device-specific `.pem` files at all.

**Required `sdkconfig.defaults`** — because `eb_azure` pulls in `eb_network`
(which needs Bluedroid BLE) and the Azure SDK/TLS stack, both the BLE config
and a larger partition table are needed:
```
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_GATTS_ENABLE=y
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
CONFIG_BT_BLE_42_ADV_EN=y
CONFIG_BT_BLE_42_SCAN_EN=y
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_ESP_TASK_WDT_INIT=y

CONFIG_EB_AZURE_DPS_ID_SCOPE="0ne00XXXXXXX"
CONFIG_EB_AZURE_DPS_REG_ID_PREFIX=""
CONFIG_EB_AZURE_TELEMETRY_INTERVAL_SEC=60
```

**Use it:**
```c
#include "eb_azure_iot.h"

static void on_c2d(const char *msg_type, const cJSON *payload) {
    // handle application-specific C2D messages
}

void app_main(void) {
    eb_nvs_init();
    eb_wifi_init();
    eb_wifi_connect();

    if (eb_azure_iot_init() == ESP_OK) {
        eb_azure_iot_set_c2d_callback(on_c2d);
        eb_azure_iot_start();
        eb_azure_iot_wait_provisioning(30000);
    }

    while (1) {
        if (eb_azure_iot_is_connected()) {
            eb_azure_iot_send_telemetry("{\"msg_type\":\"heartbeat\"}");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

**Build:**
```bash
idf.py set-target esp32c3
idf.py build
```
See `examples/azure_telemetry_demo/` for a complete, buildable project.

## API Reference

### `eb_azure_dps.h` — DPS client

| Function | Description |
|---|---|
| `esp_err_t eb_azure_dps_init(void)` | Builds the registration ID from the optional Kconfig prefix + device MAC. Must match the certificate CN for X.509 auth to succeed. |
| `esp_err_t eb_azure_dps_provision(char *hub_hostname, size_t hostname_len, char *device_id, size_t device_id_len)` | Connects to DPS via TLS/X.509, registers the device, returns the assigned Hub hostname + device ID, and caches the result in NVS. Returns `ESP_ERR_TIMEOUT` on registration timeout, `ESP_FAIL` on other provisioning failures. |
| `const char *eb_azure_dps_get_registration_id(void)` | Returns the registration ID string in use (e.g. `"98A316D7FB04"` or `"prefix-98A316D7FB04"`). |

### `eb_azure_iot.h` — IoT Hub client

**Lifecycle**

| Function | Description |
|---|---|
| `esp_err_t eb_azure_iot_init(void)` | Initializes DPS but does not connect — call `eb_azure_iot_start()` to begin. |
| `esp_err_t eb_azure_iot_start(void)` | Spawns the FreeRTOS task that handles DPS/cache, TLS+MQTT to Hub, C2D/method/twin subscriptions, heartbeat telemetry, and auto-reconnect. |
| `esp_err_t eb_azure_iot_stop(void)` | Stops the task and closes the connection. |
| `bool eb_azure_iot_is_connected(void)` | `true` if the MQTT session to IoT Hub is currently up. |

**Provisioning status**

| Function | Description |
|---|---|
| `eb_azure_prov_status_t eb_azure_iot_wait_provisioning(uint32_t timeout_ms)` | Blocks until the first DPS + Hub connection attempt resolves (success or failure); `0` = wait forever. Use this to sequence startup so other modules wait on a known Azure state. |
| `eb_azure_prov_status_t eb_azure_iot_get_prov_status(void)` | Non-blocking read of the current status. |

`eb_azure_prov_status_t`: `EB_AZURE_PROV_PENDING`, `EB_AZURE_PROV_SUCCESS`,
`EB_AZURE_PROV_DPS_FAILED`, `EB_AZURE_PROV_HUB_FAILED`,
`EB_AZURE_PROV_TIMEOUT`.

**Device-to-cloud (D2C) — callable from any task**

| Function | Description |
|---|---|
| `esp_err_t eb_azure_iot_send_telemetry(const char *json_payload)` | Sends a raw JSON telemetry message. `ESP_ERR_INVALID_STATE` if not connected. |
| `esp_err_t eb_azure_iot_send_status(void)` | Sends a built-in heartbeat: device ID, online device count, WiFi RSSI, free heap, uptime, pipeline state. |
| `esp_err_t eb_azure_iot_send_voice_command_report(const cJSON *report)` | Sends an application-defined command report object. |
| `esp_err_t eb_azure_iot_send_command_ack(const char *request_id, bool success, const char *error, uint32_t latency_ms)` | Acknowledges a `remote_command` C2D message back to the cloud. |
| `esp_err_t eb_azure_iot_send_device_added_event(void)` | Sends a one-time device-added event with device ID, model/version, SSID, RSSI, restart count, chip info, SDK versions, free heap, firmware type. |
| `esp_err_t eb_azure_iot_send_device_details(void)` | Same payload as the device-added event, different event type — triggered by the cloud's `get_device_details` C2D command. |

**Callback registration**

| Function | Description |
|---|---|
| `void eb_azure_iot_notify_wifi_state(bool connected)` | **Must** be called from your WiFi state callback whenever WiFi gains/loses its IP — when `false`, the task pauses TLS attempts to avoid heap fragmentation from repeated failed handshakes. |
| `void eb_azure_iot_set_c2d_callback(eb_azure_c2d_cb_t callback)` | `void (*)(const char *msg_type, const cJSON *payload)`. |
| `void eb_azure_iot_set_command_callback(eb_azure_command_cb_t callback)` | `void (*)(const char *command_name, const cJSON *payload, char *response, size_t response_len)` — write your JSON response into `response`. |

> Note: a handful of core onboarding C2D commands (`AddAck`, `restart`,
> `delete`, `get_device_details`) are handled internally and never reach
> `eb_azure_c2d_cb_t`. Everything else is forwarded to your callback.

### Kconfig options (menu: "EB Azure IoT Configuration")

| Symbol | Default | Description |
|---|---|---|
| `EB_AZURE_DPS_ENDPOINT` | `global.azure-devices-provisioning.net` | DPS global endpoint (same for all Azure accounts) |
| `EB_AZURE_DPS_ID_SCOPE` | `0ne00XXXXXXX` | Your Azure DPS ID Scope (Azure Portal > DPS > Overview) |
| `EB_AZURE_DPS_REG_ID_PREFIX` | `""` | Optional prefix before the MAC-address-based registration ID |
| `EB_AZURE_IOT_HUB_FQDN` | `""` | Optional — only used to skip DPS entirely; normally left empty |
| `EB_AZURE_TELEMETRY_INTERVAL_SEC` | `60` | Heartbeat telemetry interval, 10–3600s |

## Limitations

- **Certificates are placeholders** — see "Certificate provisioning" above.
  The component will build but fail the TLS handshake until real,
  CN-matching cert/key material is supplied.
- **No OTA support yet** — firmware update over Azure IoT is a documented
  TODO in `eb_azure_iot.h`, not implemented.
- **No certificate rollover** — expired/rotated certificates must currently
  be handled by re-flashing; there's no in-field cert-update path.
- Depends on `eb_network`, which in turn requires the Bluedroid BLE stack
  to be enabled even if your product never uses BLE provisioning directly —
  see `eb_network`'s own README for why.
- Requires a larger-than-default flash partition (BLE + Azure SDK + TLS
  together routinely exceed the default 1MB app slot) — see "Getting
  Started".

## Maintainer

Elecbits (originally authored by Syed S Mashaam,
syed.shigarf@elecbits.in). For issues or contributions, open an issue/PR on
[github.com/elecbitstech/eb_azure](https://github.com/elecbitstech/eb_azure).

Currently maintained by Aneesh Madhavan, aneesh.m@elecbits.in.
