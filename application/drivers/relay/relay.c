#include "relay.h"
#include "gpio.h"

void relay_init(void)
{
    gpio_init_output(1);
    relay_off();
}

void relay_on(void)
{
    gpio_set_level(1, 1);
}

void relay_off(void)
{
    gpio_set_level(1, 0);
}

bool relay_get_state(void)
{
    return gpio_get_level(1) != 0;
}
