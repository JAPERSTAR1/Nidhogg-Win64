#!/usr/bin/env python3
"""Function discovery for the Nidhogg PS4 image.

Linear sweep finds call targets; pointer tables in the data segment add the
roots that are only reached through vtables (this engine is full of them).
Each root is then walked with a small worklist so the reported extent is real
code, not "everything until the next ret".

Output: analysis/functions.csv  (start,size,plt_calls,callers)
        analysis/roots.txt     functions with no direct caller
"""
from __future__ import annotations

import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "work" / "nidhogg.elf"
OUT = ROOT / "analysis"

TEXT_VA, TEXT_SZ = 0x0, 0x1ECF58
DATA_VA, DATA_SZ = 0x1F0000, 0x22EC48
PLT_VA = 0x16F310
PLT_N = 465
PLT_END = PLT_VA + 0x10 + PLT_N * 0x10


def load():
    img = json.loads((OUT / "image.json").read_text())
    d = ELF.read_bytes()
    segs = []
    for p in img["phdrs"]:
        if p["type"] != 1:
            continue
        m = bytearray(p["memsz"])
        b = d[p["offset"]:p["offset"] + p["filesz"]]
        m[:len(b)] = b
        segs.append((p["vaddr"], bytes(m)))
    return segs


def main():
