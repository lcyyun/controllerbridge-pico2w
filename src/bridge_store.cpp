#include "bridge_runtime.h"
#include "bridge_crc32.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"
#include <cstring>

namespace {
// Last six sectors: two bridge journal slots, NS2, BTstack bank A/B, DS5.
constexpr uint32_t first_slot = PICO_FLASH_SIZE_BYTES - 6 * FLASH_SECTOR_SIZE;
struct Record { uint32_t magic, generation, length, checksum; uint8_t payload[84]; };
constexpr uint32_t magic = 0x32524250; // PBR2
static_assert(sizeof(Record) <= FLASH_PAGE_SIZE);
static_assert(offsetof(Record, checksum) == 12 && offsetof(Record, payload) == 16);
uint32_t record_checksum(const Record &record) {
    // Cover generation and length as well as payload. Otherwise a
    // single damaged generation bit could promote an obsolete configuration.
    uint32_t crc = bridge_crc32_update(0xffffffffu, &record, offsetof(Record, checksum));
    return bridge_crc32_update(crc, record.payload, sizeof(record.payload)) ^ 0xffffffffu;
}
const Record *slot(unsigned index) { return reinterpret_cast<const Record *>(XIP_BASE + first_slot + index * FLASH_SECTOR_SIZE); }
bool valid(const Record *r) {
    return r->magic == magic && r->length == sizeof(r->payload) &&
        r->checksum == record_checksum(*r);
}
int latest() {
    bool a = valid(slot(0)), b = valid(slot(1));
    if (!a) return b ? 1 : -1;
    const uint32_t delta = slot(1)->generation - slot(0)->generation;
    return b && delta != 0 && delta < 0x80000000u ? 1 : 0;
}
struct Write { uint32_t offset; uint8_t page[FLASH_PAGE_SIZE]; };
void write_flash(void *context) {
    auto *w = static_cast<Write *>(context);
    flash_range_erase(w->offset, FLASH_SECTOR_SIZE);
    flash_range_program(w->offset, w->page, FLASH_PAGE_SIZE);
}
}
bool bridge_store_load(uint8_t *wire, size_t size) {
    const int index = latest();
    if (!wire || size != sizeof(Record::payload) || index < 0) return false;
    memcpy(wire, slot(index)->payload, size); return true;
}
bool bridge_store_save(const uint8_t *wire, size_t size) {
    if (!wire || size != sizeof(Record::payload)) return false;
    const int previous = latest();
    const unsigned index = previous == 0 ? 1 : 0;
    Record record{}; record.magic = magic; record.length = size;
    record.generation = previous < 0 ? 1 : slot(previous)->generation + 1;
    memcpy(record.payload, wire, size); record.checksum = record_checksum(record);
    Write write{}; write.offset = first_slot + index * FLASH_SECTOR_SIZE;
    memset(write.page, 0xff, sizeof(write.page)); memcpy(write.page, &record, sizeof(record));
    if (flash_safe_execute(write_flash, &write, 500) != PICO_OK) return false;
    return valid(slot(index)) && memcmp(slot(index), &record, sizeof(record)) == 0;
}
