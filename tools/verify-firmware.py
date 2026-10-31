"""Validate a built RP2350 UF2 without accessing hardware (Python stdlib only)."""
import argparse
import hashlib
import json
import pathlib
import struct


def verify(path):
    data = pathlib.Path(path).read_bytes()
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    blocks = len(data) // 512
    seen = set()
    spans = []
    families = set()
    ignored_blocks = 0
    expected_total = None
    for index in range(blocks):
        block = data[index * 512 : (index + 1) * 512]
        magic0, magic1, flags, address, length, number, total, family = struct.unpack_from("<8I", block)
        if (magic0, magic1, struct.unpack_from("<I", block, 508)[0]) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise ValueError(f"Invalid UF2 magic in block {index}")
        # picotool's RP2350-E10 workaround prefixes one deliberately incomplete
        # ABSOLUTE-family transfer, with the explicit RP2_IGNORE_BLOCK extension.
        # Match the generated sentinel exactly; never exempt arbitrary absolute
        # blocks or ignore-tagged application writes from Flash bounds checks.
        if family == 0xE48BFF57:
            if not (index == 0 and flags == 0xA000 and address == 0x10FFFF00
                    and length == 256 and number == 0 and total == 2
                    and block[32:288] == b"\xef" * 256
                    and struct.unpack_from("<I", block, 288)[0] == 0x9957E304
                    and block[292:508] == bytes(216)):
                raise ValueError("Invalid RP2350-E10 ignore sentinel")
            ignored_blocks += 1
            continue
        if flags != 0x2000 or family != 0xE48BFF59:
            raise ValueError(f"Non-RP2350 ARM-S application block {index}")
        if expected_total is None:
            expected_total = total
        if total != expected_total or number >= total or number in seen:
            raise ValueError("UF2 block numbering is incomplete or duplicated")
        if flags & 1 or not 0 < length <= 476 or address < 0x10000000 or address + length > 0x103FA000:
            raise ValueError("UF2 writes outside the application Flash range or into persistent storage")
        seen.add(number)
        spans.append((address, address + length))
        families.add(family)
    if not spans or expected_total != len(seen) or len(seen) + ignored_blocks != blocks:
        raise ValueError("UF2 application block numbering is incomplete")
    spans.sort()
    if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError("UF2 contains overlapping writes")
    return {"path": str(pathlib.Path(path).resolve()), "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "blocks": blocks,
            "applicationBlocks": len(seen), "rp2350E10IgnoredBlocks": ignored_blocks,
            "familyIds": [f"0x{x:08x}" for x in sorted(families)],
            "flashStart": f"0x{spans[0][0]:08x}", "flashEndExclusive": f"0x{spans[-1][1]:08x}",
            "reservedStorageStart": "0x103fa000", "hardwareTested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("uf2")
    args = parser.parse_args()
    print(json.dumps(verify(args.uf2), indent=2))
