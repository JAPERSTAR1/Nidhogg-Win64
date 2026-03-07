#!/usr/bin/env python3
"""PS4 SELF -> clean ELF extractor for Nidhogg (CUSA00920).

The retail eboot is a SELF container.  The embedded ELF header sits at 0x120
and its program headers at 0x160, but the p_offset values in those headers are
the *original link layout*, not the offsets the SELF actually stores the
segments at.  The authoritative map is the SELF segment table at 0x40: each
0x20-byte record is {flags, file_offset, file_size, mem_size}, and the file
offsets there are the real ones.

What this produces: a standalone ELF with the two LOAD segments laid out at
clean offsets, plus the dynlib chunk appended after them so the relocation
records it contains can still be recovered by the loader.

The magic that proves the map is the interpreter string: PT_INTERP declares
{vaddr 0, filesz 0x15} and the bytes at the SELF's first segment offset are
exactly "/libexec/ld-elf.so.1".
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

ELF_HDR_OFF = 0x120
SELF_SEG_OFF = 0x40
SELF_SEG_SIZE = 0x20
PT_LOAD = 1


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def self_segments(data: bytes):
    """Parse the SELF segment table that runs from 0x40 up to the ELF header."""
    out = []
    for off in range(SELF_SEG_OFF, ELF_HDR_OFF, SELF_SEG_SIZE):
        flags, foff, fsz, msz = struct.unpack_from("<QQQQ", data, off)
        if fsz == 0:
            continue
        out.append(dict(flags=flags, offset=foff, filesz=fsz, memsz=msz))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("self_file")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--json")
    a = ap.parse_args()

    src = Path(a.self_file)
    data = src.read_bytes()

    if data[:4] != b"\x7fELF" and data[ELF_HDR_OFF:ELF_HDR_OFF + 4] != b"\x7fELF":
        raise SystemExit("no ELF header at 0x120")
    eh_off = 0 if data[:4] == b"\x7fELF" else ELF_HDR_OFF
    eh = data[eh_off:eh_off + 64]
    e_entry = struct.unpack_from("<Q", eh, 0x18)[0]
    e_phoff = struct.unpack_from("<Q", eh, 0x20)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", eh, 0x36)
    phdr_off = eh_off + e_phoff

    phdrs = []
    for i in range(e_phnum):
        o = phdr_off + i * e_phentsize
        (p_type, p_flags, p_offset, p_vaddr, p_paddr,
         p_filesz, p_memsz, p_align) = struct.unpack_from("<IIQQQQQQ", data, o)
        phdrs.append(dict(type=p_type, flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                          paddr=p_paddr, filesz=p_filesz, memsz=p_memsz, align=p_align))

    ssegs = self_segments(data)
    print(f"source      {src}  ({len(data):,} bytes)")
    print(f"sha256      {sha256(src)}")
    print(f"elf header  @0x{eh_off:x}  entry=0x{e_entry:x}  phdrs @0x{phdr_off:x} ({e_phnum})")
    print(f"self segs   {len(ssegs)}")
    for i, s in enumerate(ssegs):
        print(f"  [{i}] flags=0x{s['flags']:x} file=0x{s['offset']:x} "
              f"filesz=0x{s['filesz']:x} memsz=0x{s['memsz']:x}")

    # Match each LOAD to the SELF record that stores it (by file size).
    loads = []
    for p in phdrs:
        if p["type"] != PT_LOAD:
            continue
        cand = [s for s in ssegs if s["filesz"] == p["filesz"]]
        if len(cand) != 1:
            raise SystemExit(f"cannot match LOAD filesz=0x{p['filesz']:x} to a SELF record")
        p2 = dict(p)
        p2["self"] = cand[0]
        loads.append(p2)
        print(f"LOAD vaddr 0x{p['vaddr']:x} filesz 0x{p['filesz']:x} "
              f"-> self 0x{cand[0]['offset']:x}")
