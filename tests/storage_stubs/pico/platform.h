#pragma once
#include <cstdint>
extern uint8_t storage_flash[];
#define XIP_BASE reinterpret_cast<uintptr_t>(storage_flash)
