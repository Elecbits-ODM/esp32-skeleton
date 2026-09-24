#include "storage_task.h"
#include "littlefs.h"

static void storage_task(void *arg)
{
    (void)arg;

    while (1) {
        /* Dummy: store offline energy data / upload pending records */
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void storage_task_start(void)
{
    xTaskCreate(storage_task, "storage_task", 4096, NULL, 4, NULL);
}
