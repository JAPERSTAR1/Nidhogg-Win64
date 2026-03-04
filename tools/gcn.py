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
