import pathlib
import runpy
import struct
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
verify = runpy.run_path(str(root / "tools/verify-firmware.py"))["verify"]
def block(address=0x10000000, number=0, total=1, family=0xe48bff59):
    data = bytearray(512)
    struct.pack_into("<8I", data, 0, 0x0a324655, 0x9e5d5157, 0x2000,
                     address, 256, number, total, family)
    struct.pack_into("<I", data, 508, 0x0ab16f30)
    return data

def sentinel():
    data = block(address=0x10ffff00, total=2, family=0xe48bff57)
    struct.pack_into("<I", data, 8, 0xa000)
    data[32:288] = b"\xef" * 256
    struct.pack_into("<I", data, 288, 0x9957e304)
    return data

with tempfile.TemporaryDirectory(prefix="uf2-", dir=root / "build") as directory:
    file = pathlib.Path(directory) / "candidate.uf2"
    file.write_bytes(block())
    assert verify(file)["familyIds"] == ["0xe48bff59"]
    file.write_bytes(sentinel() + block())
    assert verify(file)["rp2350E10IgnoredBlocks"] == 1
    invalid = [b"", block()[:-1], block(family=0xe48bff56),
               block(address=0x103fa000), block(address=0x0fffff00),
               block(number=1), block(total=2),
               block(total=2) + block(number=1, total=2),  # overlapping addresses
               block(total=2) + block(address=0x10000100, total=2)]  # duplicated index
    bad_magic=block();bad_magic[508]=0
    invalid.extend([bad_magic, sentinel(), sentinel() + sentinel() + block(),
                    block() + sentinel(), sentinel() + block(address=0x103fa000),
                    block(family=0xe48bff57)])
    for offset in (8, 12, 16, 20, 24, 32, 288, 292):
        corrupt = sentinel()
        corrupt[offset] ^= 1
        invalid.append(corrupt + block())
    for data in invalid:
        file.write_bytes(data)
        try:
            verify(file)
        except ValueError:
            continue
        raise AssertionError("Malformed/protected UF2 accepted")
print("PASS: RP2350 ARM-S family, exact E10 sentinel, bounds, numbering, overlap, reserved settings protection")
