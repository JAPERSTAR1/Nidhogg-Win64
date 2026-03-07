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
