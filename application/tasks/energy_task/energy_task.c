#include "energy_task.h"
#include "bl0937.h"

static void energy_task(void *arg)
{
    (void)arg;

    while (1) {
        float power = bl0937_get_power();
        (void)power;

        /* Dummy: send energy record to storage task */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void energy_task_start(void)
{
    xTaskCreate(energy_task, "energy_task", 4096, NULL, 5, NULL);
}
