################################################################################
# CMakeLists.txt for eb_network component
#
# Network Layer for:
# - WiFi connectivity (STA mode)
# - BLE provisioning
# - mDNS discovery (TODO: future work)
# - WSS client / offline control (TODO: future work — see eb_offline.c)
################################################################################

idf_component_register(
    SRCS
        "eb_wifi.c"
        "eb_ble_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        freertos
        json
        mbedtls
        bt
    PRIV_REQUIRES
        eb_storage
        wifi_provisioning
        protocomm
)

# NOTE: eb_offline.c is intentionally NOT built (not listed in SRCS above).
# It depends on eb_device_registry.h, which does not exist yet. See
# README.md "Known limitations / Work in progress" before wiring it in.
