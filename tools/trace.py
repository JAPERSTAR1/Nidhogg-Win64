"""Annotated PM4 trace of a captured DCB.

python tools/trace.py work/dcb_40000003d8.bin
"""

import struct
import sys

OPS = {
    0x10: "NOP", 0x15: "DISPATCH_DIRECT", 0x27: "DRAW_INDEX_2",
    0x2A: "INDEX_TYPE", 0x2D: "DRAW_INDEX_AUTO", 0x37: "WRITE_DATA",
    0x3C: "WAIT_REG_MEM", 0x3F: "INDIRECT_BUFFER", 0x46: "EVENT_WRITE",
    0x47: "EVENT_WRITE_EOP", 0x48: "EVENT_WRITE_EOS", 0x58: "ACQUIRE_MEM",
    0x68: "SET_CONFIG_REG", 0x69: "SET_CONTEXT_REG", 0x76: "SET_SH_REG",
    0x79: "SET_UCONFIG_REG", 0x75: "SET_SH_REG_INDIRECT",
    0x76: "SET_SH_REG", 0x63: "SET_UCONFIG?", 0x64: "SET_CONTEXT?",
}

PRIMS = {0: "PointList", 1: "LineList", 2: "LineStrip", 3: "TriangleList",
         4: "TriangleStrip", 5: "TriangleFan", 6: "TriangleFan", 7: "RectList"}

CTX = {0x10F: "PA_CL_VPORT_XSCALE", 0x110: "PA_CL_VPORT_XOFFSET",
       0x111: "PA_CL_VPORT_YSCALE", 0x112: "PA_CL_VPORT_YOFFSET",
       0x113: "PA_CL_VPORT_ZSCALE", 0x114: "PA_CL_VPORT_ZOFFSET",
       0x192: "?", 0x1E0: "CB_TARGET_MASK", 0x200: "CB_SHADER_MASK",
       0x205: "?", 0x2AF: "?", 0x2FA: "?", 0x2FB: "?"}

CFG = {0x00: "VGT_PRIMITIVE_TYPE", 0x01: "VGT_INDEX_TYPE", 0x02: "VGT_NUM_INSTANCES"}


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "work/dcb_40000003d8.bin"
    raw = open(path, "rb").read()
    dw = list(struct.unpack("<%dI" % (len(raw) // 4), raw))
    print(f"{path}: {len(dw)} dwords\n")

    i = 0
    draw = 0
    cfg = {}
    while i < len(dw):
        h = dw[i]
        typ = h >> 30
        if typ == 3:
            op = (h >> 8) & 0xFF
            n = ((h >> 16) & 0x3FFF) + 1
            body = dw[i + 1:i + 1 + n]
            on = OPS.get(op, f"op{op:#04x}")
            print(f"[{i:4d}] {on} n={n}")
            if op in (0x76, 0x69, 0x68, 0x79):
                base = body[0]
                names = CFG if op == 0x68 else CTX
                for k, v in enumerate(body[1:]):
                    nm = names.get(base + k)
                    print(f"        base={base:#04x}  [{base + k:04x}] = {v:08x}"
                          + (f"   {nm}" if nm else ""))
                if op == 0x68:
                    for k, v in enumerate(body[1:]):
                        cfg[base + k] = v
            elif op == 0x2D:
                print(f"        count={body[0]} init={body[1]:#x} "
                      f"prim={PRIMS.get(cfg.get(0, 0), cfg.get(0, 0))}   <-- draw{draw}")
                draw += 1
            elif op == 0x27:
                mx, blo, bhi, cnt, init = body[:5]
                print(f"        max={mx} base={(bhi << 32) | blo:#x} count={cnt} "
                      f"prim={PRIMS.get(cfg.get(0, 0), cfg.get(0, 0))}   <-- draw{draw}")
                draw += 1
            elif op == 0x15:
                print(f"        {body[0]}x{body[1]}x{body[2]} init={body[3]:#x}")
            elif op == 0x2A:
                print(f"        type={body[0]}")
            elif op == 0x37:
                print(f"        {[hex(x) for x in body]}")
            elif op == 0x47:
                print(f"        cntl={body[0]:#x} addr={(body[2] << 32) | body[1]:#x} "
                      f"data={(body[4] << 32) | body[3]:#x}")
            elif op == 0x48:
                print(f"        cntl={body[0]:#x} addr={(body[2] << 32) | body[1]:#x} "
                      f"data={body[3]:#x}")
            elif n <= 8:
                print(f"        {[hex(x) for x in body]}")
            else:
                print(f"        [{len(body)} dwords]")
            i += 1 + n
        elif typ == 0:
            n = 1 + (h & 0xFFFF)
            print(f"[{i:4d}] T0 n={n}")
            i += 1 + n
        elif typ == 1:
            print(f"[{i:4d}] T1 {h:#x} {dw[i + 1]:#x}")
            i += 2
        else:
            print(f"[{i:4d}] T2 {h:#x}")
            i += 1


if __name__ == "__main__":
    main()
