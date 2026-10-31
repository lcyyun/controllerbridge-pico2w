#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "../src/bridge_store.cpp"

alignas(4096) uint8_t storage_flash[PICO_FLASH_SIZE_BYTES];
enum class Fault { None, BeforeCallback, AfterErase, PartialProgram, CorruptProgram };
static Fault fault = Fault::None;
static unsigned erases, programs;
static uint32_t erased_offset;

void flash_range_erase(uint32_t offset, size_t size) {
    assert(offset % FLASH_SECTOR_SIZE == 0 && size == FLASH_SECTOR_SIZE);
    assert(offset + size <= sizeof(storage_flash));
    ++erases; erased_offset = offset;
    memset(storage_flash + offset, 0xff, size);
}
void flash_range_program(uint32_t offset, const uint8_t *data, size_t size) {
    assert(offset % FLASH_PAGE_SIZE == 0 && size == FLASH_PAGE_SIZE);
    assert(offset + size <= sizeof(storage_flash));
    ++programs;
    const size_t count = fault == Fault::AfterErase ? 0 :
                         fault == Fault::PartialProgram ? 40 : size;
    for (size_t i = 0; i < count; ++i) storage_flash[offset + i] &= data[i];
    if (fault == Fault::CorruptProgram) storage_flash[offset + offsetof(Record, payload) + 12] ^= 1;
}
int flash_safe_execute(void (*callback)(void *), void *context, uint32_t timeout_ms) {
    assert(timeout_ms == 500);
    if (fault == Fault::BeforeCallback) return PICO_ERROR_TIMEOUT;
    callback(context);
    return PICO_OK;
}

static std::array<uint8_t,84> payload(uint8_t seed) {
    std::array<uint8_t,84> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = seed + static_cast<uint8_t>(i);
    return out;
}
static void expect_load(const std::array<uint8_t,84> &expected) {
    std::array<uint8_t,84> out{};
    assert(bridge_store_load(out.data(),out.size()) && out == expected);
}
static Record *mutable_slot(unsigned index) {
    return reinterpret_cast<Record *>(storage_flash + first_slot + index * FLASH_SECTOR_SIZE);
}
static void reset() {
    memset(storage_flash,0xff,sizeof(storage_flash));fault=Fault::None;erases=programs=0;
}

int main() {
    assert(bridge_crc32("123456789",9) == 0xcbf43926u);
    uint32_t crc=bridge_crc32_update(0xffffffffu,"1234",4);
    assert((bridge_crc32_update(crc,"56789",5)^0xffffffffu)==0xcbf43926u);
    reset();
    auto a=payload(0x11),b=payload(0x62),c=payload(0xa0),out=payload(0x00);
    assert(!bridge_store_load(out.data(),out.size()));
    assert(!bridge_store_save(nullptr,84)&&!bridge_store_save(a.data(),83));
    assert(!bridge_store_load(nullptr,84)&&!bridge_store_load(out.data(),83));
    assert(erases==0&&programs==0);
    assert(bridge_store_save(a.data(),a.size()));expect_load(a);
    assert(latest()==0&&mutable_slot(0)->generation==1);
    std::array<uint8_t,FLASH_SECTOR_SIZE> prior{};
    memcpy(prior.data(),storage_flash+first_slot,prior.size());
    for (Fault injected : {Fault::BeforeCallback,Fault::AfterErase,Fault::PartialProgram,Fault::CorruptProgram}) {
        fault=injected;
        assert(!bridge_store_save(b.data(),b.size()));
        expect_load(a);
        assert(memcmp(prior.data(),storage_flash+first_slot,prior.size())==0);
        if (injected!=Fault::BeforeCallback) assert(erased_offset==first_slot+FLASH_SECTOR_SIZE);
    }
    fault=Fault::None;
    assert(bridge_store_save(b.data(),b.size()));expect_load(b);
    assert(latest()==1&&mutable_slot(1)->generation==2);
    const Record saved_b=*mutable_slot(1);
    // Generation, length, magic, and payload damage must reject the newer slot.
    mutable_slot(1)->generation^=0x40000000u;expect_load(a);
    *mutable_slot(1)=saved_b;mutable_slot(1)->length^=1;expect_load(a);
    *mutable_slot(1)=saved_b;mutable_slot(1)->magic^=1;expect_load(a);
    *mutable_slot(1)=saved_b;mutable_slot(1)->payload[80]^=1;expect_load(a);
    *mutable_slot(1)=saved_b;mutable_slot(1)->checksum^=1;expect_load(a);
    mutable_slot(0)->payload[0]^=1;
    assert(!bridge_store_load(out.data(),out.size()));
    // A subsequent save repairs one slot even if both old records are damaged.
    assert(bridge_store_save(c.data(),c.size()));expect_load(c);
    assert(latest()==0&&mutable_slot(0)->generation==1);
    // Sequence wrap still selects the completed write; equality is deterministic.
    mutable_slot(0)->generation=UINT32_MAX;
    mutable_slot(0)->checksum=record_checksum(*mutable_slot(0));
    assert(bridge_store_save(b.data(),b.size()));
    assert(latest()==1&&mutable_slot(1)->generation==0);expect_load(b);
    puts("PASS: actual flash journal CRC, corruption rejection, failed safe-execute, erase/partial-write powercut, previous-slot retention, generation wrap");
}
