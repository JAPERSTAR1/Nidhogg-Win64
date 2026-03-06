#!/usr/bin/env python3
"""Import inventory from the SCE dynlib segment.

The dynamic table that would name each import is in the truncated tail of this
dump, so this lists what is still legible: the library names the linker recorded
and the weak-stub names, which bound the API surface the port has to cover.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT = ROOT / "work" / "nidhogg.elf"


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    d = path.read_bytes()

    # The dynlib segment starts at 0x342a20 in this image and runs past EOF.
    region = d[0x342a20:]
    libs = Counter(m.group().decode() for m in re.finditer(rb"libSce[A-Za-z0-9_]+", region))
    weak = sorted(m.group().decode() for m in re.finditer(rb"libSce[A-Za-z0-9_]+_stub_weak", region))

    print(f"{path}  ({len(d):,} bytes)\n")
    print(f"{'library':34} {'refs':>5}")
    print("-" * 41)
    for name, n in libs.most_common():
        print(f"{name:34} {n:5}")
    print(f"\nunique library names: {len(libs)}")
    print(f"weak stubs: {len(set(weak))}")
    for w in sorted(set(weak)):
        print(f"  {w}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
