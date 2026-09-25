# eb_storage

## Overview

`eb_storage` is a reusable ESP-IDF component that wraps the raw `nvs_flash` /
`nvs` APIs behind a small, product-agnostic interface for the kinds of data
almost every embedded product needs to persist across reboots: WiFi
credentials, cloud/IoT provisioning state, device identity, X.509
certificates, and generic key-value data. It exists so that NVS storage
logic is written once and reused across projects instead of being
re-implemented per product.

It has no dependency on any other `eb_*` component — `eb_network` and
`eb_azure` both depend on it, not the other way around.

## Supported Platform

| | |
|---|---|
| Platform | Espressif ESP32 family (any target with an NVS-capable flash partition) |
| MCU | ESP32, ESP32-S2/S3, ESP32-C2/C3/C6, ESP32-H2 — no chip-specific code; build-verified on **ESP32-C3** |
| SDK Version | ESP-IDF **>= 5.0** (build-verified against ESP-IDF v5.5.3) |

## Features

- WiFi station credentials (SSID / password) storage and existence checks
- Cloud/IoT provisioning config (DPS scope ID, registration ID, IoT Hub
  hostname, assigned device ID, provisioned flag) — generic enough for
  Azure IoT DPS/Hub-style provisioning flows
- MAC-derived device identity
- X.509 device certificate / private key / root CA storage (for mTLS)
- BLE (or other) commissioning/provisioning state: status, timeout, pair
  failure code, OTA URL, provisioned device ID, scope ID
- A persistent restart counter and a one-shot "device added" event flag
- Generic string and blob get/set helpers under a caller-supplied NVS
  namespace, for any other key-value data a project needs

## Folder Structure

```
eb_storage/
  CMakeLists.txt          Component build script (idf_component_register)
  idf_component.yml       Component manifest
  README.md
  LICENSE
  include/
    eb_nvs.h              Public API — all types, macros, and function declarations
  eb_nvs.c                Implementation
  examples/
    basic_storage_demo/   Standalone, buildable ESP-IDF example project
      CMakeLists.txt
      sdkconfig.defaults
      main/
        CMakeLists.txt
        main.c
```

## Dependency

None. `eb_storage` only requires ESP-IDF's own `nvs_flash` component
(declared in `CMakeLists.txt`'s `REQUIRES`). It is the base layer other
`eb_*` components build on:

```
eb_azure  ──depends on──▶  eb_network  ──depends on──▶  eb_storage
eb_azure  ────────────────depends on───────────────────▶  eb_storage
```

## Getting Started

**Add it to a project** (pick one):

```bash
# Option A — git submodule (auto-discovered under components/)
git submodule add https://github.com/elecbitstech/eb_storage.git components/eb_storage
```
```cmake
# Option B — EXTRA_COMPONENT_DIRS, in your project's top-level CMakeLists.txt,
# before include($ENV{IDF_PATH}/tools/cmake/project.cmake):
set(EXTRA_COMPONENT_DIRS "path/to/eb_storage")
```

**Use it:**
```c
#include "eb_nvs.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(eb_nvs_init());

    eb_wifi_credentials_t creds = {
        .ssid = "MyNetwork",
        .password = "MyPassword",
        .configured = true,
    };
    eb_nvs_save_wifi_credentials(&creds);

    eb_wifi_credentials_t loaded;
    if (eb_nvs_load_wifi_credentials(&loaded) == ESP_OK) {
        ESP_LOGI("app", "Loaded SSID: %s", loaded.ssid);
    }
}
```

**Build:**
```bash
idf.py set-target esp32c3
idf.py build
```
No special Kconfig or partition-size options are needed for `eb_storage` on
its own — see `examples/basic_storage_demo` for a complete, minimal project.

## API Reference

All declarations live in `include/eb_nvs.h`. Every function returns
`esp_err_t` unless noted; `ESP_OK` on success.

### Types

| Type | Fields | Notes |
|---|---|---|
| `eb_wifi_credentials_t` | `ssid[33]`, `password[65]`, `configured` | `EB_WIFI_SSID_MAX_LEN`=32, `EB_WIFI_PASSWORD_MAX_LEN`=64 |
| `eb_azure_config_t` | `scope_id[17]`, `registration_id[65]`, `hub_hostname[129]`, `device_id[65]`, `provisioned` | Cloud/DPS provisioning cache |
| `eb_device_identity_t` | `device_id[13]` (12 hex chars + NUL), `mac[6]` | MAC-derived device identity |

Certificate buffers should be sized at least `EB_CERT_MAX_SIZE` /
`EB_KEY_MAX_SIZE` (2048 bytes each).

### Initialization

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_init(void)` | Initializes the NVS flash partition. Call once at startup before any other `eb_nvs_*` call. |
| `esp_err_t eb_nvs_erase_all(void)` | Erases **all** NVS storage — factory reset. Irreversible. |

### WiFi credentials

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_save_wifi_credentials(const eb_wifi_credentials_t *creds)` | Persists SSID/password/configured flag. |
| `esp_err_t eb_nvs_load_wifi_credentials(eb_wifi_credentials_t *creds)` | Loads stored credentials. Returns `ESP_ERR_NOT_FOUND` if never configured. |
| `bool eb_nvs_wifi_credentials_exist(void)` | `true` if credentials are stored. |
| `esp_err_t eb_nvs_erase_wifi_credentials(void)` | Removes stored WiFi credentials only. |

### Cloud / Azure IoT configuration

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_save_azure_config(const eb_azure_config_t *config)` | Persists the full DPS/Hub config struct. |
| `esp_err_t eb_nvs_load_azure_config(eb_azure_config_t *config)` | Loads it back. `ESP_ERR_NOT_FOUND` if unset. |
| `esp_err_t eb_nvs_update_azure_provisioning(const char *hostname, const char *device_id)` | Updates just the hostname + device ID fields after a successful DPS provisioning round, without needing the full struct. |
| `bool eb_nvs_azure_config_exists(void)` | `true` if a config is stored. |
| `esp_err_t eb_nvs_erase_azure_config(void)` | Removes the stored cloud config only. |

### Device identity

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_get_device_identity(eb_device_identity_t *identity)` | Fills in the device's MAC-derived hex ID and raw MAC bytes. |

### Provisioning / pairing state (used by BLE commissioning flows)

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_get_prov_status(uint8_t *status)` | 0 = normal, 1 = change-AP, 2 = timed. `ESP_ERR_NOT_FOUND` if never set. |
| `esp_err_t eb_nvs_set_prov_status(uint8_t status, uint32_t timeout_sec)` | Sets status + a timeout, clamped to 60–600s. |
| `esp_err_t eb_nvs_get_prov_time(uint32_t *timeout_sec)` | Reads the timeout (default 180s if unset). |
| `esp_err_t eb_nvs_get_pair_status(uint8_t *status)` / `set_pair_status(uint8_t status)` | BLE pair failure status get/set. |
| `esp_err_t eb_nvs_save_ota_url(const char *url)` / `load_ota_url(char *url, size_t max_len)` | OTA firmware URL, typically pushed by a companion app during provisioning. |
| `esp_err_t eb_nvs_save_prov_device_id(const char *device_id)` / `load_prov_device_id(...)` | The device ID string handed down by the provisioning app (distinct from the MAC-derived identity). |
| `esp_err_t eb_nvs_save_scope_id(const char *scope_id)` / `load_scope_id(...)` | Azure DPS scope ID, if delivered at provisioning time rather than baked into Kconfig. |
| `bool eb_nvs_is_device_added_event_sent(void)` / `esp_err_t eb_nvs_mark_device_added_event_sent(void)` | One-shot flag so a "device added" cloud event is only ever sent once. |
| `uint32_t eb_nvs_get_restart_count(void)` / `esp_err_t eb_nvs_increment_restart_count(void)` | Persistent boot counter — call `increment` once per boot. |

### Certificate storage

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_save_device_cert(const char *cert_pem)` / `load_device_cert(char *cert_pem, size_t max_len)` | Device X.509 certificate, PEM-encoded. |
| `esp_err_t eb_nvs_save_device_key(const char *key_pem)` / `load_device_key(char *key_pem, size_t max_len)` | Device private key, PEM-encoded. Treat with the same care as any private key material. |
| `esp_err_t eb_nvs_save_root_ca(const char *ca_pem)` / `load_root_ca(char *ca_pem, size_t max_len)` | Root CA certificate, PEM-encoded. |
| `bool eb_nvs_certificates_exist(void)` | `true` only if cert, key, **and** root CA are all present. |
| `esp_err_t eb_nvs_erase_certificates(void)` | Removes all three. |

### Generic key-value API

| Function | Description |
|---|---|
| `esp_err_t eb_nvs_set_string(const char *namespace, const char *key, const char *value)` / `get_string(...)` | For any project-specific string data, under a caller-chosen namespace. |
| `esp_err_t eb_nvs_set_blob(const char *namespace, const char *key, const void *data, size_t len)` / `get_blob(..., size_t *len)` | Same, for arbitrary binary blobs. `get_blob`'s `len` is in/out: pass buffer size in, receives actual size back. |

## Limitations

- No encryption-at-rest beyond whatever ESP-IDF's NVS encryption feature you
  enable yourself (`CONFIG_NVS_ENCRYPTION`) — this component does not add
  its own.
- Certificate/string/blob load functions take a caller-supplied buffer and
  `max_len`; callers are responsible for sizing buffers correctly (see
  `EB_CERT_MAX_SIZE`/`EB_KEY_MAX_SIZE` for certs).
- Not thread-safety-audited beyond what the underlying `nvs` component
  already guarantees — avoid concurrent writes to the same key from
  multiple tasks without your own serialization.

## Maintainer

Elecbits (originally authored by Syed S Mashaam,
syed.shigarf@elecbits.in). For issues or contributions, open an issue/PR on
[github.com/elecbitstech/eb_storage](https://github.com/elecbitstech/eb_storage).

Currently maintained by Aneesh Madhavan, aneesh.m@elecbits.in.
