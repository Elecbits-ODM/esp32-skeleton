# eb_network

## Overview

`eb_network` is a reusable ESP-IDF component providing the network layer
most connected products need: WiFi station connectivity management, and
BLE-based WiFi/cloud provisioning (a custom Bluedroid GATT server compatible
with the Wiser mobile app protocol). It depends on `eb_storage` for all NVS
persistence, and is itself a dependency of `eb_azure`.

## Supported Platform

| | |
|---|---|
| Platform | Espressif ESP32 family — **only chips with Bluedroid BLE support** if you use `eb_ble_prov` (e.g. ESP32, ESP32-C3, ESP32-S3, ESP32-C6); WiFi-only use of `eb_wifi` works on any WiFi-capable target |
| MCU | Build-verified on **ESP32-C3** |
| SDK Version | ESP-IDF **>= 5.0** (build-verified against ESP-IDF v5.5.3) |

## Features

- **WiFi station (STA) management** — init/connect/disconnect, automatic
  credential loading from NVS (via `eb_storage`), retry/backoff
  reconnection, RSSI and scan support, state-change and IP-obtained
  callbacks.
- **Combined WiFi + cloud connectivity tracking** — an event group /
  callback pair so application code (and `eb_azure`) can ask "are we fully
  online" (WiFi *and* cloud) in one call.
- **BLE-based WiFi/cloud provisioning** — advertises a custom GATT service,
  pairs over BLE Secure Connections, authenticates with a shared key, and
  receives WiFi + cloud (Azure DPS) credentials as JSON over a data
  characteristic, with state-change and success callbacks.

## Folder Structure

```
eb_network/
  CMakeLists.txt          Component build script
  idf_component.yml       Component manifest
  README.md
  include/
    eb_wifi.h             WiFi STA manager — public API
    eb_ble_prov.h         BLE GATT provisioning server — public API
    eb_offline.h          NOT built yet — see "Limitations" below
  eb_wifi.c               WiFi STA manager implementation
  eb_ble_prov.c           BLE GATT provisioning server implementation
  eb_offline.c            NOT compiled into this component — see "Limitations"
  examples/
    wifi_connect_demo/    Standalone, buildable ESP-IDF example project
      CMakeLists.txt
      sdkconfig.defaults
      main/
        CMakeLists.txt
        main.c
```

## Dependency

**`eb_storage`** — required. `eb_network` calls into it for all NVS
persistence:

| Dependency | Provides | Declared in (file) | Called from |
|---|---|---|---|
| `eb_storage` | `eb_nvs_save_wifi_credentials()`, `eb_nvs_load_wifi_credentials()`, `eb_nvs_wifi_credentials_exist()`, `eb_nvs_erase_wifi_credentials()`, `eb_wifi_credentials_t` | `eb_storage/include/eb_nvs.h` | `eb_network/eb_wifi.c` |
| `eb_storage` | `eb_nvs_get_prov_status()`, `eb_nvs_set_prov_status()`, `eb_nvs_get_prov_time()`, `eb_nvs_get_pair_status()`, `eb_nvs_set_pair_status()`, `eb_nvs_save_prov_device_id()`, `eb_nvs_load_prov_device_id()`, `eb_nvs_save_ota_url()`, `eb_nvs_save_scope_id()`, `eb_nvs_save_wifi_credentials()` | `eb_storage/include/eb_nvs.h` | `eb_network/eb_ble_prov.c` |

`eb_nvs_init()` itself is not called by `eb_network` — it's the
application's responsibility to call it once at startup, before any
`eb_wifi_*`/`eb_ble_prov_*` function is used (see "Getting Started" below).

`eb_storage` is **not** declared in `idf_component.yml` since it's a
sibling local component, not yet resolvable by the IDF Component Manager —
your project must make it available alongside `eb_network` (see below).

ESP-IDF built-in components required: `esp_wifi`, `esp_netif`, `esp_event`,
`freertos`, `json`, `mbedtls`, `bt`, `wifi_provisioning`, `protocomm`
(declared in `CMakeLists.txt`, fetched automatically as part of ESP-IDF).

## Getting Started

**Add it to a project**, alongside `eb_storage`:
```bash
git submodule add https://github.com/elecbitstech/eb_network.git components/eb_network
git submodule add https://github.com/elecbitstech/eb_storage.git components/eb_storage
```
or via `EXTRA_COMPONENT_DIRS` pointing at both paths.

**Required `sdkconfig.defaults`** — the Bluedroid BLE stack (including the
legacy BLE 4.2 advertising API `eb_ble_prov` uses) must be explicitly
enabled, and the default 1MB app partition is too small once it's linked in:
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
```

**Use it:**
```c
#include "eb_wifi.h"
#include "eb_ble_prov.h"
#include "eb_nvs.h"

static void on_wifi_state_changed(eb_wifi_state_t old_state, eb_wifi_state_t new_state) {
    ESP_LOGI("app", "WiFi state: %d -> %d", old_state, new_state);
}

static void on_ble_prov_state_changed(eb_ble_prov_state_t state) {
    ESP_LOGI("app", "BLE prov state: %d", state);
}

void app_main(void) {
    eb_nvs_init();
    eb_wifi_init();
    eb_wifi_set_state_callback(on_wifi_state_changed);

    if (eb_wifi_has_credentials()) {
        eb_wifi_connect();
    } else {
        eb_ble_prov_set_state_callback(on_ble_prov_state_changed);
        eb_ble_prov_start(); /* reads provisioning status from NVS and advertises */
    }
}
```

**Build:**
```bash
idf.py set-target esp32c3
idf.py build
```
See `examples/wifi_connect_demo` for a complete, buildable project with the
above config already filled in.

## API Reference

### `eb_wifi.h` — WiFi station manager

| Function | Description |
|---|---|
| `esp_err_t eb_wifi_init(void)` | Initializes WiFi in STA mode, loads credentials from NVS if present. Does **not** auto-connect. |
| `esp_err_t eb_wifi_deinit(void)` | Tears down the WiFi subsystem. |
| `esp_err_t eb_wifi_connect(void)` | Connects using stored NVS credentials. `ESP_ERR_NOT_FOUND` if none stored. |
| `esp_err_t eb_wifi_connect_with_credentials(const char *ssid, const char *password, bool save_to_nvs)` | Connects with explicit credentials, optionally persisting them on success. |
| `esp_err_t eb_wifi_disconnect(void)` | Disconnects from the current AP. |
| `bool eb_wifi_is_connected(void)` | `true` only once an IP has been obtained. |
| `esp_err_t eb_wifi_get_status(eb_wifi_status_t *status)` | Fills state, SSID, RSSI, IP, retry count, credentials-valid flag. |
| `eb_wifi_state_t eb_wifi_get_state(void)` | One of `EB_WIFI_STATE_DISCONNECTED` / `_CONNECTING` / `_CONNECTED` / `_GOT_IP` / `_ERROR`. |
| `int8_t eb_wifi_get_rssi(void)` | Current RSSI in dBm, `0` if not connected. |
| `void eb_wifi_set_state_callback(eb_wifi_state_cb_t callback)` | `void (*)(eb_wifi_state_t old, eb_wifi_state_t new)` — fires on every state transition. |
| `void eb_wifi_set_ip_callback(eb_wifi_ip_cb_t callback)` | `void (*)(uint32_t ip_addr)` — fires once an IP is obtained (network byte order). |
| `bool eb_wifi_has_credentials(void)` | `true` if NVS has stored WiFi credentials. |
| `esp_err_t eb_wifi_erase_credentials(void)` | Erases stored credentials. |
| `esp_err_t eb_wifi_scan_start(uint16_t max_ap)` | Starts an active scan for up to `max_ap` (1–20) access points. |
| `esp_err_t eb_wifi_scan_get_results(wifi_ap_record_t *ap_records, uint16_t *num_records)` | Retrieves scan results; `num_records` is in/out. |
| `void eb_wifi_set_connectivity_callback(eb_connectivity_cb_t cb)` | `void (*)(bool wifi_up, bool azure_up)` — fires whenever combined WiFi/cloud connectivity changes. |
| `void eb_connectivity_notify_azure(bool up)` | Called by the cloud layer (`eb_azure`) to report Hub MQTT connect/disconnect. |
| `bool eb_connectivity_is_online(void)` | `true` only when both WiFi *and* cloud are up. |
| `bool eb_connectivity_is_wifi_up(void)` | `true` when WiFi has an IP, regardless of cloud state. |
| `extern SemaphoreHandle_t g_tls_handshake_mutex` | Take before any TLS handshake (cloud or otherwise), give immediately after — serializes concurrent handshakes; do **not** hold it for a connection's lifetime. |

### `eb_ble_prov.h` — BLE provisioning

| Function | Description |
|---|---|
| `esp_err_t eb_ble_prov_init(void)` | Initializes the BT controller + Bluedroid stack, registers GATTS/GAP callbacks, starts the connection-monitor task. |
| `esp_err_t eb_ble_prov_deinit(void)` | Stops advertising, disables Bluedroid and the BT controller — frees the RAM Bluedroid holds. |
| `esp_err_t eb_ble_prov_start(void)` | Starts provisioning using the status already stored in NVS. |
| `esp_err_t eb_ble_prov_start_with_status(eb_prov_status_t status)` | Starts provisioning with an explicit status, controlling the advertised device name format. |
| `esp_err_t eb_ble_prov_stop(void)` | Stops advertising/provisioning without a full deinit. |
| `bool eb_ble_prov_is_active(void)` | `true` if currently advertising or connected to a provisioning client. |
| `eb_ble_prov_state_t eb_ble_prov_get_state(void)` | One of `IDLE`/`STARTING`/`ADVERTISING`/`CONNECTED`/`PAIRED`/`AUTHENTICATED`/`PROVISIONING`/`SUCCESS`/`FAILED`/`STOPPED`. |
| `esp_err_t eb_ble_prov_get_device_name(char *name, size_t len)` | Retrieves the BLE device name currently advertised (buffer ≥ 48 bytes). |
| `esp_err_t eb_ble_prov_notify_client(const char *data)` | Sends a string notification to the connected BLE client over the data characteristic. |
| `void eb_ble_prov_set_state_callback(eb_ble_prov_state_cb_t callback)` | `void (*)(eb_ble_prov_state_t state)`. |
| `void eb_ble_prov_set_success_callback(eb_ble_prov_success_cb_t callback)` | `void (*)(const eb_ble_prov_credentials_t *creds)` — fires once valid credentials are received. |

`eb_prov_status_t`: `EB_PROV_STATUS_NORMAL` (existing credentials),
`EB_PROV_STATUS_CHANGE_AP` (re-provisioning requested),
`EB_PROV_STATUS_TIMED` (timed provisioning window).

`eb_ble_prov_credentials_t`: `ssid[33]`, `password[65]`, `device_id[40]`,
`scope_id[32]`, `ota_url[256]` — what the companion app sends over BLE.

Protocol constants: `EB_BLE_PROV_DEVICE_NAME_PREFIX` (`"SE-PRO-EVSO"`, kept
for compatibility with the existing companion app), `EB_BLE_PROV_AUTH_KEY`
(`"BLAZE"`), `EB_BLE_PROV_DEFAULT_TIMEOUT_SEC` (180).

## Limitations

- **`eb_offline.c` / `eb_offline.h` are present but NOT compiled into this
  component.** They implement an "offline mode" (device-to-device control
  bypassing the cloud) that was incomplete in the source project this
  component was extracted from. `eb_offline.c` `#include`s
  `eb_device_registry.h`, which does not exist yet anywhere in this repo —
  it needs to be designed and implemented (a device registry/cache mapping
  device IDs to MAC/IP/state, presumably populated via mDNS discovery).
  Until that header exists, `eb_offline.c` is deliberately excluded from
  `CMakeLists.txt`'s `SRCS`. **Currently not needed by any consuming
  project — intentionally deferred, not a blocker.** To pick it up:
  implement `include/eb_device_registry.h` + its `.c`, then add
  `"eb_offline.c"` to `SRCS`.
- No mDNS discovery or WSS client is otherwise provided by this component.
- The BLE provisioning protocol (device name prefix, auth key, JSON
  credential format) is fixed for compatibility with an existing companion
  mobile app — it isn't a generic/configurable BLE provisioning scheme.
- Requires ~large Bluedroid RAM/flash footprint; not suitable for
  extremely flash/RAM-constrained targets when both WiFi and BLE are active
  simultaneously (see the partition-size note in "Getting Started").

## Maintainer

Elecbits. For issues or contributions, open an issue/PR on
[github.com/elecbitstech/eb_network](https://github.com/elecbitstech/eb_network).

Currently maintained by Aneesh Madhavan, aneesh.m@elecbits.in.