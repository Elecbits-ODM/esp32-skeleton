#include "led.h"
#include "gpio.h"

void led_init(void)
{
    gpio_init_output(2);
}

void led_set(bool on)
{
    gpio_set_level(2, on ? 1 : 0);
}

void led_blink(void)
{
    led_set(true);
    /* Dummy delay would normally be handled by a task/timer */
    led_set(false);
}
