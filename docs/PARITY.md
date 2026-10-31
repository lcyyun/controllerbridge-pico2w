# Pico unified preview parity and verification

This preview targets **Pico 2 W / RP2350 + CYW43439**. A connected RP2350
UART debug bridge is not this receiver and must not be flashed as one.

## Implemented runtime paths

| Area | Previous implementation | Preview implementation |
| --- | --- | --- |
| Physical input | Classic DS5/Edge and BLE NS2 compiled in unified | Both retained; input preference independent of USB role; one selected physical source |
| USB output | USB identity selected by physical profile | DS5, Edge, XInput and NS2Pro selectable for either source; nonblocking re-enumeration; manager HID remains available |
| NS2 pairing/wake | Pairing advertisements used for every reconnect; no complete vendor challenge exchange | Explicit vendor pairing with asynchronous random/AES, LTK persistence, saved peer encryption, normal wake advertisements, continued backoff scans and cleanup after failure |
| Classic lifecycle | Inquiry retry disabled; incomplete manager commands | Pair/reconnect/disconnect/forget, saved peer reconnect, channel/open watchdogs and deferred retries |
| Mapping | No general manager mapping runtime | 25 canonical controls, separate DS5 and NS2 profiles, native button/trigger patching, unchanged touch/motion/vendor bytes, CRC-protected dual-slot save |
| Telemetry | USB retransmission/content-hash rates | Physical DS5 timestamp / NS2 sequence, duplicate suppression, connection epoch, unsigned wrap handling, 250 ms stale state and neutral output |
| Battery | Output identity could supply placeholder values | Only physical input battery; unknown is -1; FD2 voltage is not converted to an invented percentage |
| Motion | Native and NS2 paths | Native preservation and canonical conversion for cross-role output; XInput has no motion fields |
| Microphone | No receive/USB return implementation | DS5 short Opus frames separated from game input; Core 1 decode, bounded ring and USB IN with mute/gain/session reset |
| Feedback | Native Sony and NS2 output | Native paths retained; XInput/cross-role motor conversion; Sony four-channel USB PCM haptic channels can drive NS2 HD motor envelopes |
| Persistence | DS5 and NS2 both erased last Flash sector | Separate DS5/NS2/TLV/mapping sectors; legacy NS2 config migration; validated DS5 CRC; flash-safe Core 1 coordination |
| Management | Mixed F6 binary and 7F JSON; incomplete role/source commands | 7F JSON in all four roles; F6 compatibility retained; settings/save/reset, diagnostics, reboot and BOOTSEL; commands executed by main loop |

Bootstrap migration: the old NS2 implementation saved an address without the
vendor LTK. Such a target is retained, but the new firmware reports that it must
be paired once with `ns2 pair`; it does not invent a bond or silently forget the
address. Normal saved wake reconnect is enabled after that complete pairing.

## Hardware limits and remaining physical validation

- No receiver, physical controller or host-console session has been exercised
  during this delivery. Build and host fixtures do not certify interoperability.
- Test initial pairing, power cycle persistence, NS2 connection-button shutdown
  followed by **any-button wake**, repeated failures, saved reconnect and forget
  on real DS5, Edge and NS2Pro units. Check at least 20 wake cycles per unit.
- Verify all four identities in Windows, actual games and intended consoles.
  XInput uses a development USB VID/PID and Microsoft compatible-ID descriptor;
  driver binding and console-specific authentication are not hardware-verified.
- Verify native Sony speaker/audio/haptics, translated feedback, simultaneous
  radio/audio throughput and safe settings saves during audio playback.
- NS2 has no Sony speaker/microphone hardware. Sony PCM haptic-to-NS2 conversion
  uses per-channel energy envelopes; it is not waveform-identical Sony haptics.
  Audio envelopes and game rumble have independent lifetimes and are soft-mixed
  per band. Ordinary active-band frequency/tone values are retained; silence in
  the audio source restores ordinary rumble without stopping it.
- Standard XInput cannot expose Edge paddles, touchpad, motion or C as separate
  XInput controls. They remain available as **mapping sources** and physical
  manager telemetry, and can be bound to representable targets.
- Default clock is **150 MHz**, the Pico 2 W datasheet rating. The inherited
  320 MHz setting remains an explicit `-ClockKHz 320000` experimental option.
  Neither 150 MHz audio timing nor 320 MHz electrical/thermal stability is
  claimed hardware-validated. No feature is silently removed at 150 MHz.

## Runtime wire contract

`status` and `input status` expose `input_snapshot_schema:1`,
`axis_format:"signed16"`, `button_format:"canonical"`, `active_input`,
`input_valid`, `input_stale`, `input_epoch`, `unique_input_reports`,
`input_rate_time_ms`, `input_rate_supported`, `input_battery_percent`,
`buttons`, `lx/ly/rx/ry`, `lt/rt`, `ax/ay/az/gx/gy/gz`, `motion_valid`.
Sony/NS2 Feature 0x7f uses Y7HID1/Y7HRS1 chunk framing. In NS2 output mode,
manager command Output Report 0x02 also accepts Y7HID1 before rumble routing.
These are physical pre-mapping values. Y grows upward; triggers cover 0..65535.
Unique reports only advance at physical receive boundaries. Polling status or
sending USB reports never advances this counter. A duplicate does not refresh
the physical frame's expiry time. The report counter is uint32 and `input_epoch`
changes when the connection/source lifetime resets.

`usb status` USB/codec audio counters are cumulative since boot; NS2 audio
counters start at scheduler initialization, including a settings reset.
`audio_out_packets/bytes`
and `audio_in_packets/bytes` advance for nonempty completed TinyUSB transfers;
microphone FIFO writes are not counted as transmitted packets. Error counters
record malformed PCM frame alignment and short microphone FIFO writes, not USB
bus errors that TinyUSB does not expose to these callbacks. `audio_bt_reports`
counts accepted audio/control enqueues, `audio_bt_dropped` rejected enqueues, and
`audio_opus_errors` codec creation/encode/decode failures. `audio_haptic_blocks`
counts complete native haptic blocks or NS2 analysis windows independently of
radio enqueue success. `audio_ns2_updates/stops` track nonzero envelope submissions
and active-to-stopped transitions, including session reset and the 150 ms PCM
stream watchdog. `audio_ns2_mixed_ticks` counts scheduler ticks which combine
active audio and ordinary motor sources; polling status never advances it.

Mapping commands are `mapping get|reset|save ds5|ns2pro` and
`mapping set ds5|ns2pro TARGET SOURCE|none`. Profiles are physical, not one per
USB output. Save persists both profiles atomically in the bridge journal.
The 25 names and codecs are inherited from the BL616 pure protocol implementation
in `src/parity`; its MIT license and component ancestry are retained.

`role set ds5|dse|xinput|ns2pro` and `input set auto|ds5|ns2pro` change independent
settings. `settings save` persists them. `settings reset` restores defaults while keeping paired-device records; explicit `forget` removes a bond.
`ds5` and `ns2` accept `pair`, `reconnect`, `disconnect`, `forget`.
`reboot` restarts firmware; `bootsel` enters the RP2350 ROM loader.

## Storage map (4 MiB device)

| Offset | Purpose |
| --- | --- |
| 0x3fa000 | Bridge settings/mapping journal slot A |
| 0x3fb000 | Bridge settings/mapping journal slot B |
| 0x3fc000 | NS2 settings and saved target |
| 0x3fd000–0x3fefff | SDK Bluetooth TLV banks |
| 0x3ff000 | DS5 configuration |

Firmware packaging verification rejects UF2 application writes into these six
sectors. Updating the firmware preserves them. The two bridge slots tolerate an
interrupted inactive-slot write; DS5/NS2 config blocks retain their separate
CRC checks and are not claimed power-cut atomic.

## Sources

- [Pico 2 W datasheet](https://datasheets.raspberrypi.com/picow/pico-2-w-datasheet.pdf)
- [Pico SDK flash coordination](https://www.raspberrypi.com/documentation/pico-sdk/high_level.html#group_pico_flash)
- Pinned Pico SDK 2.2.0, BTstack, TinyUSB and CYW43 commits remain as documented
  in README. Build scripts validate the dependency commits and ARM release.
- BL616 local reference pure codecs/mapping and protocol behavior were read
  without changing that source tree or any shared SDK.
