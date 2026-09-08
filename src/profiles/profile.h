#ifndef DS5_BRIDGE_PROFILE_H
#define DS5_BRIDGE_PROFILE_H

#include <cstddef>
#include <cstdint>

enum class BridgeProfileId : uint8_t {
    Ds5FastPath = 0,
    Ns2ProCompatibility = 1,
};

enum class BridgeUsbIdentity : uint8_t {
    DebugOnly = 0,
    SonyDs5 = 1,
    NintendoSwitchPro = 2,
};

enum class BridgeInputTransport : uint8_t {
    BluetoothClassicHid = 0,
    BluetoothLeGatt = 1,
};

struct BridgeInputPacket {
    BridgeInputTransport transport;
    const uint8_t *data;
    size_t len;
};

struct BridgeProfileDescriptor {
    BridgeProfileId profile_id;
    BridgeInputTransport input_transport;
    BridgeUsbIdentity preferred_usb_identity;
    const char *name;
    bool preserves_ds5_fast_path;
};

// Stage 1 profile contract. This is intentionally not wired into the firmware
// yet; it records the boundary for the later migration without changing the
// DS5Dongle hot path.
struct BridgeProfile {
    BridgeProfileDescriptor descriptor;
    void (*init)();
    void (*task)();
    void (*on_input)(const BridgeInputPacket &packet);
    void (*on_usb_output)(const uint8_t *data, size_t len);
};

#endif // DS5_BRIDGE_PROFILE_H
