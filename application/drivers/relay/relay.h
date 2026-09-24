#pragma once
#include <stdbool.h>
void relay_init(void);
void relay_on(void);
void relay_off(void);
bool relay_get_state(void);
