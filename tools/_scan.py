import struct

ROOT = r"C:\Users\puy12\Downloads\g\nidhogg-port\work"
BASE = 0x4000000000
data = open(ROOT + r"\guest768.bin", "rb").read()
print(f"file {len(data)} bytes = {len(data)/2**20:.1f} MiB")

# float4 vertex with w == 1.0 -> the dword 0x3F800000 sits at +12 of the record.
# Find every occurrence at C speed, then test the other three lanes.
needle = b"\x00\x00\x80\x3f"
hits = []
pos = 0
while True:
    i = data.find(needle, pos)
    if i < 0:
        break
    pos = i + 1
    rec = i - 12
    if rec < 0 or rec & 3:
        continue
    x, y, z, w = struct.unpack_from("<4f", data, rec)
    if not (-16 <= x <= 336 and -16 <= y <= 216 and -2048 <= z <= 2048):
        continue
    if not (x == x and y == y and z == z):
        continue
    hits.append(rec)

print(f"candidate vertices: {len(hits)}")

# cluster: consecutive records 16 bytes apart
runs = []
if hits:
    start = prev = hits[0]
    for h in hits[1:]:
        if h == prev + 16:
            prev = h
            continue
        runs.append((start, prev))
        start = prev = h
    runs.append((start, prev))
runs = [(a, b) for a, b in runs if b - a >= 32]
print(f"runs: {len(runs)}")
for a, b in runs[:30]:
    cnt = (b - a) // 16 + 1
    print(f"\n  @ {BASE+a:#x}  {cnt} records")
    for k in range(a, min(b + 16, a + 16 * 18), 16):
        x, y, z, w = struct.unpack_from("<4f", data, k)
        print(f"    {BASE+k:#x}  {x:10.4f} {y:10.4f} {z:12.6f} {w:8.4f}")
