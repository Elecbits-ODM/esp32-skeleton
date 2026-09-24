#include "button.h"
#include "gpio.h"

void button_init(void)
{
    gpio_init_input(0);
}

bool button_is_pressed(void)
{
    return gpio_get_level(0) != 0;
}
