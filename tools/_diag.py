import struct

ROOT = r"C:\Users\puy12\Downloads\g\nidhogg-port\work"
BASE = 0x4000000000
d = open(ROOT + r"\guest768.bin", "rb").read()
print(f"guest768.bin {len(d)} bytes, covers {BASE:#x}..{BASE+len(d):#x}")


def off_of(addr):
    return addr - BASE


def dump_vertices(addr, count, stride=36):
    print(f"\n=== {addr:#x}  {count} vertices, stride {stride} ===")
    print("      pos(x,y,z)                          color(r,g,b,a)              uv")
    for i in range(count):
        o = off_of(addr) + i * stride
        px, py, pz = struct.unpack_from("<3f", d, o)
        cr, cg, cb, ca = struct.unpack_from("<4f", d, o + 0xC)
        u, v = struct.unpack_from("<2f", d, o + 0x1C)

        def f(x):
            return f"{x:10.5f}"

        print(f"[{i:2d}] {f(px)} {f(py)} {f(pz)}  | {f(cr)} {f(cg)} {f(cb)} {f(ca)} | {f(u)} {f(v)}")


dump_vertices(0x4011408000, 16)

# raw bytes so the layout is unambiguous
print("\n--- raw 0x4011408000..+0x240 ---")
o = off_of(0x4011408000)
for k in range(0, 0x240, 16):
    w = struct.unpack_from("<4I", d, o + k)
    fl = struct.unpack_from("<4f", d, o + k)
    print(f"  {0x4011408000 + k:#x}  " + " ".join(f"{v:08x}" for v in w)
          + " | " + " ".join(f"{v:11.4g}" for v in fl))

print("\n--- index buffer 0x401144e4f0..0x401144e540 ---")
o = off_of(0x401144e4f0)
for k in range(0, 0x50, 16):
    w = struct.unpack_from("<4I", d, o + k)
    print(f"  {0x401144e4f0 + k:#x}  " + " ".join(f"{v:08x}" for v in w))

print("\n--- u16 indices ---")
o = off_of(0x401144e500)
idx = struct.unpack_from("<22H", d, o)
print("  " + " ".join(str(v) for v in idx))
