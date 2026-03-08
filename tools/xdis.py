"""Disassemble the extracted image by guest virtual address or file offset.

python tools/dis.py 0x10142000 0x10142300
python tools/dis.py --file 0x146000 0x146200
"""

import sys
import capstone

ELF = r"C:\Users\puy12\Downloads\g\nidhogg-port\work\nidhogg.elf"
BASE = 0x10000000
FILE_BIAS = 0x4000          # vaddr = BASE + (fileoff - FILE_BIAS)


def main():
    args = sys.argv[1:]
    by_file = False
    if args and args[0] == "--file":
        by_file = True
        args = args[1:]
    lo = int(args[0], 0) if args else 0x10142000
    hi = int(args[1], 0) if len(args) > 1 else lo + 0x100

    img = open(ELF, "rb").read()

    def to_file(a):
        return a - BASE + FILE_BIAS if not by_file else a

    def to_addr(f):
        return BASE + f - FILE_BIAS if not by_file else f

    fo = to_file(lo)
    if fo < 0 or fo >= len(img):
        print(f"offset {fo:#x} outside image (size {len(img):#x})")
        return
    code = img[fo:to_file(hi)]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    md.skipdata = True
    for insn in md.disasm(code, to_addr(fo) if not by_file else lo):
        tail = ""
        m = insn.op_str
        # annotate rip-relative targets
        if "rip +" in m:
            try:
                disp = int(m.split("rip + ")[1].split("]")[0], 16)
                target = insn.address + insn.size + disp
                tail = f"    ; {target:#x} ({target - BASE:+#x} guest)"
            except Exception:
                pass
        if "rip -" in m:
            try:
                disp = int(m.split("rip - ")[1].split("]")[0], 16)
                target = insn.address + insn.size - disp
                tail = f"    ; {target:#x}"
            except Exception:
                pass
        print(f"{insn.address:#012x}  {insn.mnemonic:<8} {m}{tail}")


if __name__ == "__main__":
    main()
