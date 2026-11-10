import struct
import sys

DLL = sys.argv[1]
TARGETS = [int(a, 0) for a in sys.argv[2:]]

d = open(DLL, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
assert d[pe:pe + 4] == b"PE\0\0"
coff = pe + 4
nsect = struct.unpack_from("<H", d, coff + 2)[0]
optsz = struct.unpack_from("<H", d, coff + 16)[0]
opt = coff + 20
magic = struct.unpack_from("<H", d, opt)[0]
ddoff = opt + (0x70 if magic == 0x20B else 0x60)
exp_rva, exp_size = struct.unpack_from("<II", d, ddoff)
sects = []
so = opt + optsz
for i in range(nsect):
    name = d[so + i * 40:so + i * 40 + 8].rstrip(b"\0").decode()
    vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, so + i * 40 + 8)
    sects.append((name, vaddr, vsize, rawptr, rawsize))


def rva_to_off(rva):
    for name, vaddr, vsize, rawptr, rawsize in sects:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    return None


fo = rva_to_off(exp_rva)
(nnames,) = struct.unpack_from("<I", d, fo + 24)
addr_rva, = struct.unpack_from("<I", d, fo + 28)
name_ptrs, = struct.unpack_from("<I", d, fo + 32)
ordinals, = struct.unpack_from("<I", d, fo + 36)

exports = []
for i in range(nnames):
    no = rva_to_off(struct.unpack_from("<I", d, rva_to_off(name_ptrs) + i * 4)[0])
    end = d.find(b"\0", no)
    nm = d[no:end].decode("latin-1")
    o = struct.unpack_from("<H", d, rva_to_off(ordinals) + i * 2)[0]
    rva = struct.unpack_from("<I", d, rva_to_off(addr_rva) + o * 4)[0]
    exports.append((rva, nm))

exports.sort()
for t in TARGETS:
    best = None
    for rva, nm in exports:
        if rva <= t:
            best = (rva, nm)
        else:
            break
    if best:
        print(f"{t:#x} -> {best[1]}+{t - best[0]:#x}  (rva {best[0]:#x})")
    else:
        print(f"{t:#x} -> below first export")
