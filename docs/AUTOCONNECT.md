# Automatic connection fix: 0.2.1-preview.1

The 0.2.0-preview.1 hardware report was: automatic connections fail, while the
Manager Pair button connects the controllers. The following software defects
were reproduced or identified in the released connection paths and corrected.

- Pair/reconnect/scan no longer overwrite the persistent physical-input
  preference. They use a temporary target. Saving mappings during a manual
  connection keeps Auto saved as Auto. Disconnect, cancellation and a 60-second
  unsuccessful attempt restore the original selection and cancel transport-level
  manual modes. Disconnecting an inactive controller leaves the active one alone.
- Explicitly choosing Auto or a specific input clears that input's manual
  disconnect suppression. Ordinary session recovery preserves explicit disconnect
  intent. USB output identity remains independent of physical input selection.
- Classic connection/security requests use BTstack scheduling, instead of raw HCI
  commands which can be rejected while BLE consumes the controller command credit.
  Saved DualSense peers remain connectable for normal PS-button wake. Explicit
  reconnect can initiate a connection. HID setup is bounded and Feature requests
  are queued so calibration/full-report initialization is not lost to busy buffers.
- NS2 normal saved-peer advertisements use the stored key. A saved controller's
  unbound pairing advertisement enters the vendor pairing path. A committed vendor
  key saves the peer before the first input report, and shared-HCI initialization
  always supplies the real receiver address. Normalized BTstack connection events
  cover both legacy and enhanced LE completion without duplicate initialization.

## Updating an existing receiver

0.2.0 may already have saved an unintended DS5-only or NS2-only preference after
Pair and a subsequent mapping/settings save. Its stored format cannot distinguish
this from an intentional user selection, so this update preserves the preference.
After updating, select **Auto** in Manager's physical-input selector and save once,
or send these management commands:

```text
input set auto
settings save
```

This does not erase existing controller keys. Keep a deliberate fixed input
selection if that is the desired behavior. Use normal controller wake for an
already paired controller; use its pairing mode when establishing a new bond.

`status` includes the saved `input_preference`, temporary `connection_target`,
`ns2_state`, `ns2_auto_enabled`, connection attempts and last disconnect reason.
`ns2 status` exposes the BLE diagnostic state and last error without key material.

Host regression tests exercise the real runtime/connection-policy code, production
Classic and BLE state machines, and backpressure/event-order scenarios. Target
build and module import are separate checks. The new firmware still requires a
physical retest of boot, power-off/wake, USB replug and switching between the two
controllers; an offline pass is not a claim that those hardware cases passed.
