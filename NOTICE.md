# Notices and Attribution

Pico Controller Bridge is a Raspberry Pi Pico 2 W firmware project derived from
and inspired by several open-source projects.

## Main Code Base

This repository was created from the `DS5Dongle` Pico firmware code base:

- Project: DS5Dongle
- Upstream: https://github.com/awalol/DS5Dongle
- Local reference revision: `8760ee3 fix: ci artifact path`
- License: MIT License
- License copy: `LICENSES/DS5Dongle-MIT.txt`

The Pico SDK integration, TinyUSB/BTstack firmware structure, build scripts,
UF2 workflow, DualSense-oriented fallback firmware files, and much of the
repository layout come from this code base.

## NS2Pro / Switch 2 Pro Protocol Reference

The NS2Pro / Switch 2 Pro controller protocol work in this repository uses
`y700-switch2-pro-bridge` as an important reference:

- Project: y700-switch2-pro-bridge
- Upstream: https://github.com/LeonChrome/y700-switch2-pro-bridge
- Local reference revision: `3697227 Make README bilingual`
- License: Apache License 2.0
- License copy: `LICENSES/y700-switch2-pro-bridge-Apache-2.0.txt`

The Pico implementation is not a direct ESP-IDF port, but it borrows protocol
knowledge and design choices from that project, especially:

- BLE service/characteristic UUIDs and controller discovery heuristics.
- NS2Pro initialization command sequence.
- FD2 input report layout, including stick packing and motion offset notes.
- Nintendo-style USB HID report identity and report ID usage.
- HID OUT to BLE rumble forwarding strategy.
- Report-rate/status concepts used by the local WebHID tuner.

Source files with protocol-derived implementation notes include:

- `src/ns2/ns2_gatt.cpp`
- `src/ns2/ns2_input.cpp`
- `src/ns2/ns2_usb.cpp`
- `src/ns2/ns2_usb_descriptors.cpp`
- `tools/ns2-webhid-tuner.html`
- `firmware/sf32lb52/protocol/sf32lb52_ns2_protocol.c`
- `firmware/sf32lb52/src/ble_gatt_sifli.c`
- `firmware/sf32lb52/src/ns2_profile.c`

The isolated SF32LB52 target also follows DS5Dongle's documented DualSense HID
report and Bluetooth HIDP behavior while using SiFli SDK/CherryUSB APIs rather
than copying the Pico SDK, TinyUSB, or BTstack runtime architecture.  Relevant
files are under `firmware/sf32lb52/src/ds5_classic_sifli.c`,
`firmware/sf32lb52/protocol/sf32lb52_bridge_protocol.c`, and
`firmware/sf32lb52/usb/sf32lb52_usb_device.c`.

## Additional Design References

### Current SF32LB52 Audio-Haptics and Role-Mapping References

The current SF32LB52 audio-haptics conversion and cross-role mapping were
independently implemented after comparing several actively maintained public
projects at pinned revisions:

- Switch2Connect, revision `688f8149ff5441efad713997def484c3cc5e90cc`
  (2026-08-23): DualSense 4-channel UAC capture, channel 3/4 spectral analysis,
  independent ordinary/audio rumble state, and NS2Pro HD-rumble scheduling.
  License file at this revision: GNU GPL v3.
  https://github.com/TommyWabg/Switch2Connect/tree/688f8149ff5441efad713997def484c3cc5e90cc
- S2P-XInput-Lite, revision `1fd759bdcabdfb265bf00c84b3b83e6f205e9694`
  (2026-08-20): audio-haptics activity gating and saturating soft mixing with
  ordinary rumble. License file at this revision: GNU GPL v3.
  https://github.com/duoduo-88/S2P-XInput-Lite/tree/1fd759bdcabdfb265bf00c84b3b83e6f205e9694
- VIIPER, revision `88f66f1ed0c3716c78f810d92b1924112093f896`:
  current DualSense and NS2Pro report packing and cross-role axis conventions.
  License file at this revision: GNU GPL v3.
  https://github.com/Alia5/VIIPER/tree/88f66f1ed0c3716c78f810d92b1924112093f896

No source file from these projects is vendored or copied into this repository.
The SF32 implementation is a fixed-point, allocation-free implementation for
the board's RT-Thread/CherryUSB data path. The older Y700 project remains a
historical NS2 protocol reference for the Pico-era implementation; it is not
the implementation basis for the SF32 audio-haptics converter.

The SF32LB52 bridge architecture and compatibility checklist were also
compared against these public projects supplied as design references:

- https://github.com/AizawaHikaru233/DS5_NS2Pro_Dongle
- https://github.com/lcyyun/ns2pro-bridge
- https://github.com/lcyyun/pico-controller-bridge

They informed interoperability checks such as single-active controller
selection, parsed/repacked USB reports, role management, rumble translation,
and motion preservation. No source file from those three repositories is
vendored into the isolated SF32LB52 target; their own licenses and notices
remain authoritative for their code.

## Bundled Third-Party Components

The repository also contains third-party components inherited from DS5Dongle.
Their license files remain in their original directories:

- Raspberry Pi Pico SDK, TinyUSB, and BTstack are used through the Pico SDK
  build environment.
- Opus codec sources are under `lib/opus/` with their upstream license files.
- Cockos WDL sources are under `lib/WDL/` with their upstream license files.
- Additional small third-party components inside those libraries keep their
  own license files next to the source.

The Windows Bridge Manager uses HidSharp 2.6.4 for unpackaged desktop HID
enumeration and raw input/output/feature reports:

- Project: HidSharp
- Upstream: https://github.com/IntergatedCircuits/HidSharp
- License: Apache License 2.0
- License copy: `LICENSES/HidSharp-Apache-2.0.txt`

## Trademarks

This project is not affiliated with, endorsed by, or sponsored by Nintendo,
Sony, Valve, Raspberry Pi, or any other hardware/software vendor. Nintendo
Switch, Switch Pro Controller, DualSense, Steam, Raspberry Pi, and related
names are trademarks of their respective owners.
