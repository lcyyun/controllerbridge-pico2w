# Pico firmware module

`module.json` is the firmware-owned Runtime API 2 contract for
`pico-unified-bridge` 0.2.1-preview.1. It declares real DS5, Edge, XInput and
NS2Pro identities, independent physical input selection, two 25-control
mapping profiles, and settings/recovery operations.

Build the default unified firmware first, then package the explicit completed
UF2 (PowerShell 7.2+):

```powershell
./module/package.ps1 -Uf2Path ./artifacts/unified/controllerbridge-pico2w-unified.uf2
```

Output: `dist/modules/pico-unified-bridge-0.2.1-preview.1.cbmodule`.
Packaging never builds, flashes or accesses a serial port. Existing versions
are never overwritten. It validates UF2 blocks, schema, paths and required
payloads, includes LICENSE/NOTICE/LICENSES, generates MODULE-SHA256.txt, then
reopens and hashes every ZIP entry. Run `tools/verify-firmware.py` first to
also check RP2350 family, block numbering and protected Flash boundaries.

```powershell
./module/tests/package.Tests.ps1
```

The 30 package tests use synthetic firmware in an isolated temporary fixture.
They cover metadata/firmware payloads, overwrite refusal, path containment,
symlink/reparse protection, traversal, malformed files, archive hashes and
cleanup. They do not substitute for the real firmware build or a hardware test.

The existing official Manager update flow uses GitHub release asset SHA-256
and the package's full payload hashes. Firmware release publication adds a
new preview tag/version, keeps the source visibility unchanged and does not
weaken the Manager's update verification. Shared Manager packaging is handled
by its designated integration task; this repository only owns this module.
