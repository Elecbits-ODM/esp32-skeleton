################################################################################
# CMakeLists.txt for eb_storage component
#
# NVS Storage Layer for:
# - WiFi credentials
# - Azure IoT configuration
# - X.509 certificates
# - Device identity
################################################################################

idf_component_register(
    SRCS
        "eb_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
    PRIV_REQUIRES
        esp_system
)
