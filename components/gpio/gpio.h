#pragma once
void gpio_init_output(int pin);
void gpio_init_input(int pin);
void gpio_set_level(int pin, int level);
int gpio_get_level(int pin);
