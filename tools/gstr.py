import sys

ELF = r"C:\Users\puy12\Downloads\g\nidhogg-port\work\nidhogg.elf"
BASE = 0x10000000
SEG_BIAS = 0x4000

data = open(ELF, "rb").read()


def read(vaddr, n=256):
    off = (vaddr - BASE) + SEG_BIAS
    if off < 0 or off >= len(data):
        return None
    end = data.find(b"\x00", off, off + n)
    if end < 0:
        end = off + n
    return data[off:end]


for arg in sys.argv[1:]:
    a = int(arg, 0)
    b = read(a)
    if b is None:
        print(f"{a:#x}  <out of range>")
        continue
    try:
        s = b.decode("utf-8")
    except UnicodeDecodeError:
        s = repr(b)
    print(f"{a:#x}  {s!r}")
