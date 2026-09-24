# ESP32 Smart Adapter - Dummy Firmware

This project demonstrates the proposed firmware repository structure.

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
