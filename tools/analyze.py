#!/usr/bin/env python3
"""Static survey of the extracted Nidhogg PS4 image.

Produces:
  analysis/report.md       human summary
  analysis/functions.csv   discovered entry points / call targets
  analysis/imports.csv     GOT slots reached through PLT stubs (the SCE surface)
  analysis/strings.txt     printable strings with vaddrs
"""
from __future__ import annotations

import csv
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_IMM, CS_OP_MEM, x86_const

ROOT = Path(__file__).resolve().parent.parent
ELF = ROOT / "work" / "nidhogg.elf"
OUT = ROOT / "analysis"


def load():
    img = json.loads((OUT / "image.json").read_text())
    data = ELF.read_bytes()
    segs = []
    for p in img["phdrs"]:
        if p["type"] != 1:
            continue
        blob = data[p["offset"]:p["offset"] + p["filesz"]]
        mem = bytearray(p["memsz"])
        mem[:len(blob)] = blob
        segs.append(dict(vaddr=p["vaddr"], data=bytes(mem), file=blob,
                         flags=p["flags"], filesz=p["filesz"], memsz=p["memsz"]))
    return img, segs


def read_at(segs, va, n):
    for s in segs:
        if s["vaddr"] <= va < s["vaddr"] + len(s["data"]):
            off = va - s["vaddr"]
            return s["data"][off:off + n]
    return b""


def main():
    img, segs = load()
    text = [s for s in segs if s["flags"] & 4][0]
    entry = img["entry"]

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md.skipdata = True

    base = text["vaddr"]
    code = text["data"]
    plt_slots = Counter()          # got slot -> hit count
    plt_sites = defaultdict(list)  # got slot -> stub vaddrs
    calls = Counter()              # call target -> count
    insn_count = 0
    prologues = set()

    for ins in md.disasm(code, base):
        insn_count += 1
        m = ins.mnemonic
        if m == "jmp" and ins.operands and ins.operands[0].type == CS_OP_MEM:
            op = ins.operands[0].mem
            if op.base == x86_const.X86_REG_RIP:
                target = ins.address + ins.size + op.disp
                plt_slots[target] += 1
                plt_sites[target].append(ins.address)
        elif m == "call":
            if ins.operands[0].type == CS_OP_IMM:
                t = ins.operands[0].imm
                calls[t] += 1
                if t < base or t >= base + len(code):
                    plt_slots[t] += 1
                    plt_sites[t].append(ins.address)
        if ins.size >= 4 and ins.bytes[0] == 0x55:
            prologues.add(ins.address)
        elif ins.size >= 5 and ins.bytes[0] == 0x48 and ins.bytes[1] == 0x83 \
                and ins.bytes[2] == 0xEC:
            prologues.add(ins.address)

    funcs = set(calls)
    funcs |= prologues
    funcs.add(entry)

    # ---- imports: GOT slots live in the writable LOAD, outside .text
    rw = [s for s in segs if s["flags"] & 2]
    def in_image(va):
        return any(s["vaddr"] <= va < s["vaddr"] + len(s["data"]) for s in segs)
