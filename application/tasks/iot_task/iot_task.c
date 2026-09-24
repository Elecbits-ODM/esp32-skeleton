#include "iot_task.h"
#include "wifi.h"
#include "mqtt.h"
#include "relay.h"

static void iot_task(void *arg)
{
    (void)arg;
    wifi_init();
    mqtt_init();

    while (1) {
        /* Dummy: receive cloud command and apply product logic */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void iot_task_start(void)
{
    xTaskCreate(iot_task, "iot_task", 4096, NULL, 5, NULL);
}
