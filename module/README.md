# Firmware Module Contract

This directory is self-contained. PowerShell 7.2 or later is required. It does
not depend on a manager checkout, a monorepo, a default build directory, or
globally installed build/flash tools. Packaging never builds or flashes and
has no serial-port argument.

`module.json` is seeded from the exported manager manifest. The module id,
module version, firmware version, and runtime API version are preserved.
Change versions only as part of an explicitly coordinated release.

## Invocation

Run the applicable command from the firmware repository root:

```powershell
# SF32LB52: explicit directory containing sftool_param.json and its referenced files.
pwsh -NoProfile -File ./module/package.ps1 -BuildDirectory C:\build\sf32-release

# Pico 2 W: explicit completed UF2; its packaged filename comes from module.json.
pwsh -NoProfile -File ./module/package.ps1 -Uf2Path C:\build\pico-release.uf2

# ESP32-S3: metadata-only, with flashMethod None and no firmware artifacts.
pwsh -NoProfile -File ./module/package.ps1
```

Each repository's entry point accepts only its applicable arguments. Absolute
script paths also work from another working directory. Relative input paths
are resolved against the caller's current directory, not the script directory.

## Inputs And Output

- SF32: copies `sftool_param.json` plus every file in `write_flash.files`,
  preserving paths relative to that JSON file. Unreferenced build outputs
  are not included. Empty lists, missing files and duplicate paths are rejected.
- Pico: copies the explicit UF2 unchanged to the manifest's artifact path.
  UF2 length and every block's magic values are checked; this is not a
  firmware compatibility or bootability test.
- ESP32-S3: packages metadata only. It does not infer ESP images or enable
  automatic flashing.
- All repos: copy root `LICENSE`, `NOTICE.md`, and all files under `LICENSES/`.
  Install all applicable SDK/vendor license notices in the repository before
  release packaging. In particular, SF32 SDK license preparation is separate
  from this packaging step; the script does not certify license completeness.

Output is always:

```text
dist/modules/<module-id>-<moduleVersion>.cbmodule
```

An existing package is never overwritten. Explicitly remove or relocate the
old file within `dist` before repacking the same version. Temporary files are
created in unique directories under `dist/stage` and removed on success or
failure. Sources, manifests and root notices are never rewritten.

The `.cbmodule` is a ZIP with exactly one root `module.json`, notices and any
declared `artifacts/`. `MODULE-SHA256.txt` lists SHA-256 for every payload file
except itself. The completed archive is read back and every hash is checked
before it is moved to `dist/modules`.

The local `tools/pack-module-directory.ps1` retains the exported manager
packer's schema, board, HID and artifact validation, with additional
containment and archive verification. `schemas/module-v2.schema.json` is an
unchanged copy of the exported schema. Runtime API 1 manifests do not use the
Runtime API 2 schema.

Absolute/traversing embedded paths, alternate data streams, reserved names,
links, junctions and reparse-point ancestors are rejected. Custom output
locations outside the exact repository destination are not accepted.

## Tests

```powershell
pwsh -NoProfile -File ./module/tests/package.Tests.ps1
```

Tests create a disposable isolated repository under `module/tests/.work-*`,
using synthetic firmware bytes and fixture notices. They validate metadata
packing, all referenced files, unchanged manifest bytes, archive hashes,
hidden files, containment, malformed inputs, overwrite refusal and cleanup.
They do not build, connect to devices, use the network or publish packages.
