import re
import sys
from collections import Counter, defaultdict

LOG = r"C:\Users\puy12\Downloads\g\nidhogg-port\trace.log"

calls = Counter()
sites = defaultdict(Counter)
order = []

pat = re.compile(r"\[hle (\d+) (.+?)\s*\] ret=\+0x([0-9a-f]+)")
with open(LOG, "r", encoding="latin-1") as f:
    for line in f:
        m = pat.search(line)
        if not m:
            continue
        slot = int(m.group(1))
        site = int(m.group(3), 16)
        calls[slot] += 1
        sites[slot][site] += 1
        if slot not in order:
            order.append(slot)

print("total traced calls:", sum(calls.values()), " slots:", len(order))
print()
print(f"{'slot':>4} {'calls':>7}  name")
for slot, n in calls.most_common(60):
    print(f"{slot:>4} {n:>7}  {sites[slot].most_common(1)[0][0]:#08x} "
          + " ".join(f"{s:#x}x{c}" for s, c in sites[slot].most_common(6)))
