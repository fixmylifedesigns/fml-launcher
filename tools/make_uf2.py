"""Turn cubelaunch.dol into a PicoLoader .uf2, like https://makeo.github.io/PicoLoader/converter/

  python tools/make_uf2.py cubelaunch.dol cubelaunch.uf2

The DOL is stored raw in the Pico's flash at 0x10031000, once for RP2040 (Pico)
and once for RP2350 (Pico 2), and merged with the PicoLoader firmware so one
file flashes everything. Pass --no-firmware to update only the DOL.
"""
import struct
import sys
import urllib.request

FIRMWARE_URL = "https://makeo.github.io/PicoLoader/fw/picoloader.uf2"
MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FAMILY_IDS = [0xE48BFF56, 0xE48BFF59]  # RP2040, RP2350
BASE_ADDR = 0x10031000
PAYLOAD = 256
MAX_SIZE = 16576512


def check_dol(d):
    """Same checks as the converter: the IPL must be able to boot it."""
    u = lambda o: struct.unpack(">I", d[o:o + 4])[0]
    entry, entry_ok, end = u(0xE0), False, 0
    for i in range(18):
        off, addr, size = u(i * 4), u(0x48 + i * 4), u(0x90 + i * 4)
        if off and (off & 3 or off < 0x100):
            return "bad section offset"
        if addr and (addr & 3 or addr < 0x80000000 or addr > 0x81200000):
            return "section address out of range"
        end = max(end, off + size)
        if i < 7 and addr <= entry < addr + size:
            entry_ok = True
    if not entry_ok:
        return "entry point isn't in a text section"
    if end > len(d):
        return "file is truncated"
    bss = u(0xD8)
    if bss and bss < 0x80000000:
        return "bad BSS address"
    return None


def payload_blocks(data, family):
    n = (len(data) + PAYLOAD - 1) // PAYLOAD
    blocks = []
    for i in range(n):
        chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD].ljust(PAYLOAD, b"\0")
        head = struct.pack("<8I", MAGIC0, MAGIC1, 0x2000, BASE_ADDR + i * PAYLOAD, PAYLOAD, i, n, family)
        blocks.append(head + chunk + b"\0" * (512 - 32 - PAYLOAD - 4) + struct.pack("<I", MAGIC_END))
    return blocks


def parse_blocks(data):
    if len(data) % 512:
        raise ValueError("firmware .uf2 isn't a multiple of 512 bytes")
    blocks = [data[i:i + 512] for i in range(0, len(data), 512)]
    for b in blocks:
        m0, m1 = struct.unpack("<2I", b[:8])
        if m0 != MAGIC0 or m1 != MAGIC1 or struct.unpack("<I", b[-4:])[0] != MAGIC_END:
            raise ValueError("firmware .uf2 has a bad block")
    return blocks


def combine(blocks):
    """Renumber blocks per chip family; each family's last block goes at the very end."""
    families = {}
    for b in blocks:
        families.setdefault(struct.unpack("<I", b[28:32])[0], []).append(b)
    body, last = [], []
    for fam_blocks in families.values():
        total = len(fam_blocks)
        for i, b in enumerate(fam_blocks):
            b = b[:20] + struct.pack("<2I", i, total) + b[28:]
            (last if i == total - 1 else body).append(b)
    return b"".join(body + last)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 1
    dol = open(args[0], "rb").read()
    if len(dol) > MAX_SIZE:
        print("DOL is too large for the Pico's flash")
        return 1
    problem = check_dol(dol)
    if problem:
        print(f"Not a bootable DOL: {problem}")
        return 1

    blocks = []
    if "--no-firmware" not in argv:
        req = urllib.request.Request(FIRMWARE_URL, headers={"User-Agent": "CubeLaunch uf2 tool"})
        with urllib.request.urlopen(req, timeout=30) as r:
            blocks += parse_blocks(r.read())
        print(f"firmware: {len(blocks)} blocks from {FIRMWARE_URL}")
    for fam in FAMILY_IDS:
        blocks += payload_blocks(dol, fam)
    out = combine(blocks)
    open(args[1], "wb").write(out)
    print(f"wrote {args[1]} ({len(out) // 512} blocks, {len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
