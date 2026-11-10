import re
import sys
from collections import Counter, defaultdict

LOG = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\puy12\Downloads\g\nidhogg-port\trace.log"

pat = re.compile(r"\[hle (\d+)[^\]]*\] ret=\+0x([0-9a-f]+)\s+rdi=([0-9a-f]+) rsi=([0-9a-f]+) rdx=([0-9a-f]+)")
rows = []
for line in open(LOG, "r", encoding="latin-1", errors="replace"):
    m = pat.search(line)
    if m:
        rows.append((int(m.group(1)), int(m.group(2), 16),
                     int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)))

print("traced", len(rows))
for i, (slot, site, a, b, c) in enumerate(rows):
    print(f"{i:4d} slot {slot:3d}  ret +0x{site:06x}  rdi={a:#x} rsi={b:#x} rdx={c:#x}")
