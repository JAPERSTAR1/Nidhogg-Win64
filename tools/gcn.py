"""GCN (PS4 / GFX8) disassembler.

Field layouts follow the public GCN ISA and shadPS4's frontend decode.cpp;
opcode numbering follows shadPS4's opcodes.h.

    python tools/gcn.py --at 0xf5c [--end 0x1000]     # arena_probe.bin
    python tools/gcn.py --blobs
"""

import struct
import sys

# ------------------------------------------------------------------ tables --

SOP1 = {
    3: "s_mov_b32", 4: "s_mov_b64", 5: "s_cmov_b32", 6: "s_cmov_b64",
    7: "s_not_b32", 8: "s_not_b64", 9: "s_wqm_b32", 10: "s_wqm_b64",
    11: "s_brev_b32", 12: "s_brev_b64", 13: "s_bcnt0_i32_b32",
    14: "s_bcnt0_i32_b64", 15: "s_bcnt1_i32_b32", 16: "s_bcnt1_i32_b64",
    17: "s_ff0_i32_b32", 18: "s_ff0_i32_b64", 19: "s_ff1_i32_b32",
    20: "s_ff1_i32_b64", 21: "s_flbit_i32_b32", 22: "s_flbit_i32_b64",
    23: "s_flbit_i32", 24: "s_flbit_i32_i64", 25: "s_sext_i32_i8",
    26: "s_sext_i32_i16", 27: "s_bitset0_b32", 28: "s_bitset0_b64",
    29: "s_bitset1_b32", 30: "s_bitset1_b64", 31: "s_getpc_b64",
    32: "s_setpc_b64", 33: "s_swappc_b64", 34: "s_rfe_b64",
    36: "s_and_saveexec_b64", 37: "s_or_saveexec_b64",
    38: "s_xor_saveexec_b64", 39: "s_andn2_saveexec_b64",
    40: "s_orn2_saveexec_b64", 41: "s_nand_saveexec_b64",
    42: "s_nor_saveexec_b64", 43: "s_xnor_saveexec_b64",
    44: "s_quadmask_b32", 45: "s_quadmask_b64", 46: "s_movrels_b32",
    47: "s_movrels_b64", 48: "s_movreld_b32", 49: "s_movreld_b64",
    50: "s_cbranch_join", 51: "s_mov_regrd_b32", 52: "s_abs_i32",
    53: "s_mov_fed_b32",
}

SOP2 = {
    0: "s_add_u32", 1: "s_sub_u32", 2: "s_add_i32", 3: "s_sub_i32",
    4: "s_addc_u32", 5: "s_subb_u32", 6: "s_min_i32", 7: "s_min_u32",
    8: "s_max_i32", 9: "s_max_u32", 10: "s_cselect_b32", 11: "s_cselect_b64",
    14: "s_and_b32", 15: "s_and_b64", 16: "s_or_b32", 17: "s_or_b64",
    18: "s_xor_b32", 19: "s_xor_b64", 20: "s_andn2_b32", 21: "s_andn2_b64",
    22: "s_orn2_b32", 23: "s_orn2_b64", 24: "s_nand_b32", 25: "s_nand_b64",
    26: "s_nor_b32", 27: "s_nor_b64", 28: "s_xnor_b32", 29: "s_xnor_b64",
    30: "s_lshl_b32", 31: "s_lshl_b64", 32: "s_lshr_b32", 33: "s_lshr_b64",
    34: "s_ashr_i32", 35: "s_ashr_i64", 36: "s_bfm_b32", 37: "s_bfm_b64",
    38: "s_mul_i32", 39: "s_bfe_u32", 40: "s_bfe_i32", 41: "s_bfe_u64",
    42: "s_bfe_i64", 43: "s_cbranch_g_fork", 44: "s_absdiff_i32",
}

SOPK = {0: "s_movk_i32", 2: "s_cmovk_i32", 3: "s_cmpk_eq_i32",
        4: "s_cmpk_lg_i32", 5: "s_cmpk_gt_i32", 6: "s_cmpk_ge_i32",
        7: "s_cmpk_lt_i32", 8: "s_cmpk_le_i32", 9: "s_cmpk_eq_u32",
        10: "s_cmpk_lg_u32", 11: "s_cmpk_gt_u32", 12: "s_cmpk_ge_u32",
        13: "s_cmpk_lt_u32", 14: "s_cmpk_le_u32", 15: "s_addk_i32",
        16: "s_mulk_i32", 17: "s_cbranch_i_fork", 18: "s_getreg_b32",
        19: "s_setreg_b32", 20: "s_getreg_regrd_b32",
        21: "s_setreg_imm32_b32"}

SOPC = {0: "s_cmp_eq_i32", 1: "s_cmp_lg_i32", 2: "s_cmp_gt_i32",
        3: "s_cmp_ge_i32", 4: "s_cmp_lt_i32", 5: "s_cmp_le_i32",
        6: "s_cmp_eq_u32", 7: "s_cmp_lg_u32", 8: "s_cmp_gt_u32",
        9: "s_cmp_ge_u32", 10: "s_cmp_lt_u32", 11: "s_cmp_le_u32",
        12: "s_bitcmp0_b32", 13: "s_bitcmp1_b32", 14: "s_bitcmp0_b64",
        15: "s_bitcmp1_b64", 16: "s_setvskip"}

SOPP = {0: "s_nop", 1: "s_endpgm", 2: "s_branch", 4: "s_cbranch_scc0",
        5: "s_cbranch_scc1", 6: "s_cbranch_vccz", 7: "s_cbranch_vccnz",
        8: "s_cbranch_execz", 9: "s_cbranch_execnz", 10: "s_barrier",
        11: "s_setkill", 12: "s_waitcnt", 13: "s_sethalt", 14: "s_sleep",
        15: "s_setprio", 16: "s_sendmsg", 17: "s_sendmsghalt", 18: "s_trap",
        19: "s_icache_inv", 20: "s_incperflevel", 21: "s_decperflevel",
        22: "s_ttracedata"}

VOP1 = {
    0: "v_nop", 1: "v_mov_b32", 2: "v_readfirstlane_b32",
    3: "v_cvt_i32_f64", 4: "v_cvt_f64_i32", 5: "v_cvt_f32_i32",
    6: "v_cvt_f32_u32", 7: "v_cvt_u32_f32", 8: "v_cvt_i32_f32",
    9: "v_mov_fed_b32", 10: "v_cvt_f16_f32", 11: "v_cvt_f32_f16",
    12: "v_cvt_rpi_i32_f32", 13: "v_cvt_flr_i32_f32", 14: "v_cvt_off_f32_i4",
    15: "v_cvt_f32_f64", 16: "v_cvt_f64_f32", 17: "v_cvt_f32_ubyte0",
    18: "v_cvt_f32_ubyte1", 19: "v_cvt_f32_ubyte2", 20: "v_cvt_f32_ubyte3",
    21: "v_cvt_u32_f64", 22: "v_cvt_f64_u32", 23: "v_trunc_f64",
    24: "v_ceil_f64", 25: "v_rndne_f64", 26: "v_floor_f64", 32: "v_fract_f32",
    33: "v_trunc_f32", 34: "v_ceil_f32", 35: "v_rndne_f32",
    36: "v_floor_f32", 37: "v_exp_f32", 38: "v_log_clamp_f32",
    39: "v_log_f32", 40: "v_rcp_clamp_f32", 41: "v_rcp_legacy_f32",
    42: "v_rcp_f32", 43: "v_rcp_iflag_f32", 44: "v_rsq_clamp_f32",
    45: "v_rsq_legacy_f32", 46: "v_rsq_f32", 51: "v_sqrt_f32",
    53: "v_sin_f32", 54: "v_cos_f32", 55: "v_not_b32", 56: "v_bfrev_b32",
    57: "v_ffbh_u32", 58: "v_ffbl_b32", 59: "v_ffbh_i32",
    63: "v_frexp_exp_i32_f32", 64: "v_frexp_mant_f32", 66: "v_movreld_b32",
    67: "v_movrels_b32", 68: "v_movrelsd_b32", 69: "v_log_legacy_f32",
    70: "v_exp_legacy_f32",
}

VOP2 = {
    0: "v_cndmask_b32", 1: "v_readlane_b32", 2: "v_writelane_b32",
    3: "v_add_f32", 4: "v_sub_f32", 5: "v_subrev_f32", 6: "v_mac_legacy_f32",
    7: "v_mul_legacy_f32", 8: "v_mul_f32", 9: "v_mul_i32_i24",
    10: "v_mul_hi_i32_i24", 11: "v_mul_u32_u24", 12: "v_mul_hi_u32_u24",
    13: "v_min_legacy_f32", 14: "v_max_legacy_f32", 15: "v_min_f32",
    16: "v_max_f32", 17: "v_min_i32", 18: "v_max_i32", 19: "v_min_u32",
    20: "v_max_u32", 21: "v_lshr_b32", 22: "v_lshrrev_b32",
    23: "v_ashr_i32", 24: "v_ashrrev_i32", 25: "v_lshl_b32",
    26: "v_lshlrev_b32", 27: "v_and_b32", 28: "v_or_b32", 29: "v_xor_b32",
    30: "v_bfm_b32", 31: "v_mac_f32", 32: "v_madmk_f32", 33: "v_madak_f32",
    34: "v_bcnt_u32_b32", 35: "v_mbcnt_lo_u32_b32", 36: "v_mbcnt_hi_u32_b32",
    37: "v_add_i32", 38: "v_sub_i32", 39: "v_subrev_i32", 40: "v_addc_u32",
    41: "v_subb_u32", 42: "v_subbrev_u32", 43: "v_ldexp_f32",
    44: "v_cvt_pkaccum_u8_f32", 45: "v_cvt_pknorm_i16_f32",
    46: "v_cvt_pknorm_u16_f32", 47: "v_cvt_pkrtz_f16_f32",
    48: "v_cvt_pk_u16_u32", 49: "v_cvt_pk_i16_i32",
}

VOP3N = {320: "v_mad_legacy_f32", 321: "v_mad_f32", 322: "v_mad_i32_i24",
         323: "v_mad_u32_u24", 324: "v_cubeid_f32", 325: "v_cubesc_f32",
         326: "v_cubetc_f32", 327: "v_cubema_f32", 328: "v_bfe_u32",
         329: "v_bfe_i32", 330: "v_bfi_b32", 331: "v_fma_f32",
         332: "v_fma_f64", 333: "v_lerp_u8", 334: "v_alignbit_b32",
         335: "v_alignbyte_b32", 336: "v_mullit_f32", 337: "v_min3_f32",
         338: "v_min3_i32", 339: "v_min3_u32", 340: "v_max3_f32",
         341: "v_max3_i32", 342: "v_max3_u32", 343: "v_med3_f32",
         344: "v_med3_i32", 345: "v_med3_u32", 346: "v_sad_u8",
         347: "v_sad_hi_u8", 348: "v_sad_u16", 349: "v_sad_u32",
         350: "v_cvt_pk_u8_f32", 351: "v_div_fixup_f32", 353: "v_lshl_b64",
         354: "v_lshr_b64", 355: "v_ashr_i64", 356: "v_add_f64",
         357: "v_mul_f64", 358: "v_min_f64", 359: "v_max_f64",
         360: "v_ldexp_f64", 361: "v_mul_lo_u32", 362: "v_mul_hi_u32",
         363: "v_mul_lo_i32", 364: "v_mul_hi_i32", 365: "v_div_scale_f32",
         366: "v_div_scale_f64", 367: "v_div_fmas_f32", 368: "v_div_fmas_f64",
         369: "v_msad_u8", 370: "v_qsad_u8", 371: "v_mqsad_u8",
         372: "v_trig_preop_f64", 373: "v_mqsad_u32_u8", 374: "v_mad_u64_u32",
         375: "v_mad_i64_i32"}

CMP_F = ["f", "lt", "eq", "le", "gt", "lg", "ge", "o",
         "u", "nge", "nlg", "ngt", "nle", "neq", "nlt", "tru"]


def vopc_name(op):
    block = op & 0xC0
    low = op & 0x3F
    kind = {0x00: "f32", 0x40: "f16", 0x80: "i32", 0xC0: "u32"}.get(block)
    if kind is None or low > 0x0F:
        return f"v_cmp_{op:#04x}"
    name = "tru" if low == 0x0F else CMP_F[low]
    if low == 0x00:
        name = "f"
    return f"v_cmp_{name}_{kind}"


VINTRP = {0: "v_interp_p1_f32", 1: "v_interp_p2_f32", 2: "v_interp_mov_f32"}

SMRD = {0: "s_load_dword", 1: "s_load_dwordx2", 2: "s_load_dwordx4",
        3: "s_load_dwordx8", 4: "s_load_dwordx16", 8: "s_buffer_load_dword",
        9: "s_buffer_load_dwordx2", 10: "s_buffer_load_dwordx4",
        11: "s_buffer_load_dwordx8", 12: "s_buffer_load_dwordx16",
        29: "s_dcache_inv_vol", 30: "s_memtime", 31: "s_dcache_inv"}

SMRD_WIDTH = {0: 1, 1: 2, 2: 4, 3: 8, 4: 16, 8: 1, 9: 2, 10: 4, 11: 8, 12: 16}

MIMG = {0: "image_load", 1: "image_load_mip", 2: "image_load_pck",
        3: "image_load_pck_sgn", 4: "image_load_mip_pck",
        5: "image_load_mip_pck_sgn", 8: "image_store", 9: "image_store_mip",
        10: "image_store_pck", 11: "image_store_mip_pck",
        14: "image_get_resinfo", 15: "image_atomic_swap",
        16: "image_atomic_cmpswap", 17: "image_atomic_add",
        18: "image_atomic_sub", 27: "image_atomic_smin",
        28: "image_atomic_umin", 29: "image_atomic_smax",
        30: "image_atomic_umax", 31: "image_atomic_and",
        32: "image_sample", 33: "image_sample_cl", 34: "image_sample_d",
        35: "image_sample_d_cl", 36: "image_sample_l", 37: "image_sample_b",
        38: "image_sample_b_cl", 39: "image_sample_lz",
        40: "image_sample_c", 41: "image_sample_c_cl",
        42: "image_sample_c_d", 43: "image_sample_c_d_cl",
        44: "image_sample_c_l", 45: "image_sample_c_b",
        46: "image_sample_c_b_cl", 47: "image_sample_c_lz",
        48: "image_sample_o", 64: "image_gather4", 65: "image_gather4_cl",
        96: "image_get_lod"}

MUBUF = {0: "buffer_load_format_x", 1: "buffer_load_format_xy",
         2: "buffer_load_format_xyz", 3: "buffer_load_format_xyzw",
         4: "buffer_store_format_x", 5: "buffer_store_format_xy",
         6: "buffer_store_format_xyz", 7: "buffer_store_format_xyzw",
         8: "buffer_load_ubyte", 9: "buffer_load_sbyte",
         10: "buffer_load_ushort", 11: "buffer_load_sshort",
         12: "buffer_load_dword", 13: "buffer_load_dwordx2",
         14: "buffer_load_dwordx4", 15: "buffer_load_dwordx3",
         16: "buffer_store_byte", 18: "buffer_store_short",
         20: "buffer_store_dword", 21: "buffer_store_dwordx2",
         22: "buffer_store_dwordx4", 23: "buffer_store_dwordx3",
         24: "buffer_load_ubyte_d16?", 48: "buffer_atomic_swap",
         49: "buffer_atomic_cmpswap", 50: "buffer_atomic_add"}

EXP_TGT = {0: "mrt0", 1: "mrt1", 2: "mrt2", 3: "mrt3", 4: "mrt4",
           5: "mrt5", 6: "mrt6", 7: "mrt7", 8: "mrtz", 9: "pos0",
           10: "pos1", 11: "pos2", 12: "pos3", 13: "param0", 14: "param1",
           15: "param2", 16: "param3", 17: "param4", 18: "param5",
           19: "param6", 20: "param7", 21: "param8", 22: "param9",
           23: "param10", 24: "param11", 25: "param12", 26: "param13",
           27: "param14", 28: "param15", 32: "pos0_hi", 33: "pos1_hi",
           34: "pos2_hi", 35: "pos3_hi", 36: "param0_hi"}

FLT = {240: 0.5, 241: -0.5, 242: 1.0, 243: -1.0, 244: 2.0, 245: -2.0,
       246: 4.0, 247: -4.0, 248: 0.15915494}

SREG = {102: "flat_scratch_lo", 103: "flat_scratch_hi",
        104: "xnack_lo", 105: "xnack_hi", 106: "vcc_lo", 107: "vcc_hi",
        108: "tba_lo", 109: "tba_hi", 110: "tba_hi", 124: "m0",
        126: "exec_lo", 127: "exec_hi"}


def sreg(c):
    return SREG.get(c, f"s{c}")


def sop(c):
    """Scalar source operand, 8-bit field."""
    if c < 104:
        return sreg(c)
    if c == 255:
        return "lit"
    if c < 128:
        return sreg(c)
    if c <= 192:
        return str(c - 128)
    if c <= 208:
        return str(-(c - 192))
    if c in FLT:
        v = FLT[c]
        return f"{v:g}"
    return {249: "0.15915494", 250: "1.0", 251: "0.5", 252: "2.0",
            253: "0x7fffffff", 254: "-1.0"}.get(c, f"const{c}")


def vsop(c):
    """9-bit source: VGPR when >= 256."""
    return f"v{c - 256}" if c >= 256 else sop(c & 0xFF) if c < 256 else "?"


def se(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def swz(c, neg, ab, k):
    t = vsop(c)
    if ab & (1 << k):
        t = f"|{t}|"
    if neg & (1 << k):
        t = f"-{t}"
    return t


def decode(dw, i):
    w = dw[i]
    t9 = w >> 23
    t6 = w >> 26

    if t9 == 0x17D:                                   # SOP1
        op = (w >> 8) & 0xFF
        sdst = (w >> 16) & 0x7F
        s0 = w & 0xFF
        size = 2 if (s0 == 255 and op in (3, 4, 11, 12)) else 1
        lit = f" 0x{dw[i + 1]:08x}" if size == 2 else ""
        return f"{SOP1.get(op, f'sop1_{op:#04x}')} {sreg(sdst)}, {sop(s0)}{lit}", size
    if t9 == 0x17E:                                   # SOPC
        op = (w >> 16) & 0x7F
        return f"{SOPC.get(op, f'sopc_{op:#04x}')} {sop(w & 0xFF)}, {sop((w >> 8) & 0xFF)}", 1
    if t9 == 0x17F:                                   # SOPP
        op = (w >> 16) & 0x7F
        simm = se(w & 0xFFFF, 16)
        if op == 12:
            return (f"s_waitcnt vmcnt({simm & 0xF}) expcnt({(simm >> 4) & 0x7}) "
                    f"lgkmcnt({(simm >> 8) & 0xF})"), 1
        return f"{SOPP.get(op, f'sopp_{op:#04x}')} {simm}", 1

    if (w >> 25) & 0x7F == 0x3F:                      # VOP1
        op = (w >> 9) & 0xFF
        s0 = w & 0x1FF
        size = 2 if s0 == 255 else 1
        lit = f" 0x{dw[i + 1]:08x}" if size == 2 else ""
        return f"{VOP1.get(op, f'vop1_{op:#04x}')} v{(w >> 17) & 0xFF}, {vsop(s0)}{lit}", size
    if (w >> 25) & 0x7F == 0x3E:                      # VOPC
        op = (w >> 17) & 0xFF
        s0 = w & 0x1FF
        size = 2 if s0 == 255 else 1
        lit = f" 0x{dw[i + 1]:08x}" if size == 2 else ""
        return f"{vopc_name(op)} vcc, {vsop(s0)}, v{(w >> 9) & 0xFF}{lit}", size

    if t6 == 0x34:                                    # VOP3
        op = (w >> 16) & 0x3FF
        d1 = dw[i + 1]
        if op < 256:
            name = vopc_name(op)
        elif op < 320:
            name = VOP2.get(op - 256, f"vop2_{op - 256:#04x}")
        elif op < 384:
            name = VOP3N.get(op, f"vop3_{op:#04x}")
        else:
            name = VOP1.get(op - 384, f"vop1_{op - 384:#04x}")
        ab = (w >> 8) & 7
        neg = (d1 >> 29) & 7
        s0, s1, s2 = d1 & 0x1FF, (d1 >> 9) & 0x1FF, (d1 >> 18) & 0x1FF
        parts = [swz(s0, neg, ab, 0), swz(s1, neg, ab, 1), swz(s2, neg, ab, 2)]
        return f"{name} v{w & 0xFF}, {', '.join(parts)}", 2

    if t6 == 0x3E:                                    # EXP
        en = w & 0xF
        tgt = (w >> 4) & 0x3F
        d1 = dw[i + 1]
        vs = [d1 & 0xFF, (d1 >> 8) & 0xFF, (d1 >> 16) & 0xFF, (d1 >> 24) & 0xFF]
        f = []
        if (w >> 10) & 1:
            f.append("compr")
        if (w >> 11) & 1:
            f.append("done")
        if (w >> 12) & 1:
            f.append("vm")
        n = bin(en).count("1")
        return (f"exp {EXP_TGT.get(tgt, f'tgt{tgt}')} en={en:x} {' '.join(f)} "
                f"[{', '.join('v%d' % v for v in vs[:n])}]"), 2

    if t6 == 0x32:                                    # VINTRP
        return (f"{VINTRP.get((w >> 16) & 3, 'vintrp')} v{(w >> 18) & 0xFF}, "
                f"v{w & 0xFF}, attr{(w >> 10) & 0x3F}.{(w >> 8) & 3}"), 1

    if t6 == 0x3C:                                    # MIMG
        op = (w >> 18) & 0x7F
        d1 = dw[i + 1]
        vaddr = w & 0xFF
        dmask = (w >> 8) & 0xF
        return (f"{MIMG.get(op, f'image_{op}')} v[{vaddr}:{vaddr + 3}] "
                f"vdata=v{d1 & 0xFF} srsrc=s{(d1 >> 16) & 0x1F} "
                f"ssamp=s{(d1 >> 8) & 0x1F} dmask={dmask:x}"
                f"{' unorm' if (w >> 12) & 1 else ''}"), 2

    if t6 == 0x38:                                    # MUBUF
        op = (w >> 18) & 0x7F
        d1 = dw[i + 1]
        return (f"{MUBUF.get(op, f'mubuf_{op}')} vdata=v{(d1 >> 8) & 0xFF}, "
                f"vaddr=v{d1 & 0xFF}, srsrc=s{(d1 >> 16) & 0x1F}, "
                f"soffset={sop((w >> 24) & 0xFF)}, offset={w & 0xFFF}"
                f"{' offen' if (w >> 12) & 1 else ''}"
                f"{' idxen' if (w >> 13) & 1 else ''}"
                f"{' addr64' if (w >> 14) & 1 else ''}"), 2

    if w >> 27 == 0x18:                               # SMRD
        op = (w >> 22) & 0x1F
        sdst = (w >> 15) & 0x7F
        sbase = (w >> 9) & 0x3F
        width = SMRD_WIDTH.get(op, 1)
        imm = (w >> 8) & 1
        off = f"0x{w & 0xFF:x}" if imm else sop(w & 0xFF)
        return (f"{SMRD.get(op, f'smrd_{op}')} s[{sdst}:{sdst + width - 1}], "
                f"s[{sbase}:{sbase + 1}], {off}"), 1

    if w >> 28 == 0xB:                                # SOPK
        op = (w >> 23) & 0x1F
        return (f"{SOPK.get(op, f'sopk_{op}')} s{(w >> 16) & 0x7F}, "
                f"{se(w & 0xFFFF, 16)}"), 1

    if w >> 30 == 0x2:                                # SOP2
        op = (w >> 23) & 0x7F
        s0 = w & 0xFF
        size = 2 if s0 == 255 else 1
        lit = f" 0x{dw[i + 1]:08x}" if size == 2 else ""
        return (f"{SOP2.get(op, f'sop2_{op:#04x}')} s{(w >> 16) & 0x7F}, "
                f"{sop(s0)}, {sop((w >> 8) & 0xFF)}{lit}"), size

    if not (w >> 31):                                 # VOP2
        op = (w >> 25) & 0x3F
        s0 = w & 0x1FF
        if op in (32, 33):
            return (f"{VOP2[op]} v{(w >> 17) & 0xFF}, {vsop(s0)}, "
                    f"v{(w >> 9) & 0xFF}, 0x{dw[i + 1]:08x}"), 2
        size = 2 if s0 == 255 else 1
        lit = f" 0x{dw[i + 1]:08x}" if size == 2 else ""
        return (f"{VOP2.get(op, f'vop2_{op:#04x}')} v{(w >> 17) & 0xFF}, "
                f"{vsop(s0)}, v{(w >> 9) & 0xFF}{lit}"), size

    return f"??? {w:08x}", 1


def disasm(dw, start, end, base):
    i = start
    out = []
    while i < end:
        text, size = decode(dw, i)
        out.append((base + i * 4, text))
        i += size
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else r"C:\Users\puy12\Downloads\g\nidhogg-port\work\arena_probe.bin"
    raw = open(path, "rb").read()
    dw = list(struct.unpack("<%dI" % (len(raw) // 4), raw))
    base = 0x4010000000

    if "--blobs" in sys.argv:
        starts = [i for i in range(len(dw)) if dw[i] == 0xBEEB03FF]
        for st in starts:
            print(f"\n===== marker at +0x{st * 4:x} =====")
            for addr, text in disasm(dw, st + 2, min(st + 2 + 40, len(dw)), base):
                print(f"  {addr:012x}  {text}")
        return

    at = int(next(a for a in sys.argv[1:] if a.startswith("--at=")).split("=")[1], 16)
    endarg = next((a for a in sys.argv[1:] if a.startswith("--end=")), None)
    stop = int(endarg.split("=")[1], 16) if endarg else at + 0x100
    for addr, text in disasm(dw, at // 4, stop // 4, base):
        print(f"  {addr:012x}  {text}")


if __name__ == "__main__":
    main()
