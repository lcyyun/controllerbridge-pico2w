#pragma once
#include "pico/time.h"
#define CYW43_WL_GPIO_LED_PIN 0
inline void cyw43_arch_gpio_put(unsigned, bool) {}
