#!/usr/bin/env python3
"""Identify import slots from their call sites.

The image has no per-import names, so identity has to come from behaviour:
what arguments the call site sets up, what it does with the result, and the
strings the surrounding code prints.  This dumps, for every PLT slot, the
call sites with their immediate argument setup and the nearest string.
"""
from __future__ import annotations

import re
from collections import Counter, defaultdict

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "work" / "nidhogg.elf"
OUT = ROOT / "analysis"

BIAS = 0x4000            # file offset -> vaddr, uniform for both LOADs
TEXT_END = 0x1ECF58
PLT0 = 0x171BD0          # entry 0 (the resolver head is at 0x171bc0)
NSLOTS = 465


def string_at(data: bytes, va: int, maxlen: int = 120) -> str | None:
    if not (0 <= va < len(data)):
        return None
    m = re.match(rb"[\x20-\x7e]{4,}", data[va:va + maxlen])
    return m.group().decode("latin1") if m else None


def main() -> None:
    data = open(ELF, "rb").read()
    code = data[BIAS:BIAS + TEXT_END]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md.skipdata = True

    insns = [i for i in md.disasm(code, 0) if i.id != 0]
    index = {i.address: n for n, i in enumerate(insns)}

    sites: dict[int, list[int]] = defaultdict(list)
    for n, ins in enumerate(insns):
        if ins.mnemonic not in ("call", "jmp"):
            continue
        if not ins.operands or ins.operands[0].type != X86_OP_IMM:
            continue
        t = ins.operands[0].imm
        if PLT0 <= t < PLT0 + 16 * NSLOTS and (t - PLT0) % 16 == 0:
            sites[(t - PLT0) // 16].append(n)

    with open(OUT / "slot_strings.txt", "w", encoding="utf-8") as out:
        for slot in range(NSLOTS):
            ns = sites.get(slot, [])
            out.write(f"=== slot {slot}  0x{PLT0 + 16 * slot:06x}  {len(ns)} sites\n")
            if not ns:
                out.write("\n")
                continue
            for n in ns[:6]:
                base = max(0, n - 12)
                for k in range(base, min(len(insns), n + 8)):
                    ins = insns[k]
                    mark = ">>" if k == n else "  "
                    out.write(f"{mark} {ins.address:08x}  {ins.mnemonic} {ins.op_str}\n")
                    for op in ins.operands:
                        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                            s = string_at(data, ins.address + ins.size + op.mem.disp)
                            if s:
                                out.write(f"         ; \"{s}\"\n")
                            break
                out.write("\n")

    # console summary: nearest string per slot
    for slot in range(NSLOTS):
        ns = sites.get(slot, [])
        if not ns:
            continue
        found = Counter()
        for n in ns:
            for k in range(max(0, n - 12), min(len(insns), n + 8)):
                ins = insns[k]
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        s = string_at(data, ins.address + ins.size + op.mem.disp)
                        if s:
                            found[s] += 1
                        break
        top = found.most_common(3)
        print(f"slot {slot:3d}  0x{PLT0 + 16 * slot:06x}  n={len(ns):4d}  " +
              " | ".join(s[:52] for s, _ in top))


