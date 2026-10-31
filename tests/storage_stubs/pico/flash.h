#pragma once
#include <cstdint>
#define PICO_OK 0
#define PICO_ERROR_TIMEOUT (-1)
int flash_safe_execute(void (*callback)(void *), void *context, uint32_t timeout_ms);
