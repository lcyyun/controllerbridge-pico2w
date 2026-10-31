#pragma once
#include <cstddef>
#include <cstdint>
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
#define PICO_FLASH_SIZE_BYTES (8u * FLASH_SECTOR_SIZE)
void flash_range_erase(uint32_t offset, size_t size);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t size);
