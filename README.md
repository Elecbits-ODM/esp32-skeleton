# ESP32 Firmware Skeleton

This repository is a skeleton that guides you in creating an ESP32-based project. It lays out the proposed firmware repository structure to follow when starting a new ESP32 product.

**Modular · Scalable · Maintainable**

## Project Structure

```
project_repository_name/
├── application/                  # Product-specific application layer
│   ├── tasks/                    # FreeRTOS tasks
│   │   ├── iot_task/
│   │   │   ├── iot_task.c
│   │   │   └── iot_task.h
│   │   ├── energy_task/
│   │   │   ├── energy_task.c
│   │   │   └── energy_task.h
│   │   └── storage_task/
│   │       ├── storage_task.c
│   │       └── storage_task.h
│   ├── drivers/                  # Product hardware drivers
│   │   ├── relay/
│   │   │   ├── relay.c
│   │   │   └── relay.h
│   │   ├── led/
│   │   │   ├── led.c
│   │   │   └── led.h
│   │   └── button/
│   │       ├── button.c
│   │       └── button.h
│   └── services/                 # Product-specific services
│       ├── diagnostics_service/
│       │   ├── diagnostics_service.c
│       │   └── diagnostics_service.h
│       └── ota_service/
│           ├── ota_service.c
│           └── ota_service.h
├── components/                   # Reusable components
│   ├── bl0937/
│   ├── wifi/
│   ├── mqtt/
│   ├── nvs/
│   ├── littlefs/
│   ├── gpio/
│   └── ...
├── main/                         # ESP-IDF entry point
│   ├── main.c
│   ├── app_config.h
│   └── CMakeLists.txt
├── tools/                        # Development and automation scripts
│   └── script.py
├── tests/                        # Unit and integration tests
│   ├── unit/
│   └── integration/
├── CMakeLists.txt                # Build configuration
├── README.md                     # Project documentation
├── CHANGELOG.md                  # Version history
└── LICENSE                       # License information
```

## Layers

### Application Layer
Product-specific business logic.

- **tasks/** → FreeRTOS task implementations
- **drivers/** → Hardware drivers (relay, LED, button)
- **services/** → Business services (diagnostics, OTA)

### Components Layer
Reusable components (drivers, libraries, hardware abstraction, etc.).

Examples:
- BL0937 driver
- Wi-Fi
- MQTT
- NVS
- LittleFS
- GPIO

### Main Layer
ESP-IDF entry point and project configuration.

### Tools Layer
Development and automation scripts.

### Tests Layer
Unit and integration tests.

### Project Files
Build configuration and documentation (`CMakeLists.txt`, `README.md`, `CHANGELOG.md`, `LICENSE`).

## High-Level Architecture

```
                              main.c
                                │
                                ▼
┌───────────────────────────────────────────────────────────────┐
│                        Application Layer                       │
│                                                                 │
│   Tasks                     Drivers                Services    │
│   ┌─────────────────────┐   ┌───────────────────┐  ┌─────────┐ │
│   │ iot_task energy_task │   │ relay  led  button │  │diagnos- │ │
│   │      storage_task    │   │                    │  │ostics_  │ │
│   │                      │   │                    │  │service  │ │
│   │                      │   │                    │  │ota_     │ │
│   │                      │   │                    │  │service  │ │
│   └─────────────────────┘   └───────────────────┘  └─────────┘ │
└─────────────────┬─────────────────────┬────────────────────────┘
                   ▼                     ▼
┌───────────────────────────────────────────────────────────────┐
│                        Components Layer                        │
│   bl0937   wifi   mqtt   nvs   littlefs   gpio                 │
└───────────────────────────────┬─────────────────────────────────┘
                                 ▼
                   ESP32 Hardware (HAL / Drivers)
```

## Data Flow

Energy task -> BL0937 -> Storage task -> LittleFS / MQTT

IoT task -> Wi-Fi + MQTT -> Relay driver

Services -> Diagnostics / OTA
