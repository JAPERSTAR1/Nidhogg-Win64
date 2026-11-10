import collections
import re

path = r"C:\Users\puy12\Downloads\g\nidhogg-port\out.log"
c = collections.Counter()
first = {}
with open(path, encoding="utf-16", errors="replace") as f:
    for line in f:
        if "[hle " not in line:
            continue
        m = re.search(r"\[hle (\d+)", line)
        if m:
            s = int(m.group(1))
            c[s] += 1
            first.setdefault(s, line.rstrip()[:150])

print(f"distinct={len(c)}")
for s, n in c.most_common(45):
    print(f"{s:4d} x{n:7d}  {first[s]}")
