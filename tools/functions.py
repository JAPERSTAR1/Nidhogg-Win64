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
    segs = load()

    def rd(va, n):
        for v, m in segs:
            if v <= va < v + len(m):
                return m[va - v:va - v + n]
        return b""

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md.skipdata = True

    text = rd(TEXT_VA, TEXT_SZ)

    call_targets = defaultdict(int)
    for ins in md.disasm(text, TEXT_VA):
        if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm
            if TEXT_VA <= t < TEXT_VA + TEXT_SZ:
                call_targets[t] += 1

    roots = set(call_targets)
    # pointers in the writable segment that land in text
    data = rd(DATA_VA, DATA_SZ)
    for off in range(0, len(data) - 8, 8):
        q = struct.unpack_from("<Q", data, off)[0]
        if TEXT_VA + 0x1000 <= q < TEXT_VA + TEXT_SZ:
            roots.add(q)

    def walk(start):
        seen = set()
        work = [start]
        plt = set()
        end = start
        while work:
            va = work.pop()
            if va in seen or not (TEXT_VA <= va < TEXT_VA + TEXT_SZ):
                continue
            ptr = va
            steps = 0
            while TEXT_VA <= ptr < TEXT_VA + TEXT_SZ and steps < 4096:
                if ptr in seen:
                    break
                seen.add(ptr)
                ins = next(md.disasm(rd(ptr, 16), ptr), None)
                if ins is None:
                    break
                steps += 1
                end = max(end, ptr + ins.size)
                m = ins.mnemonic
                if m == "call":
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm
                        if PLT_VA <= t < PLT_END:
                            plt.add((t - PLT_VA - 0x10) // 0x10)
                        elif TEXT_VA <= t < TEXT_VA + TEXT_SZ:
                            work.append(t)
                elif m == "jmp":
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm
                        if PLT_VA <= t < PLT_END:
                            plt.add((t - PLT_VA - 0x10) // 0x10)
                        elif TEXT_VA <= t < TEXT_VA + TEXT_SZ:
                            work.append(t)
                    break
                elif m.startswith("j") and m != "jmp":
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM and TEXT_VA <= op.imm < TEXT_VA + TEXT_SZ:
                        work.append(op.imm)
                elif m in ("ret", "ud2", "int3", "hlt"):
                    break
                ptr += ins.size
        return end, plt

    funcs = []
    for r in sorted(roots):
        e, plt = walk(r)
        funcs.append((r, e, sorted(plt)))

    OUT.mkdir(exist_ok=True)
    with open(OUT / "functions.csv", "w", newline="") as f:
        f.write("vaddr,size,plt_calls,callers\n")
        for r, e, plt in funcs:
            f.write(f"0x{r:x},0x{max(e - r, 0):x},{len(plt)},{call_targets.get(r, 0)}\n")

    roots_only = [f for f in funcs if call_targets.get(f[0], 0) == 0]
    with open(OUT / "roots.txt", "w") as f:
        for r, e, plt in sorted(roots_only, key=lambda x: len(x[2]), reverse=True):
            f.write(f"0x{r:08x} size=0x{max(e-r,0):05x} plt={len(plt)} {[hex(p) for p in plt[:10]]}\n")

    print(f"functions  {len(funcs):,}")
    print(f"roots      {len(roots_only):,}")
    print("top roots by distinct import calls:")
    for r, e, plt in sorted(roots_only, key=lambda x: len(x[2]), reverse=True)[:12]:
        print(f"  0x{r:08x}  size=0x{max(e-r,0):05x}  plt={len(plt)}  first={[hex(p) for p in plt[:8]]}")


if __name__ == "__main__":
    main()
