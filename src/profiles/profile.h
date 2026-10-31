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
    SonyDsEdge = 3,
    XInput = 4,
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

// Physical input profiles and USB output identities are independent. A profile
// selects the wireless protocol; the persisted output role selects USB codecs.
struct BridgeProfile {
    BridgeProfileDescriptor descriptor;
    void (*init)();
    void (*task)();
    void (*on_input)(const BridgeInputPacket &packet);
    void (*on_usb_output)(const uint8_t *data, size_t len);
};

#endif // DS5_BRIDGE_PROFILE_H
