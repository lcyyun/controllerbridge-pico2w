#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "ns2/ns2_input.h"
#include "ns2/ns2_gatt.h"

static void counter(uint8_t *report, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) report[i] = static_cast<uint8_t>(value >> (8 * i));
}
static void center(uint8_t *r) { r[10] = r[13] = 0; r[11] = r[14] = 8; r[12] = r[15] = 128; }
static Ns2InputSnapshot snapshot() { Ns2InputSnapshot s{}; ns2_input_get_snapshot(&s); return s; }

int main() {
    ns2_input_reset();
    assert(!snapshot().valid && snapshot().battery_percent == -1);
    uint8_t fd2[60]{};
    center(fd2);
    counter(fd2, UINT32_MAX);
    fd2[31] = 0x74; fd2[32] = 0x0e; // 3700 mV, percentage deliberately unknown.
    fd2[48] = 0x34; fd2[49] = 0x12;
    assert(ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, sizeof(fd2)));
    auto s = snapshot();
    assert(s.fresh && s.unique_updates == 1 && s.source_sequence == UINT32_MAX);
    assert(s.battery_mv == 3700 && s.battery_percent == -1 && s.motion_valid);
    Ns2MotionSample motion{};
    assert(ns2_input_get_motion_sample(&motion) && motion.accel[0] == 0x1234);
    for (int i = 0; i < 100; ++i) assert(ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, sizeof(fd2)));
    s = snapshot();
    assert(!s.fresh && s.unique_updates == 1 && s.duplicate_updates == 100 && s.motion_updates == 1);
    counter(fd2, 0); // Source wrap is a fresh sample even if every control is static.
    assert(ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, sizeof(fd2)));
    assert(snapshot().fresh && snapshot().unique_updates == 2);
    for (uint32_t i = 1; i <= 30; ++i) {
        counter(fd2, i);
        assert(ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, sizeof(fd2)));
    }
    s = snapshot();
    assert(s.unique_updates == 32 && s.lx == 2048 && s.ly == 2048);
    counter(fd2, 31); fd2[7] = 3; // GR + GL; extended keys must survive.
    assert(ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, sizeof(fd2)));
    assert((snapshot().buttons & ((1u << 18) | (1u << 19))) == ((1u << 18) | (1u << 19)));
    assert(!ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, 15));
    assert(!ns2_input_parse_notify(Ns2InputReportKind::Fd2, nullptr, 0));
    assert(snapshot().parse_errors == 2);
    ns2_input_reset(); // New session accepts the same source sequence again.
    assert(ns2_input_parse_notify(Ns2InputReportKind::Fd2, fd2, sizeof(fd2)) && snapshot().fresh);
    uint8_t legacy[11]{};
    legacy[0] = 255; legacy[1] = 9 << 2; legacy[4] = 0x1c;
    assert(ns2_input_parse_notify(Ns2InputReportKind::Legacy, legacy, sizeof(legacy)));
    s = snapshot();
    assert(s.fresh && s.battery_percent == 100 && !s.motion_valid);
    assert((s.buttons & ((1u << 18) | (1u << 19) | (1u << 20))) == ((1u << 18) | (1u << 19) | (1u << 20)));
    legacy[0] = 0; legacy[1] = 15 << 2;
    assert(ns2_input_parse_notify(Ns2InputReportKind::Legacy, legacy, sizeof(legacy)));
    assert(snapshot().fresh && snapshot().battery_percent == -1);

    uint8_t wire[32]{}, payload[17]{};
    payload[1] = 0x42;
    assert(ns2_gatt_build_pair_command(wire, sizeof(wire), 4, payload, sizeof(payload)) == 25);
    assert(wire[0] == 0x15 && wire[1] == 0x91 && wire[3] == 4 && wire[5] == 17 && wire[9] == 0x42);
    assert(ns2_gatt_build_pair_command(wire, 24, 4, payload, sizeof(payload)) == 0);
    assert(ns2_gatt_build_pair_command(wire, sizeof(wire), 4, nullptr, 17) == 0);
    const uint8_t *decoded = nullptr;
    wire[1] = 1; wire[8] = 1;
    assert(ns2_gatt_parse_pair_response(wire, 25, 4, &decoded) && decoded == wire + 8);
    assert(!ns2_gatt_parse_pair_response(wire, 24, 4, &decoded) && !decoded);
    assert(!ns2_gatt_parse_pair_response(wire, 25, 2, &decoded));
    wire[8] = 0;
    assert(!ns2_gatt_parse_pair_response(wire, 25, 4, &decoded));
    const uint8_t mac[] = {1, 2, 3, 4, 5, 6};
    ns2_gatt_set_console_mac(mac);
    const auto &init = ns2_gatt_init_command(0);
    assert(init.data[10] == 6 && init.data[15] == 1);
    std::puts("PASS: physical source counters, duplicate/wrap/reconnect, extended keys, motion, battery, pairing framing/ACK validation/address order");
}
