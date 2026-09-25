################################################################################
# CMakeLists.txt for eb_azure component
#
# Azure IoT Integration:
# - DPS X.509 provisioning (eb_azure_dps)
# - IoT Hub persistent MQTT connection (eb_azure_iot)
# - C2D message handling, D2C telemetry
################################################################################

idf_component_register(
    SRCS
        "eb_azure_dps.c"
        "eb_azure_iot.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        json
        azure-iot-middleware-freertos
    PRIV_REQUIRES
        eb_storage
        eb_network
        esp_event
        esp_wifi
        esp-tls
        esp_hw_support
        spi_flash
    EMBED_TXTFILES
        "certs/device_cert.pem"
        "certs/device_key.pem"
)
