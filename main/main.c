#include "bl0937.h"
#include "relay.h"
#include "led.h"
#include "button.h"
#include "diagnostics_service.h"
#include "ota_service.h"
#include "iot_task.h"
#include "energy_task.h"
#include "storage_task.h"

void app_main(void)
{
    /* Hardware/component initialization */
    bl0937_init();
    relay_init();
    led_init();
    button_init();

    /* Product service initialization */
    diagnostics_service_init();
    ota_service_init();

    /* Start product application tasks */
    iot_task_start();
    energy_task_start();
    storage_task_start();
}
