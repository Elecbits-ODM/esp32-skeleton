# ESP32 Firmware Skeleton

This repository is a skeleton that guides you in creating an ESP32-based project. It lays out the proposed firmware repository structure to follow when starting a new ESP32 product.

## Architecture

### Application
- `application/tasks/` - FreeRTOS task implementations
- `application/drivers/` - product-specific hardware drivers
- `application/services/` - product-specific services such as diagnostics and OTA

### Components
Reusable/common components:
- BL0937
- Wi-Fi
- MQTT
- NVS
- LittleFS
- GPIO

### Main
`main/main.c` is the ESP-IDF application entry point and performs initialization/startup.

## Data Flow

Energy task -> BL0937 -> Storage task -> LittleFS / MQTT

IoT task -> Wi-Fi + MQTT -> Relay driver

Services -> Diagnostics / OTA
