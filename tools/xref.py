import struct
import sys

ELF = r"C:\Users\puy12\Downloads\g\nidhogg-port\work\nidhogg.elf"
BASE = 0x10000000
BIAS = 0x4000

data = open(ELF, "rb").read()


def vaddr_to_off(v):
    return v - BASE + BIAS


def off_to_vaddr(o):
    return BASE + o - BIAS


def main():
    target = int(sys.argv[1], 0)
    want = set()
    for a in sys.argv[1:]:
        want.add(int(a, 0))

    hits = []
    for o in range(0, len(data) - 5):
        if data[o] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, o + 1)[0]
        site = off_to_vaddr(o)
        dest = site + 5 + rel
        if dest in want:
            hits.append((site, dest))

    for site, dest in hits:
        print(f"  call {dest:#x} from {site:#x}")


main()
