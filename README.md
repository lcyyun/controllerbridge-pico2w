# ControllerBridge Pico 2 W

[中文](README.CN.md)

Standalone experimental Pico 2 W unified receiver firmware, maintained in the
repository `lcyyun/controllerbridge-pico2w`. This is a DS5Dongle-derived
implementation, not a clean-room rewrite. No sibling repository is needed to
compile it. The Windows manager and its module/release packaging are separate.

## Firmware Scope

- Unified physical inputs: DualSense / DualSense Edge Bluetooth Classic and
  NS2Pro BLE. `ENABLE_NS2PRO=OFF` selects the unified DS5-based entry point;
  it does not exclude BLE when `ENABLE_AUTO_PROFILE=ON`.
- Independently selectable DS5, Edge, XInput and NS2Pro USB outputs, full
  per-physical-controller button mapping and persistent settings.
- Physical fresh-frame telemetry, unknown-battery handling, stale neutralization,
  asynchronous pairing/reconnect, rumble, Sony audio/haptics and NS2 HD feedback.
- Default 150 MHz; `-ClockKHz 320000` retains the inherited **unvalidated**
  overclock as an explicit opt-in. Hardware tests are still required.
- Runtime API 2 `.cbmodule` includes the actual UF2 and two complete mapping
  pages. See [parity, protocol and validation limits](docs/PARITY.md).
- NS2-only remains an optional separate image, not the default. Host wake HID
  is unsupported (`src/wake.cpp` is absent). No Pico W release is offered.

## Windows Build

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1
# Optional, distinct NS2-only image:
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1 -Variant ns2pro
```

The helper can install missing tools and fetch pinned SDK sources. Standalone
script use clones this repository into
`%USERPROFILE%\.controllerbridge-pico2w-build\controllerbridge-pico2w`; authenticate Git if required by your fork. Existing cached project checkouts are never reset or pulled.

To use installed dependencies without installing tools or changing SDK checkouts,
put the native host compiler, CMake, Ninja, Git and Python on `PATH`, then run:

```powershell
.\tools\build-windows.ps1 -UseInstalledTools `
  -SdkPath C:\SDKs\pico-sdk `
  -ArmToolchainPath C:\Toolchains\arm-gnu-toolchain
```

The default `unified` configuration is `ENABLE_NS2PRO=OFF`,
`ENABLE_AUTO_PROFILE=ON`, `PICO_BOARD=pico2_w`, `PICO_W_BUILD=OFF`,
`ENABLE_SERIAL=OFF`. For NS2-only, use `ENABLE_NS2PRO=ON` and
`ENABLE_AUTO_PROFILE=OFF` in a separate build directory. The helper also keeps
`standard` (DS5-only) and `debug` (unified with serial/verbose logging) for
developer use; they are not the default release configuration.

Each variant builds CMake target `ds5-bridge` in `build/<variant>/`. The helper
copies only that build's `ds5-bridge.uf2` to:

| Variant | Output |
| --- | --- |
| `unified` | `artifacts/unified/controllerbridge-pico2w-unified.uf2` |
| `ns2pro` | `artifacts/ns2pro/ns2pro-bridge-pico2w.uf2` |

It prints SHA-256 and never flashes or copies to a device/Desktop.
The firmware-owned `module/package.ps1` packages a completed UF2 under the
versioned filename declared in `module/module.json`. It does not flash.

## Dependency Pins

Do not update these dependencies as part of the repository split.

| Dependency | Pin |
| --- | --- |
| Pico SDK | `2.2.0`, `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779` |
| TinyUSB override | `0.20.0`, `3af1bec1a9161ee8dec29487831f7ac7ade9e189` |
| BTstack | `501e6d2b86e6c92bfb9c390bcf55709938e25ac1` |
| cyw43-driver | `dd7568229f3bf7a37737b9e1ef250c26efe75b23` |
| SDK lwIP (poll architecture target; no Wi-Fi runtime) | `77dcd25a72509eb83f72b033d219b1d40cd8eb95` |
| ARM GNU | `14.2.rel1` (compiler reports `14.2.1`) |
| Opus / WDL | Imported vendor snapshots in `lib/`, not submodules |

The helper and CI check SDK commits and the ARM release banner. SDK tools
`pioasm`/`picotool` build locally when needed; initial generation may require
network access. Pico VS Code SDK overrides are opt-in, disabled by the helper
and CI. Installed dependency checkouts are validated, not upgraded.

## Source, CI And Licenses

- `src/`: DS5 runtime, `src/profiles/` unified selection, `src/ns2/` BLE/HID.
- `lib/`: preserved Opus/WDL source and component notices.
- `tools/`: Windows builder and optional WebHID diagnostics. Run
  `node tools/serve-ns2-webhid-tuner.js`, then open
  `http://127.0.0.1:8787/tools/auto-debug-webui.html`.
- `.github/workflows/`: manual build/reusable build/candidate validation only.
  Unified and NS2-only artifacts are distinct. No tag, push or release event
  publishes firmware; candidate checks do not create a GitHub Release.
- `SOURCE-SNAPSHOT.json`: unchanged original import hashes, not a claim that
  subsequently adapted build files still match, and not a build attestation.
- [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), [LICENSES/](LICENSES/):
  MIT project ancestry and the retained Apache-2.0 NS2 protocol-reference notice.
  Also retain [Opus COPYING](lib/opus/COPYING) and WDL's source-level licenses.
  The historical NOTICE includes other original-project targets, which are not
  included here. SDK dependency redistribution notices still require release review.
