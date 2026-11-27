/*
 * LATM IR1 -> IR2 translation for MIPS64r2.
 *
 * The instruction semantics follow QEMU target/mips/tcg/translate.c.  The
 * IR1 list and generate/stop structure follow LATA's translate-a64.c.
 */
#include "qemu/osdep.h"
#include "translate.h"
#include "ir1.h"
#define LATM_REGCACHE_WRAP
#include "latm.h"
#include "reg-alloc.h"
#include "env.h"
#include "latm-helpers.h"
#include "internal.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"

TranslateFn translate_functions[MIPS_OP__COUNT];

static void translate_one(DisasContext *s);

/* ------------------------------------------------------------------ */
/* Helper call bridge                                                  */

static void latm_set_pc(DisasContext *s)
{
    uint64_t pc = s->ir1.pc;
    IR2_OPND t = ra_alloc_itemp();

    /* exception_resume_pc() subtracts the branch PC for a delay slot. */
    li_d(t, pc);
    latm_env_st_d(t, env_offset_pc());
    ra_free_itemp(t._reg_num);
}

static void latm_set_delay_flag(DisasContext *s, bool on)
{
    IR2_OPND t = ra_alloc_itemp();
    IR2_OPND mask = ra_alloc_itemp();

    latm_env_ld_w(t, env_offset_hflags());
    li_d(mask, MIPS_HFLAG_B);
    if (on) {
        la_or(t, t, mask);
    } else {
        li_d(mask, ~(uint64_t)MIPS_HFLAG_BMASK);
        la_and(t, t, mask);
    }
    latm_env_st_w(t, env_offset_hflags());
    ra_free_itemp(t._reg_num);
    ra_free_itemp(mask._reg_num);
}

static void latm_spill_st(int slot, IR2_OPND v)
{
    latm_env_st_d(v, offsetof(CPUMIPSState, latm_spill[slot]));
}

static void latm_spill_ld(int slot, IR2_OPND dst)
{
    latm_env_ld_d(dst, offsetof(CPUMIPSState, latm_spill[slot]));
}

static void latm_spill_put(int slot, IR2_OPND v)
{
    latm_spill_st(slot, v);
    ra_free_itemp(v._reg_num);
}

static IR2_OPND latm_spill_get(int slot)
{
    IR2_OPND t = ra_alloc_itemp();
    latm_spill_ld(slot, t);
    return t;
}

static IR2_OPND hcall(uintptr_t fn, int nargs, IR2_OPND *args)
{
    static const int arg_regs[6] = { la_a1, la_a2, la_a3, la_a4, la_a5, la_a6 };
    IR2_OPND f;

    /*
     * Stage every argument in the guest-state scratch area before loading
     * the ABI registers.  Guest $1..$7 are mapped onto a1..a7, so a naive
     * left-to-right move can overwrite an argument that a later move still
     * needs.  The scratch slots are independent of both guest GPRs and the
     * helper prologue.
     */
    for (int i = 0; i < nargs; i++) {
        if (ir2_opnd_is_imm(&args[i])) {
            IR2_OPND t = ra_alloc_itemp();
            li_d(t, ir2_opnd_imm(&args[i]));
            latm_env_st_d(t, offsetof(CPUMIPSState, latm_hcall_args[i]));
            ra_free_itemp(t._reg_num);
        } else {
            latm_env_st_d(args[i], offsetof(CPUMIPSState, latm_hcall_args[i]));
        }
    }

    lata_gen_call_helper_prologue(tcg_ctx);

    for (int i = 0; i < nargs; i++) {
        IR2_OPND dst = ir2_opnd_new(IR2_OPND_GPR, arg_regs[i]);
        latm_env_ld_d(dst, offsetof(CPUMIPSState, latm_hcall_args[i]));
    }
    /* latm_env_ld_d() may use a0 as a large-offset scratch. */
    la_mov64(a0_ir2_opnd, env_ir2_opnd);

    f = ra_alloc_itemp();
    li_d(f, fn);
    la_jirl(ra_ir2_opnd, f, 0);
    ra_free_itemp(f._reg_num);

    lata_gen_call_helper_epilogue(tcg_ctx);
    return a0_ir2_opnd;
}

static IR2_OPND hcall0(uintptr_t fn)
{
    return hcall(fn, 0, NULL);
}

static IR2_OPND hcall1(uintptr_t fn, IR2_OPND a)
{
    IR2_OPND args[1] = { a };
    return hcall(fn, 1, args);
}

static IR2_OPND hcall2(uintptr_t fn, IR2_OPND a, IR2_OPND b)
{
    IR2_OPND args[2] = { a, b };
    return hcall(fn, 2, args);
}

static IR2_OPND hcall3(uintptr_t fn, IR2_OPND a, IR2_OPND b, IR2_OPND c)
{
    IR2_OPND args[3] = { a, b, c };
    return hcall(fn, 3, args);
}

static IR2_OPND hcall4(uintptr_t fn, IR2_OPND a, IR2_OPND b, IR2_OPND c,
                       IR2_OPND d)
{
    IR2_OPND args[4] = { a, b, c, d };
    return hcall(fn, 4, args);
}

static IR2_OPND hcall5(uintptr_t fn, IR2_OPND a, IR2_OPND b, IR2_OPND c,
                       IR2_OPND d, IR2_OPND e)
{
    IR2_OPND args[5] = { a, b, c, d, e };
    return hcall(fn, 5, args);
}

static IR2_OPND hcall6(uintptr_t fn, IR2_OPND a, IR2_OPND b, IR2_OPND c,
                       IR2_OPND d, IR2_OPND e, IR2_OPND f)
{
    IR2_OPND args[6] = { a, b, c, d, e, f };
    return hcall(fn, 6, args);
}

static IR2_OPND imm_opnd(int64_t v)
{
    IR2_OPND t = ra_alloc_itemp();
    li_d(t, v);
    return t;
}

static void free_gpr_temp(IR2_OPND opnd, int guest_reg)
{
    if (guest_reg != 0 && mips_la_map[guest_reg] < 0) {
        /*
         * The temporary goes back to the shared itemp pool, so the guest
         * register must stop claiming it: otherwise the next allocation (an
         * imm_opnd() for a link address, say) writes into the same host register
         * and a later read of the guest register silently sees that value. That
         * is how `jalr $25` left the return address in $25 and the kernel ended
         * up jumping to 0x5ffb5495820b36ec.
         */
        latm_regcache_release_temp(guest_reg, opnd._reg_num);
        ra_free_itemp(opnd._reg_num);
    }
}

static void store_gpr_dst_f(int guest_reg, IR2_OPND v)
{
    store_gpr_dst(guest_reg, v);
    free_alloc_gpr(v);
}

static void store_gpr32_dst_f(int guest_reg, IR2_OPND v)
{
    store_gpr32_dst(guest_reg, v);
    free_alloc_gpr(v);
}

/* ------------------------------------------------------------------ */
/* TB control flow                                                     */

static inline void li_tb(IR2_OPND opnd, int64_t value)
{
    uint32_t hi32 = value >> 32;
    uint32_t lo32 = value & 0xffffffff;

    la_lu12i_w(opnd, lo32 >> 12);
    la_ori(opnd, opnd, lo32 & 0xfff);
    la_lu32i_d(opnd, hi32 & 0xfffff);
}

static void lata_gen_exit_tb_ret_0(DisasContext *s)
{
    IR2_OPND addr;
    int64_t curr_ins_pos = (uintptr_t)s->base->tb->tc.ptr +
                           (lsenv->tr_data->real_ir2_inst_num << 2);
    int64_t exit_offset = lsenv->tr_data->cs_native_to_bt_ret_0 - curr_ins_pos;

    ir2_opnd_build(&addr, IR2_OPND_IMM, exit_offset >> 2);
    la_b(addr);
}

static void gen_goto_tb(DisasContext *s, int n, uint64_t dest)
{
    IR2_OPND goto_label = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND addr;
    TranslationBlock *tb = lsenv->tr_data->curr_tb;

    la_label(goto_label);
    tb->jmp_reset_offset[n] = ir2_opnd_label_id(&goto_label);
    ir2_opnd_build(&addr, IR2_OPND_IMM, 1);
    la_b(addr); /* nop patched by label_dispose / TB chaining */

    IR2_OPND pcv = ra_alloc_itemp();
    li_d(pcv, dest);
    latm_env_st_d(pcv, env_offset_pc());
    ra_free_itemp(pcv._reg_num);
    li_tb(a0_ir2_opnd, (uint64_t)tb | n);

    int64_t curr_ins_pos = (uintptr_t)tb->tc.ptr +
                           (lsenv->tr_data->real_ir2_inst_num << 2);
    int64_t exit_offset = lsenv->tr_data->cs_native_to_bt - curr_ins_pos;
    ir2_opnd_build(&addr, IR2_OPND_IMM, exit_offset >> 2);
    la_b(addr);
}

static void gen_goto_tb_indirect(DisasContext *s, IR2_OPND target)
{
    IR2_OPND miss = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND host;

    latm_env_st_d(target, env_offset_pc());
    host = hcall1((uintptr_t)latm_jmp_cache_lookup, target);
    la_beqz(host, miss);
    if (latm_link_use_jiscr1) {
        la_gr2scr(scr0_ir2_opnd, host);
        la_jiscr1(0);
    } else if (latm_have_lbt) {
        la_gr2scr(scr0_ir2_opnd, host);
        la_jiscr0(0);
    } else {
        la_jirl(zero_ir2_opnd, host, 0);
    }
    la_label(miss);
    lata_gen_exit_tb_ret_0(s);
}

static void translate_delay_slot(DisasContext *s)
{
    DisasContext *d = s + 1;
    TRANSLATION_DATA *td = lsenv->tr_data;
    uint32_t start = (uint32_t)td->real_ir2_inst_num << 2;
    uint32_t end;
    LatmRecoveryBinding binds[LATM_RECOVERY_MAX_BINDS_PER_SPAN];
    /* P0-2: binds describe state entering the delay slot (its span opens at
     * the faultable access), so snapshot before it translates. */
    uint8_t nb = latm_regcache_snapshot_gpr_binds(binds);

    latm_set_delay_flag(s, true);
    if (mips_ir1_is_branch(&d->ir1)) {
        translate_functions[MIPS_OP_RI](d);
    } else {
        translate_one(d);
    }
    latm_set_delay_flag(s, false);
    end = (uint32_t)td->real_ir2_inst_num << 2;
    latm_tb_insn_map_add(td, d->ir1.pc, start, end, 1,
                         latm_recovery_site_of(d->ir1.op), 0, binds, nb);
}

/* ------------------------------------------------------------------ */
/* Simple translators                                                  */

static bool trans_nop(DisasContext *s)
{
    (void)s;
    return true;
}

static bool trans_ri(DisasContext *s)
{
    latm_set_pc(s);
    IR2_OPND e = imm_opnd(EXCP_RI);
    hcall1((uintptr_t)helper_raise_exception, e);
    ra_free_itemp(e._reg_num);
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_alu(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    IR2_OPND rs = alloc_gpr_src(s->ir1.rs);
    IR2_OPND rt = alloc_gpr_src(s->ir1.rt);
    IR2_OPND rd;
    IR2_OPND t, t2;

    switch (op) {
    case MIPS_ALU_ADD:
    case MIPS_ALU_SUB:
    case MIPS_ALU_DADD:
    case MIPS_ALU_DSUB: {
        latm_set_pc(s);
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        IR2_OPND b = rs;
        IR2_OPND c = rt;
        IR2_OPND r = hcall3((uintptr_t)latm_helper_arith, a, b, c);
        store_gpr32_dst_f(s->ir1.rd, r);
        break;
    }
    case MIPS_ALU_ADDU:
    case MIPS_ALU_SUBU:
        rd = alloc_gpr_dst(s->ir1.rd);
        if (op == MIPS_ALU_ADDU) {
            la_add_w(rd, rs, rt);
        } else {
            la_sub_w(rd, rs, rt);
        }
        store_gpr32_dst_f(s->ir1.rd, rd);
        break;
    case MIPS_ALU_DADDU:
        rd = alloc_gpr_dst(s->ir1.rd);
        la_add_d(rd, rs, rt);
        store_gpr_dst_f(s->ir1.rd, rd);
        break;
    case MIPS_ALU_DSUBU:
        rd = alloc_gpr_dst(s->ir1.rd);
        la_sub_d(rd, rs, rt);
        store_gpr_dst_f(s->ir1.rd, rd);
        break;
    case MIPS_ALU_AND:
    case MIPS_ALU_OR:
    case MIPS_ALU_XOR:
    case MIPS_ALU_NOR: {
        rd = alloc_gpr_dst(s->ir1.rd);
        switch (op) {
        case MIPS_ALU_AND: la_and(rd, rs, rt); break;
        case MIPS_ALU_OR:  la_or(rd, rs, rt); break;
        case MIPS_ALU_XOR: la_xor(rd, rs, rt); break;
        default:           la_nor(rd, rs, rt); break;
        }
        store_gpr_dst_f(s->ir1.rd, rd);
        break;
    }
    case MIPS_ALU_SLT:
    case MIPS_ALU_SLTU:
        rd = alloc_gpr_dst(s->ir1.rd);
        if (op == MIPS_ALU_SLT) {
            la_slt(rd, rs, rt);
        } else {
            la_sltu(rd, rs, rt);
        }
        store_gpr_dst_f(s->ir1.rd, rd);
        break;
    case MIPS_ALU_MUL:
        rd = alloc_gpr_dst(s->ir1.rd);
        la_mul_w(rd, rs, rt);
        store_gpr32_dst_f(s->ir1.rd, rd);
        break;
    case MIPS_ALU_MOVZ:
    case MIPS_ALU_MOVN: {
        IR2_OPND old = alloc_gpr_src(s->ir1.rd);
        t = ra_alloc_itemp();
        t2 = ra_alloc_itemp();
        rd = alloc_gpr_dst(s->ir1.rd);
        /* masknez keeps rj when rk==0; maskeqz keeps rj when rk!=0. */
        if (op == MIPS_ALU_MOVZ) {
            la_masknez(t, rs, rt);
            la_maskeqz(t2, old, rt);
        } else {
            la_maskeqz(t, rs, rt);
            la_masknez(t2, old, rt);
        }
        la_or(rd, t, t2);
        store_gpr_dst_f(s->ir1.rd, rd);
        ra_free_itemp(t._reg_num);
        ra_free_itemp(t2._reg_num);
        break;
    }
    default:
        return trans_ri(s);
    }

    free_alloc_gpr(rs);
    if (rt._reg_num != rs._reg_num) free_alloc_gpr(rt);
    return true;
}

static bool trans_alu_imm(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    int64_t imm = s->ir1.simm;
    IR2_OPND rs = alloc_gpr_src(s->ir1.rs);
    IR2_OPND rd;
    IR2_OPND t;

    switch (op) {
    case MIPS_IMM_ADDI:
    case MIPS_IMM_DADDI: {
        latm_set_pc(s);
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        IR2_OPND b = rs;
        IR2_OPND c = imm_opnd(imm);
        IR2_OPND r = hcall3((uintptr_t)latm_helper_arith, a, b, c);
        ra_free_itemp(c._reg_num);
        store_gpr32_dst_f(s->ir1.rt, r);
        break;
    }
    case MIPS_IMM_ADDIU:
        rd = alloc_gpr_dst(s->ir1.rt);
        if (imm >= -2048 && imm <= 2047) {
            la_addi_w(rd, rs, imm);
        } else {
            t = imm_opnd(imm);
            la_add_w(rd, rs, t);
            ra_free_itemp(t._reg_num);
        }
        store_gpr32_dst_f(s->ir1.rt, rd);
        break;
    case MIPS_IMM_DADDIU:
        rd = alloc_gpr_dst(s->ir1.rt);
        if (imm >= -2048 && imm <= 2047) {
            la_addi_d(rd, rs, imm);
        } else {
            t = imm_opnd(imm);
            la_add_d(rd, rs, t);
            ra_free_itemp(t._reg_num);
        }
        store_gpr_dst_f(s->ir1.rt, rd);
        break;
    case MIPS_IMM_SLTI:
    case MIPS_IMM_SLTIU:
        rd = alloc_gpr_dst(s->ir1.rt);
        if (imm >= -2048 && imm <= 2047) {
            if (op == MIPS_IMM_SLTI) la_slti(rd, rs, imm);
            else la_sltui(rd, rs, imm);
        } else {
            t = imm_opnd(imm);
            if (op == MIPS_IMM_SLTI) la_slt(rd, rs, t);
            else la_sltu(rd, rs, t);
            ra_free_itemp(t._reg_num);
        }
        store_gpr_dst_f(s->ir1.rt, rd);
        break;
    case MIPS_IMM_ANDI:
    case MIPS_IMM_ORI:
    case MIPS_IMM_XORI: {
        uint32_t uimm = s->ir1.insn & 0xffff;
        rd = alloc_gpr_dst(s->ir1.rt);
        if (uimm <= 0xfff) {
            if (op == MIPS_IMM_ANDI) la_andi(rd, rs, uimm);
            else if (op == MIPS_IMM_ORI) la_ori(rd, rs, uimm);
            else la_xori(rd, rs, uimm);
        } else {
            t = imm_opnd(uimm);
            if (op == MIPS_IMM_ANDI) la_and(rd, rs, t);
            else if (op == MIPS_IMM_ORI) la_or(rd, rs, t);
            else la_xor(rd, rs, t);
            ra_free_itemp(t._reg_num);
        }
        store_gpr_dst_f(s->ir1.rt, rd);
        break;
    }
    case MIPS_IMM_LUI:
        rd = alloc_gpr_dst(s->ir1.rt);
        la_lu12i_w(rd, (uint32_t)(s->ir1.insn & 0xffff) << 4);
        store_gpr_dst_f(s->ir1.rt, rd);
        break;
    default:
        return trans_ri(s);
    }

    free_alloc_gpr(rs);
    return true;
}

static bool trans_shift(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    unsigned sa = s->ir1.sa;
    IR2_OPND rt = alloc_gpr_src(s->ir1.rt);
    IR2_OPND rd = alloc_gpr_dst(s->ir1.rd);

    switch (op) {
    case MIPS_SH_SLL: la_slli_w(rd, rt, sa); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_SRL: la_srli_w(rd, rt, sa); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_SRA: la_srai_w(rd, rt, sa); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_DSLL: la_slli_d(rd, rt, sa); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_DSRL: la_srli_d(rd, rt, sa); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_DSRA: la_srai_d(rd, rt, sa); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_DSLL32: la_slli_d(rd, rt, sa + 32); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_DSRL32: la_srli_d(rd, rt, sa + 32); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SH_DSRA32: la_srai_d(rd, rt, sa + 32); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_DROTR: la_rotri_d(rd, rt, sa); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_DROTR32: la_rotri_d(rd, rt, sa + 32); store_gpr_dst_f(s->ir1.rd, rd); break;
    default: return trans_ri(s);
    }
    free_gpr_temp(rt, s->ir1.rt);
    return true;
}

static bool trans_shift_v(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    IR2_OPND rs = alloc_gpr_src(s->ir1.rs);
    IR2_OPND rt = alloc_gpr_src(s->ir1.rt);
    IR2_OPND rd = alloc_gpr_dst(s->ir1.rd);

    switch (op) {
    case MIPS_SHV_SLLV: la_sll_w(rd, rt, rs); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_SRLV: la_srl_w(rd, rt, rs); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_SRAV: la_sra_w(rd, rt, rs); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_DSLLV: la_sll_d(rd, rt, rs); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_DSRLV: la_srl_d(rd, rt, rs); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_DSRAV: la_sra_d(rd, rt, rs); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_ROTR: la_rotri_w(rd, rt, s->ir1.sa); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_ROTRV: la_rotr_w(rd, rt, rs); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_SHV_DROTRV: la_rotr_d(rd, rt, rs); store_gpr_dst_f(s->ir1.rd, rd); break;
    default: return trans_ri(s);
    }
    free_gpr_temp(rs, s->ir1.rs);
    free_gpr_temp(rt, s->ir1.rt);
    return true;
}

static bool trans_bitfield(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    unsigned pos = s->ir1.sa;
    unsigned rd_field = s->ir1.rd; /* size-1 for EXT/DEXT, msb for INS/DINS */
    IR2_OPND rd;

    switch (op) {
    case MIPS_BF_EXT:
    case MIPS_BF_DEXT:
    case MIPS_BF_DEXTM:
    case MIPS_BF_DEXTU: {
        /* EXT/DEXT read their source from RS and write RT; only the INS
         * family is a read-modify-write of RT. */
        unsigned p = pos + (op == MIPS_BF_DEXTU ? 32u : 0u);
        unsigned width = rd_field + 1u + (op == MIPS_BF_DEXTM ? 32u : 0u);
        IR2_OPND rs;
        if ((op == MIPS_BF_EXT && pos + width > 32) || p + width > 64) {
            return trans_ri(s);
        }
        rs = alloc_gpr_src(s->ir1.rs);
        rd = alloc_gpr_dst(s->ir1.rt);
        if (op == MIPS_BF_EXT && p == 0 && width == 32) {
            la_slli_w(rd, rs, 0);
        } else {
            la_bstrpick_d(rd, rs, p + width - 1, p);
        }
        store_gpr_dst_f(s->ir1.rt, rd);
        free_gpr_temp(rs, s->ir1.rs);
        break;
    }
    case MIPS_BF_INS: {
        unsigned msb = rd_field;
        IR2_OPND rs = alloc_gpr_src(s->ir1.rs);
        if (pos > msb) {
            free_gpr_temp(rs, s->ir1.rs);
            return trans_ri(s);
        }
        rd = alloc_gpr_src(s->ir1.rt);
        la_bstrins_d(rd, rs, msb, pos);
        la_slli_w(rd, rd, 0);
        store_gpr32_dst_f(s->ir1.rt, rd);
        free_gpr_temp(rs, s->ir1.rs);
        break;
    }
    case MIPS_BF_DINS:
    case MIPS_BF_DINSM:
    case MIPS_BF_DINSU: {
        unsigned p = pos + (op == MIPS_BF_DINSU ? 32u : 0u);
        unsigned msb = rd_field +
                       ((op == MIPS_BF_DINSM || op == MIPS_BF_DINSU) ? 32u : 0u);
        IR2_OPND rs = alloc_gpr_src(s->ir1.rs);
        if (p > msb) {
            free_gpr_temp(rs, s->ir1.rs);
            return trans_ri(s);
        }
        rd = alloc_gpr_src(s->ir1.rt);
        la_bstrins_d(rd, rs, msb, p);
        store_gpr_dst_f(s->ir1.rt, rd);
        free_gpr_temp(rs, s->ir1.rs);
        break;
    }
    default:
        return trans_ri(s);
    }
    return true;
}

static bool trans_bshfl(DisasContext *s)
{
    IR2_OPND rt = alloc_gpr_src(s->ir1.rt);
    IR2_OPND rd = alloc_gpr_dst(s->ir1.rd);

    switch (s->ir1.subop) {
    case MIPS_BS_WSBH: la_revb_2h(rd, rt); la_slli_w(rd, rd, 0); store_gpr32_dst_f(s->ir1.rd, rd); break;
    case MIPS_BS_SEB:  la_ext_w_b(rd, rt); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_BS_SEH:  la_ext_w_h(rd, rt); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_BS_DSBH: la_revb_2h(rd, rt); store_gpr_dst_f(s->ir1.rd, rd); break;
    case MIPS_BS_DSHD: la_revh_2w(rd, rt); store_gpr_dst_f(s->ir1.rd, rd); break;
    default: return trans_ri(s);
    }
    free_gpr_temp(rt, s->ir1.rt);
    return true;
}

static bool trans_muldiv(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    IR2_OPND rs = alloc_gpr_src(s->ir1.rs);
    IR2_OPND rt = alloc_gpr_src(s->ir1.rt);

    switch (op) {
    case MIPS_MD_MULT:
    case MIPS_MD_MULTU: {
        IR2_OPND lo = ra_alloc_itemp();
        IR2_OPND hi = ra_alloc_itemp();
        la_mul_w(lo, rs, rt);
        if (op == MIPS_MD_MULT) la_mulh_w(hi, rs, rt);
        else la_mulh_wu(hi, rs, rt);
        latm_env_st_d(lo, offsetof(CPUMIPSState, active_tc.LO[0]));
        latm_env_st_d(hi, offsetof(CPUMIPSState, active_tc.HI[0]));
        ra_free_itemp(lo._reg_num);
        ra_free_itemp(hi._reg_num);
        break;
    }
    case MIPS_MD_DMULT:
    case MIPS_MD_DMULTU: {
        IR2_OPND lo = ra_alloc_itemp();
        IR2_OPND hi = ra_alloc_itemp();
        la_mul_d(lo, rs, rt);
        if (op == MIPS_MD_DMULT) la_mulh_d(hi, rs, rt);
        else la_mulh_du(hi, rs, rt);
        latm_env_st_d(lo, offsetof(CPUMIPSState, active_tc.LO[0]));
        latm_env_st_d(hi, offsetof(CPUMIPSState, active_tc.HI[0]));
        ra_free_itemp(lo._reg_num);
        ra_free_itemp(hi._reg_num);
        break;
    }
    case MIPS_MD_DIV:
    case MIPS_MD_DIVU:
    case MIPS_MD_DDIV:
    case MIPS_MD_DDIVU: {
        latm_set_pc(s);
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        IR2_OPND b = rs, c = rt;
        hcall3((uintptr_t)latm_helper_div, a, b, c);
        break;
    }
    case MIPS_MD_MADD:
    case MIPS_MD_MADDU:
    case MIPS_MD_MSUB:
    case MIPS_MD_MSUBU: {
        latm_set_pc(s);
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        IR2_OPND b = rs, c = rt;
        hcall3((uintptr_t)latm_helper_madd, a, b, c);
        break;
    }
    case MIPS_MD_CLZ:
    case MIPS_MD_CLO:
    case MIPS_MD_DCLZ:
    case MIPS_MD_DCLO: {
        IR2_OPND rd = alloc_gpr_dst(s->ir1.rd);
        switch (op) {
        case MIPS_MD_CLZ:  la_clz_w(rd, rs); break;
        case MIPS_MD_CLO:  la_clo_w(rd, rs); break;
        case MIPS_MD_DCLZ: la_clz_d(rd, rs); break;
        default:           la_clo_d(rd, rs); break;
        }
        store_gpr_dst_f(s->ir1.rd, rd);
        break;
    }
    default:
        return trans_ri(s);
    }
    free_gpr_temp(rs, s->ir1.rs);
    free_gpr_temp(rt, s->ir1.rt);
    return true;
}

static bool trans_hilo(DisasContext *s)
{
    IR2_OPND t;
    int off;

    switch (s->ir1.subop) {
    case MIPS_HILO_MFHI:
        if (s->ir1.rd == 0) return true;
        off = offsetof(CPUMIPSState, active_tc.HI[0]);
        t = alloc_gpr_dst(s->ir1.rd);
        latm_env_ld_d(t, off);
        store_gpr_dst_f(s->ir1.rd, t);
        break;
    case MIPS_HILO_MFLO:
        if (s->ir1.rd == 0) return true;
        off = offsetof(CPUMIPSState, active_tc.LO[0]);
        t = alloc_gpr_dst(s->ir1.rd);
        latm_env_ld_d(t, off);
        store_gpr_dst_f(s->ir1.rd, t);
        break;
    case MIPS_HILO_MTHI:
        off = offsetof(CPUMIPSState, active_tc.HI[0]);
        if (s->ir1.rs == 0) {
            latm_env_st_d(zero_ir2_opnd, off);
        } else {
            IR2_OPND v = alloc_gpr_src(s->ir1.rs);
            latm_env_st_d(v, off);
            free_gpr_temp(v, s->ir1.rs);
        }
        break;
    case MIPS_HILO_MTLO:
        off = offsetof(CPUMIPSState, active_tc.LO[0]);
        if (s->ir1.rs == 0) {
            latm_env_st_d(zero_ir2_opnd, off);
        } else {
            IR2_OPND v = alloc_gpr_src(s->ir1.rs);
            latm_env_st_d(v, off);
            free_gpr_temp(v, s->ir1.rs);
        }
        break;
    }
    return true;
}

static void emit_addr(IR2_OPND addr, int base, int64_t imm)
{
    IR2_OPND b = alloc_gpr_src(base);
    if (imm == 0) {
        la_mov64(addr, b);
    } else if (imm >= -2048 && imm <= 2047) {
        la_addi_d(addr, b, imm);
    } else {
        IR2_OPND t = imm_opnd(imm);
        la_add_d(addr, b, t);
        ra_free_itemp(t._reg_num);
    }
    if (mips_la_map[base] < 0 && base != 0) free_alloc_gpr(b);
}

static bool trans_load(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    IR2_OPND addr = ra_alloc_itemp();
    IR2_OPND rd;

    latm_set_pc(s);
    emit_addr(addr, s->ir1.rs, s->ir1.simm);

    if (latm_system_memory && op <= MIPS_MEM_LD) {
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        IR2_OPND r = hcall2((uintptr_t)latm_helper_load, a, addr);
        /* latm_helper_load() sign-extends LB/LH/LW itself (it casts through
         * int64_t); the returned value is already the final GPR content, so it
         * is stored verbatim and the itemp ownership goes with it. */
        store_gpr_dst_f(s->ir1.rt, r);
        ra_free_itemp(addr._reg_num);
        return true;
    }
    if (latm_system_memory &&
        (op == MIPS_MEM_LWL || op == MIPS_MEM_LWR ||
         op == MIPS_MEM_LDL || op == MIPS_MEM_LDR)) {
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        IR2_OPND old = alloc_gpr_src(s->ir1.rt);
        IR2_OPND r = hcall3((uintptr_t)latm_helper_unaligned_load,
                            a, addr, old);
        free_gpr_temp(old, s->ir1.rt);
        if (op == MIPS_MEM_LWL || op == MIPS_MEM_LWR)
            store_gpr32_dst_f(s->ir1.rt, r);
        else
            store_gpr_dst_f(s->ir1.rt, r);
        ra_free_itemp(addr._reg_num);
        return true;
    }

    switch (op) {
    /* LB/LH/LW sign-extend into the 64-bit GPR; LB/LBU/LH/LHU/LW/LWU/LD all go
     * through the helper so there is exactly one place that owns MIPS load
     * semantics (a bare (int32_t)v cast to uint64_t zero-extends again, which
     * is how LW silently behaved like LWU). */
    /* Direct (non-system) loads read the caller's identity-mapped memory.
     * Sign extension is written out per width instead of being delegated to a
     * helper, because this path has no platform/MMU behind it. */
    case MIPS_MEM_LB:
    case MIPS_MEM_LH:
    case MIPS_MEM_LW: {
        /* LoongArch word-sized results are sign-extended into the 64-bit
         * destination, so ext.w.h / ext.w.b widen the narrow loads and
         * `srai.w rd,rd,0` widens the word one. */
        IR2_OPND t = ra_alloc_itemp();
        /* ld.w / ld.h / ld.b already sign-extend into the 64-bit destination;
         * the .wu/.hu/.bu variants are the zero-extending ones. */
        if (op == MIPS_MEM_LB) {
            la_ldx_b(t, addr, zero_ir2_opnd);
        } else if (op == MIPS_MEM_LH) {
            la_ldx_h(t, addr, zero_ir2_opnd);
        } else {
            la_ldx_w(t, addr, zero_ir2_opnd);
        }
        store_gpr_dst_f(s->ir1.rt, t);
        break;
    }
    case MIPS_MEM_LBU: rd = alloc_gpr_dst(s->ir1.rt); la_ldx_bu(rd, addr, zero_ir2_opnd); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LHU: rd = alloc_gpr_dst(s->ir1.rt); la_ldx_hu(rd, addr, zero_ir2_opnd); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LWU: rd = alloc_gpr_dst(s->ir1.rt); la_ldx_wu(rd, addr, zero_ir2_opnd); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LD:  rd = alloc_gpr_dst(s->ir1.rt); la_ldx_d(rd, addr, zero_ir2_opnd); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LWL: rd = alloc_gpr_src(s->ir1.rt); la_ldl_w(rd, addr, 0); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LWR: rd = alloc_gpr_src(s->ir1.rt); la_ldr_w(rd, addr, 0); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LDL: rd = alloc_gpr_src(s->ir1.rt); la_ldl_d(rd, addr, 0); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LDR: rd = alloc_gpr_src(s->ir1.rt); la_ldr_d(rd, addr, 0); store_gpr_dst_f(s->ir1.rt, rd); break;
    case MIPS_MEM_LL:
    case MIPS_MEM_LLD:
        /* Reservation semantics are handled by a C helper in system mode. */
        latm_set_pc(s);
        {
            IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
            IR2_OPND b = addr;
            IR2_OPND r = hcall2((uintptr_t)latm_helper_ll, a, b);
                store_gpr_dst_f(s->ir1.rt, r);
        }
        break;
    default:
        ra_free_itemp(addr._reg_num);
        return trans_ri(s);
    }
    ra_free_itemp(addr._reg_num);
    return true;
}

static bool trans_store(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    IR2_OPND addr = ra_alloc_itemp();
    IR2_OPND rt = alloc_gpr_src(s->ir1.rt);

    latm_set_pc(s);
    emit_addr(addr, s->ir1.rs, s->ir1.simm);

    if (latm_system_memory && op >= MIPS_MEM_SB && op <= MIPS_MEM_SD) {
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        hcall3((uintptr_t)latm_helper_store, a, addr, rt);
        ra_free_itemp(addr._reg_num);
        free_gpr_temp(rt, s->ir1.rt);
        return true;
    }
    if (latm_system_memory &&
        (op == MIPS_MEM_SWL || op == MIPS_MEM_SWR ||
         op == MIPS_MEM_SDL || op == MIPS_MEM_SDR)) {
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
        hcall3((uintptr_t)latm_helper_unaligned_store, a, addr, rt);
        ra_free_itemp(addr._reg_num);
        free_gpr_temp(rt, s->ir1.rt);
        return true;
    }

    switch (op) {
    case MIPS_MEM_SB:  la_stx_b(rt, addr, zero_ir2_opnd); break;
    case MIPS_MEM_SH:  la_stx_h(rt, addr, zero_ir2_opnd); break;
    case MIPS_MEM_SW:  la_stx_w(rt, addr, zero_ir2_opnd); break;
    case MIPS_MEM_SD:  la_stx_d(rt, addr, zero_ir2_opnd); break;
    case MIPS_MEM_SWL: la_stl_w(rt, addr, 0); break;
    case MIPS_MEM_SWR: la_str_w(rt, addr, 0); break;
    case MIPS_MEM_SDL: la_stl_d(rt, addr, 0); break;
    case MIPS_MEM_SDR: la_str_d(rt, addr, 0); break;
    case MIPS_MEM_SC:
    case MIPS_MEM_SCD:
        latm_set_pc(s);
        {
            IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, op);
            IR2_OPND b = addr;
            IR2_OPND c = rt;
            IR2_OPND r = hcall3((uintptr_t)latm_helper_sc, a, b, c);
                store_gpr_dst_f(s->ir1.rt, r);
        }
        break;
    default:
        ra_free_itemp(addr._reg_num);
        free_gpr_temp(rt, s->ir1.rt);
        return trans_ri(s);
    }
    ra_free_itemp(addr._reg_num);
    free_gpr_temp(rt, s->ir1.rt);
    return true;
}

/* ------------------------------------------------------------------ */
/* Branches, jumps, FPU, CP0                                           */

static bool trans_branch(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    bool likely = s->ir1.likely;
    bool link = s->ir1.link;
    IR2_OPND cond = ra_alloc_itemp();
    IR2_OPND t = ra_alloc_itemp();
    uint64_t target = s->ir1.pc + 4 + ((int64_t)s->ir1.simm << 2);
    uint64_t fall = s->ir1.pc + 8;

    switch (op) {
    case MIPS_BR_BEQ:
    case MIPS_BR_BNE:
    case MIPS_BR_BEQL:
    case MIPS_BR_BNEL: {
        IR2_OPND a = alloc_gpr_src(s->ir1.rs);
        IR2_OPND b = alloc_gpr_src(s->ir1.rt);
        la_xor(t, a, b);
        if (op == MIPS_BR_BEQ || op == MIPS_BR_BEQL) {
            la_sltui(cond, t, 1);
        } else {
            la_sltu(cond, zero_ir2_opnd, t);
        }
        free_gpr_temp(a, s->ir1.rs);
        free_gpr_temp(b, s->ir1.rt);
        break;
    }
    case MIPS_BR_BLEZ:
    case MIPS_BR_BLEZL:
    case MIPS_BR_BGTZ:
    case MIPS_BR_BGTZL:
    case MIPS_BR_BLTZ:
    case MIPS_BR_BLTZL:
    case MIPS_BR_BGEZ:
    case MIPS_BR_BGEZL:
    case MIPS_BR_BLTZAL:
    case MIPS_BR_BGEZAL:
    case MIPS_BR_BLTZALL:
    case MIPS_BR_BGEZALL: {
        IR2_OPND a = alloc_gpr_src(s->ir1.rs);
        switch (op) {
        case MIPS_BR_BLTZ: case MIPS_BR_BLTZL:
        case MIPS_BR_BLTZAL: case MIPS_BR_BLTZALL:
            la_slt(cond, a, zero_ir2_opnd);
            break;
        case MIPS_BR_BGEZ: case MIPS_BR_BGEZL:
        case MIPS_BR_BGEZAL: case MIPS_BR_BGEZALL:
            la_slt(cond, a, zero_ir2_opnd);
            la_xori(cond, cond, 1);
            break;
        case MIPS_BR_BGTZ: case MIPS_BR_BGTZL:
            la_slt(cond, zero_ir2_opnd, a);
            break;
        default: /* BLEZ/BLEZL */
            la_slt(cond, zero_ir2_opnd, a);
            la_xori(cond, cond, 1);
            break;
        }
        free_gpr_temp(a, s->ir1.rs);
        break;
    }
    default:
        return trans_ri(s);
    }
    ra_free_itemp(t._reg_num);

    if (link) {
        IR2_OPND l = imm_opnd(s->ir1.pc + 8);
        store_gpr_dst_f(31, l);
    }

    /* A delay-slot helper may clobber caller-saved itemps. */
    if (likely) {
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);
        la_beqz(cond, skip);
        latm_spill_put(0, cond);
        translate_delay_slot(s);
        cond = latm_spill_get(0);
        gen_goto_tb(s, 0, target);
        la_label(skip);
        gen_goto_tb(s, 1, fall);
    } else {
        IR2_OPND taken = ir2_opnd_new_type(IR2_OPND_LABEL);
        latm_spill_put(0, cond);
        translate_delay_slot(s);
        cond = latm_spill_get(0);
        la_bnez(cond, taken);
        gen_goto_tb(s, 1, fall);
        la_label(taken);
        gen_goto_tb(s, 0, target);
    }
    ra_free_itemp(cond._reg_num);
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_jump(DisasContext *s)
{
    unsigned op = s->ir1.subop;

    if (op == MIPS_JP_J || op == MIPS_JP_JAL) {
        uint64_t target = ((s->ir1.pc + 4) & UINT64_C(0xfffffffff0000000)) |
                          ((uint64_t)s->ir1.target << 2);
        if (op == MIPS_JP_JAL) {
            IR2_OPND l = imm_opnd(s->ir1.pc + 8);
            store_gpr_dst_f(31, l);
        }
        translate_delay_slot(s);
        gen_goto_tb(s, 0, target);
        s->base->is_jmp = DISAS_NORETURN;
        return true;
    }

    if (op == MIPS_JP_JR || op == MIPS_JP_JALR) {
        IR2_OPND target = ra_alloc_itemp();
        IR2_OPND src = alloc_gpr_src(s->ir1.rs);
        la_mov64(target, src);
        free_gpr_temp(src, s->ir1.rs);
        if (op == MIPS_JP_JALR && s->ir1.rd != 0) {
            IR2_OPND l = imm_opnd(s->ir1.pc + 8);
            store_gpr_dst_f(s->ir1.rd, l);
        }
        latm_spill_put(1, target);
        translate_delay_slot(s);
        target = latm_spill_get(1);
        gen_goto_tb_indirect(s, target);
        ra_free_itemp(target._reg_num);
        s->base->is_jmp = DISAS_NORETURN;
        return true;
    }
    return trans_ri(s);
}

static bool trans_fpu_bc(DisasContext *s)
{
    unsigned cc = (s->ir1.cond >> 2) & 7;
    unsigned tf = s->ir1.cond & 1;
    bool likely = s->ir1.likely;
    unsigned fcc_bit = latm_fcc_bit(cc);
    IR2_OPND cond = ra_alloc_itemp();
    IR2_OPND t = ra_alloc_itemp();
    uint64_t target = s->ir1.pc + 4 + ((int64_t)s->ir1.simm << 2);
    uint64_t fall = s->ir1.pc + 8;

    latm_env_ld_w(t, offsetof(CPUMIPSState, active_fpu.fcr31));
    la_bstrpick_d(cond, t, fcc_bit, fcc_bit);
    if (tf == 0) {
        la_xori(cond, cond, 1);
    }
    ra_free_itemp(t._reg_num);

    if (likely) {
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);
        la_beqz(cond, skip);
        latm_spill_put(2, cond);
        translate_delay_slot(s);
        cond = latm_spill_get(2);
        gen_goto_tb(s, 0, target);
        la_label(skip);
        gen_goto_tb(s, 1, fall);
    } else {
        IR2_OPND taken = ir2_opnd_new_type(IR2_OPND_LABEL);
        latm_spill_put(2, cond);
        translate_delay_slot(s);
        cond = latm_spill_get(2);
        la_bnez(cond, taken);
        gen_goto_tb(s, 1, fall);
        la_label(taken);
        gen_goto_tb(s, 0, target);
    }
    ra_free_itemp(cond._reg_num);
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_fpu_arith(DisasContext *s)
{
    IR2_OPND args[5] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.fmt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rd),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt),
    };
    latm_set_pc(s);
    hcall((uintptr_t)latm_fpu_arith, 5, args);
    return true;
}

static bool trans_fpu_cvt(DisasContext *s)
{
    IR2_OPND args[4] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.fmt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rd),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
    };
    latm_set_pc(s);
    hcall((uintptr_t)latm_fpu_cvt, 4, args);
    return true;
}

static bool trans_fpu_cmp(DisasContext *s)
{
    IR2_OPND args[5] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.fmt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.cond),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt),
        ir2_opnd_new(IR2_OPND_IMM, (s->ir1.rd >> 2) & 7),
    };
    latm_set_pc(s);
    hcall((uintptr_t)latm_fpu_cmp, 5, args);
    return true;
}

static bool trans_fpu_movc(DisasContext *s)
{
    IR2_OPND args[5] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.fmt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rd),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.cond),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.sa),
    };
    hcall((uintptr_t)latm_fpu_movc, 5, args);
    return true;
}

static bool trans_fpu_move(DisasContext *s)
{
    IR2_OPND args[5] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.fmt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rd),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt),
    };
    hcall((uintptr_t)latm_fpu_move, 5, args);
    return true;
}

static bool trans_fpu_madd(DisasContext *s)
{
    IR2_OPND args[6] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.fmt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rd),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.sa),
    };
    latm_set_pc(s);
    hcall((uintptr_t)latm_fpu_madd, 6, args);
    return true;
}

static bool trans_fpu_gpr(DisasContext *s)
{
    IR2_OPND args[3] = {
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt),
        ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs),
    };
    hcall((uintptr_t)latm_fpu_gpr, 3, args);
    return true;
}

static bool trans_fpu_ldst(DisasContext *s)
{
    unsigned op = s->ir1.subop;
    unsigned ft = s->ir1.rt;
    unsigned fs = 0;
    IR2_OPND addr = ra_alloc_itemp();
    int helper_op;

    if (op <= MIPS_FLDST_SDC1) {
        helper_op = op;
        emit_addr(addr, s->ir1.rs, s->ir1.simm);
        if (op == MIPS_FLDST_SWC1 || op == MIPS_FLDST_SDC1) {
            fs = s->ir1.rt;
        }
    } else {
        IR2_OPND base = alloc_gpr_src(s->ir1.rs);
        IR2_OPND index = alloc_gpr_src(s->ir1.rt);
        la_add_d(addr, base, index);
        free_gpr_temp(base, s->ir1.rs);
        free_gpr_temp(index, s->ir1.rt);
        ft = s->ir1.rd;
        fs = s->ir1.sa;
        switch (s->ir1.sel) {
        case 0: helper_op = 4; break;  /* LWXC1 */
        case 1: helper_op = 5; break;  /* LDXC1 */
        case 5: helper_op = 6; break;  /* LUXC1 */
        case 8: helper_op = 7; break;  /* SWXC1 */
        case 9: helper_op = 8; break;  /* SDXC1 */
        case 13: helper_op = 9; break; /* SUXC1 */
        default: ra_free_itemp(addr._reg_num); return trans_ri(s);
        }
    }

    latm_set_pc(s);
    IR2_OPND args[4] = {
        ir2_opnd_new(IR2_OPND_IMM, helper_op),
        ir2_opnd_new(IR2_OPND_IMM, ft),
        ir2_opnd_new(IR2_OPND_IMM, fs),
        addr,
    };
    hcall((uintptr_t)latm_fpu_ldst, 4, args);
    ra_free_itemp(addr._reg_num);
    return true;
}

static bool trans_cp0_move(DisasContext *s)
{
    unsigned subop = s->ir1.subop;
    unsigned rt = s->ir1.rt;
    unsigned reg = s->ir1.rd;
    unsigned sel = s->ir1.sel;

    switch (subop) {
    case MIPS_CP0_MFC0:
    case MIPS_CP0_DMFC0:
    case MIPS_CP0_MFHC0: {
        latm_set_pc(s);
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, reg);
        IR2_OPND b = ir2_opnd_new(IR2_OPND_IMM, sel);
        IR2_OPND r = hcall2((uintptr_t)latm_helper_cp0_read, a, b);
        if (subop == MIPS_CP0_DMFC0) {
            store_gpr_dst_f(rt, r);
        } else {
            if (subop == MIPS_CP0_MFHC0) {
                la_srai_d(r, r, 32);
            }
            store_gpr32_dst_f(rt, r);
        }
        break;
    }
    case MIPS_CP0_MTC0:
    case MIPS_CP0_DMTC0: {
        latm_set_pc(s);
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, reg);
        IR2_OPND b = ir2_opnd_new(IR2_OPND_IMM, sel);
        IR2_OPND c = alloc_gpr_src(rt);
        hcall3((uintptr_t)latm_helper_cp0_write, a, b, c);
        free_gpr_temp(c, rt);
        break;
    }
    case MIPS_CP0_RDPGPR: {
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, reg);
        IR2_OPND r = hcall1((uintptr_t)latm_helper_shadow_read, a);
        store_gpr_dst_f(rt, r);
        break;
    }
    case MIPS_CP0_WRPGPR: {
        IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, reg);
        IR2_OPND c = alloc_gpr_src(rt);
        hcall2((uintptr_t)latm_helper_shadow_write, a, c);
        free_gpr_temp(c, rt);
        break;
    }
    default:
        return trans_ri(s);
    }
    return true;
}

static bool trans_cp0_op(DisasContext *s)
{
    switch (s->ir1.subop) {
    case MIPS_CP0OP_ERET:
    case MIPS_CP0OP_DERET:
        latm_set_pc(s);
        hcall0((uintptr_t)latm_helper_eret);
        lata_gen_exit_tb_ret_0(s);
        s->base->is_jmp = DISAS_NORETURN;
        break;
    case MIPS_CP0OP_WAIT:
        latm_set_pc(s);
        hcall0((uintptr_t)latm_helper_wait);
        lata_gen_exit_tb_ret_0(s);
        s->base->is_jmp = DISAS_NORETURN;
        break;
    case MIPS_CP0OP_DI:
    case MIPS_CP0OP_EI: {
        IR2_OPND r;
        uintptr_t fn = s->ir1.subop == MIPS_CP0OP_DI ?
                       (uintptr_t)latm_helper_di : (uintptr_t)latm_helper_ei;
        latm_set_pc(s);
        r = hcall0(fn);
        store_gpr_dst_f(s->ir1.rt, r);
        lata_gen_exit_tb_ret_0(s);
        s->base->is_jmp = DISAS_NORETURN;
        break;
    }
    default:
        return trans_ri(s);
    }
    return true;
}

static bool trans_tlb(DisasContext *s)
{
    IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop);
    latm_set_pc(s);
    hcall1((uintptr_t)latm_helper_tlb, a);
    return true;
}

static bool trans_sync(DisasContext *s)
{
    (void)s;
    la_dbar(0);
    return true;
}

static bool trans_synci(DisasContext *s)
{
    IR2_OPND addr = ra_alloc_itemp();
    emit_addr(addr, s->ir1.rs, s->ir1.simm);
    latm_set_pc(s);
    hcall1((uintptr_t)latm_helper_synci, addr);
    ra_free_itemp(addr._reg_num);
    gen_goto_tb(s, 0, s->ir1.pc + 4);
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_cache(DisasContext *s)
{
    IR2_OPND addr = ra_alloc_itemp();
    IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt);
    emit_addr(addr, s->ir1.rs, s->ir1.simm);
    hcall2((uintptr_t)latm_helper_cache, a, addr);
    ra_free_itemp(addr._reg_num);
    return true;
}

static bool trans_pref(DisasContext *s)
{
    IR2_OPND addr = ra_alloc_itemp();
    IR2_OPND hint = ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt);
    if (s->ir1.sel == 0 && s->ir1.op == MIPS_OP_PREF &&
        s->ir1.insn >> 26 == 0x33) {
        emit_addr(addr, s->ir1.rs, s->ir1.simm);
    } else {
        IR2_OPND base = alloc_gpr_src(s->ir1.rs);
        IR2_OPND index = alloc_gpr_src(s->ir1.rt);
        la_add_d(addr, base, index);
        free_gpr_temp(base, s->ir1.rs);
        free_gpr_temp(index, s->ir1.rt);
        hint = ir2_opnd_new(IR2_OPND_IMM, 0);
    }
    hcall2((uintptr_t)latm_helper_pref, hint, addr);
    ra_free_itemp(addr._reg_num);
    return true;
}

static bool trans_rdhwr(DisasContext *s)
{
    IR2_OPND a = ir2_opnd_new(IR2_OPND_IMM, s->ir1.rd);
    IR2_OPND b = ir2_opnd_new(IR2_OPND_IMM, s->ir1.sel);
    IR2_OPND r;
    latm_set_pc(s);
    r = hcall2((uintptr_t)latm_helper_rdhwr, a, b);
    store_gpr_dst_f(s->ir1.rt, r);
    if (s->ir1.rd == 2) {
        lata_gen_exit_tb_ret_0(s);
        s->base->is_jmp = DISAS_NORETURN;
    }
    return true;
}

static bool trans_trap(DisasContext *s)
{
    IR2_OPND opv = ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop);
    IR2_OPND a, b;
    IR2_OPND code = ir2_opnd_new(IR2_OPND_IMM, (s->ir1.insn >> 6) & 0x3ff);

    if (s->ir1.insn >> 26 == 0x01) { /* REGIMM immediate trap */
        a = alloc_gpr_src(s->ir1.rs);
        b = imm_opnd(s->ir1.simm);
    } else {
        a = alloc_gpr_src(s->ir1.rs);
        b = alloc_gpr_src(s->ir1.rt);
    }
    latm_set_pc(s);
    hcall4((uintptr_t)latm_helper_trap, opv, a, b, code);
    if (s->ir1.insn >> 26 == 0x01) {
        free_alloc_gpr(b);
    } else {
        free_gpr_temp(b, s->ir1.rt);
    }
    free_gpr_temp(a, s->ir1.rs);
    return true;
}

static bool trans_syscall(DisasContext *s)
{
    latm_set_pc(s);
    hcall0((uintptr_t)latm_helper_syscall);
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_break(DisasContext *s)
{
    IR2_OPND code = ir2_opnd_new(IR2_OPND_IMM, (s->ir1.insn >> 6) & 0xfffff);
    latm_set_pc(s);
    hcall1((uintptr_t)latm_helper_break, code);
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_loongson(DisasContext *s)
{
    IR2_OPND op = ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop);
    IR2_OPND a = alloc_gpr_src(s->ir1.rs);
    IR2_OPND b = alloc_gpr_src(s->ir1.rt);
    latm_set_pc(s);
    IR2_OPND r = hcall3((uintptr_t)latm_helper_loongson_arith, op, a, b);
    store_gpr_dst_f(s->ir1.rd, r);
    free_gpr_temp(a, s->ir1.rs);
    free_gpr_temp(b, s->ir1.rt);
    return true;
}

static bool trans_loongson_mem(DisasContext *s)
{
    IR2_OPND op = ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop);
    IR2_OPND base = alloc_gpr_src(s->ir1.rs);
    uint32_t index_reg = (s->ir1.subop == MIPS_LM_GSLQ ||
                          s->ir1.subop == MIPS_LM_GSSQ) ? 0 : s->ir1.rd;
    IR2_OPND index = index_reg ? alloc_gpr_src(index_reg) : zero_ir2_opnd;
    IR2_OPND off = imm_opnd(s->ir1.simm);
    IR2_OPND rt = ir2_opnd_new(IR2_OPND_IMM, s->ir1.rt);
    IR2_OPND rt2 = ir2_opnd_new(IR2_OPND_IMM, s->ir1.aux);
    latm_set_pc(s);
    hcall6((uintptr_t)latm_helper_loongson_mem, op, base, index, off, rt, rt2);
    if (index_reg && mips_la_map[index_reg] < 0) {
        free_alloc_gpr(index);
    }
    free_gpr_temp(base, s->ir1.rs);
    ra_free_itemp(off._reg_num);
    return true;
}

static bool trans_iocsr(DisasContext *s)
{
    IR2_OPND addr;
    IR2_OPND val;
    IR2_OPND size;
    IR2_OPND r;

    latm_set_pc(s);
    switch (s->ir1.subop) {
    case MIPS_IOCSR_CPUCFG:
        r = hcall2((uintptr_t)latm_helper_cpucfg,
                   ir2_opnd_new(IR2_OPND_IMM, s->ir1.rs), zero_ir2_opnd);
        store_gpr_dst_f(s->ir1.rd, r);
        break;
    case MIPS_IOCSR_RDCSR:
    case MIPS_IOCSR_DRDCSR:
        addr = alloc_gpr_src(s->ir1.rs);
        size = ir2_opnd_new(IR2_OPND_IMM,
                            s->ir1.subop == MIPS_IOCSR_DRDCSR ? 8 : 4);
        r = hcall3((uintptr_t)latm_helper_iocsr_read, addr, size, zero_ir2_opnd);
        store_gpr_dst_f(s->ir1.rd, r);
        free_gpr_temp(addr, s->ir1.rs);
        break;
    case MIPS_IOCSR_WRCSR:
    case MIPS_IOCSR_DWRCSR:
        addr = alloc_gpr_src(s->ir1.rs);
        val = alloc_gpr_src(s->ir1.rd);
        size = ir2_opnd_new(IR2_OPND_IMM,
                            s->ir1.subop == MIPS_IOCSR_DWRCSR ? 8 : 4);
        hcall4((uintptr_t)latm_helper_iocsr_write, addr, val, size, zero_ir2_opnd);
        free_gpr_temp(addr, s->ir1.rs);
        free_gpr_temp(val, s->ir1.rd);
        break;
    default:
        return false;
    }
    return true;
}

static bool trans_crc(DisasContext *s)
{
    IR2_OPND op = ir2_opnd_new(IR2_OPND_IMM, s->ir1.subop);
    IR2_OPND a = alloc_gpr_src(s->ir1.rs);
    IR2_OPND b = alloc_gpr_src(s->ir1.rd);
    latm_set_pc(s);
    IR2_OPND r = hcall3((uintptr_t)latm_helper_crc, op, a, b);
    store_gpr_dst_f(s->ir1.rt, r);
    free_gpr_temp(a, s->ir1.rs);
    free_gpr_temp(b, s->ir1.rd);
    return true;
}

/* ------------------------------------------------------------------ */
/* IR1 table and TB loop                                               */

TranslateFn translate_functions[MIPS_OP__COUNT] = {
    [MIPS_OP_NOP]       = trans_nop,
    [MIPS_OP_RI]        = trans_ri,
    [MIPS_OP_ALU]       = trans_alu,
    [MIPS_OP_ALU_IMM]   = trans_alu_imm,
    [MIPS_OP_SHIFT]     = trans_shift,
    [MIPS_OP_SHIFT_V]   = trans_shift_v,
    [MIPS_OP_BITFIELD]  = trans_bitfield,
    [MIPS_OP_BSHFL]     = trans_bshfl,
    [MIPS_OP_MULDIV]    = trans_muldiv,
    [MIPS_OP_HILO]      = trans_hilo,
    [MIPS_OP_LOAD]      = trans_load,
    [MIPS_OP_STORE]     = trans_store,
    [MIPS_OP_BRANCH]    = trans_branch,
    [MIPS_OP_JUMP]      = trans_jump,
    [MIPS_OP_TRAP]      = trans_trap,
    [MIPS_OP_SYSCALL]   = trans_syscall,
    [MIPS_OP_BREAK]     = trans_break,
    [MIPS_OP_SYNC]      = trans_sync,
    [MIPS_OP_SYNCI]     = trans_synci,
    [MIPS_OP_CACHE]     = trans_cache,
    [MIPS_OP_PREF]      = trans_pref,
    [MIPS_OP_EHB]       = trans_nop,
    [MIPS_OP_PAUSE]     = trans_nop,
    [MIPS_OP_RDHWR]     = trans_rdhwr,
    [MIPS_OP_CP0_MOVE]  = trans_cp0_move,
    [MIPS_OP_CP0_OP]    = trans_cp0_op,
    [MIPS_OP_TLB]       = trans_tlb,
    [MIPS_OP_FPU_ARITH] = trans_fpu_arith,
    [MIPS_OP_FPU_CMP]   = trans_fpu_cmp,
    [MIPS_OP_FPU_CVT]   = trans_fpu_cvt,
    [MIPS_OP_FPU_MOVE]  = trans_fpu_move,
    [MIPS_OP_FPU_MADD]  = trans_fpu_madd,
    [MIPS_OP_FPU_MOVC]  = trans_fpu_movc,
    [MIPS_OP_FPU_BC]    = trans_fpu_bc,
    [MIPS_OP_FPU_LDST]  = trans_fpu_ldst,
    [MIPS_OP_FPU_GPR]   = trans_fpu_gpr,
    [MIPS_OP_LOONGSON]  = trans_loongson,
    [MIPS_OP_LOONGSON_MEM] = trans_loongson_mem,
    [MIPS_OP_IOCSR]     = trans_iocsr,
    [MIPS_OP_CRC]       = trans_crc,
};

static void translate_one(DisasContext *s)
{
    TranslateFn fn = translate_functions[s->ir1.op];
    if (fn == NULL) {
        trans_ri(s);
    } else {
        fn(s);
    }
}

__attribute__((weak)) uint32_t latm_guest_ldl(uint64_t addr)
{
    CPUMIPSState *env = lsenv ? lsenv->cpu_state : NULL;
    if (env) {
        return cpu_ldl_code(env, addr);
    }
    return *(const uint32_t *)(uintptr_t)addr;
}

static DisasContext *get_ir1_list(TranslationBlock *tb, int max_insns)
{
    DisasContext *ir1_list = lsenv->tr_data->ir1_list;
    DisasContextBase *db = lsenv->tr_data->db_rel;
    int n = 0;
    uint64_t pc = tb->pc;
    uint64_t page_start = pc & TARGET_PAGE_MASK;

    if (lsenv && lsenv->cpu_state) {
        ((CPUMIPSState *)lsenv->cpu_state)->latm_fetch_fault = 0;
    }
    db->tb = tb;
    db->pc_first = pc;
    db->pc_next = pc;
    db->is_jmp = DISAS_NEXT;
    db->num_insns = 0;
    /* The page check below still stops ordinary straight-line fetch at the
     * page boundary.  A branch at the last word must be allowed to decode its
     * delay slot from the next page; the fetch helper applies that page's
     * permissions and faults. */
    db->max_insns = max_insns;

    while (true) {
        uint32_t insn;
        if (n >= db->max_insns) {
            db->is_jmp = DISAS_TOO_MANY;
            break;
        }

        insn = latm_guest_ldl(db->pc_next);
        if (lsenv && lsenv->cpu_state &&
            ((CPUMIPSState *)lsenv->cpu_state)->latm_fetch_fault) {
            db->is_jmp = DISAS_NORETURN;
            db->num_insns = 0;
            tb->size = 0;
            tb->icount = 0;
            return ir1_list;
        }
        ir1_list[n].base = db;
        mips_decode_one(&ir1_list[n].ir1, insn, db->pc_next);

        if (mips_ir1_is_branch(&ir1_list[n].ir1)) {
            if (n + 2 > db->max_insns) {
                db->is_jmp = DISAS_TOO_MANY;
                break;
            }
            db->pc_next += 4;
            insn = latm_guest_ldl(db->pc_next);
            if (lsenv && lsenv->cpu_state &&
                ((CPUMIPSState *)lsenv->cpu_state)->latm_fetch_fault) {
                db->is_jmp = DISAS_NORETURN;
                db->num_insns = 0;
                tb->size = 0;
                tb->icount = 0;
                return ir1_list;
            }
            ir1_list[n + 1].base = db;
            mips_decode_one(&ir1_list[n + 1].ir1, insn, db->pc_next);
            ir1_list[n + 1].ir1.delay_slot = true;
            db->pc_next += 4;
            n += 2;
            db->is_jmp = DISAS_NORETURN;
            break;
        }

        db->pc_next += 4;
        n++;
        if ((db->pc_next - page_start) >= TARGET_PAGE_SIZE) {
            db->is_jmp = DISAS_TOO_MANY;
            break;
        }
    }

    db->num_insns = n;
    tb->size = db->pc_next - db->pc_first;
    tb->icount = n;
    return ir1_list;
}

void target_disasm(TranslationBlock *tb, int *max_insns, void *cpu)
{
    latm_translate_lock();
    if (cpu) {
        latm_bind_env(cpu);
    }
    if (lsenv && lsenv->tr_data) {
        CPUMIPSState *env = cpu;
        lsenv->tr_data->lock_held = true;
        lsenv->tr_data->cache_key = env ? latm_cache_key(env) : 0;
    }
    tb->ir1 = get_ir1_list(tb, *max_insns);
}

bool tr_ir2_generate(TranslationBlock *tb)
{
    int ir1_nr = tb->icount;
    DisasContext *pir1 = (DisasContext *)tb->ir1;

    tr_init(tb);
    latm_tb_insn_map_begin(lsenv->tr_data, tb->tc.ptr, 0);
    if (ir1_nr <= 0) {
        tr_fini();
        return false;
    }
    for (int i = 0; i < ir1_nr; i++) {
        if (pir1->ir1.delay_slot) {
            pir1++;
            continue;
        }
        if (translate_functions[pir1->ir1.op] == NULL) {
            tr_fini();
            return false;
        }
        {
            TRANSLATION_DATA *td = lsenv->tr_data;
            LatmRecoveryBinding binds[LATM_RECOVERY_MAX_BINDS_PER_SPAN];
            uint32_t start = (uint32_t)td->real_ir2_inst_num << 2;
            /* P0-2: snapshot bindings BEFORE the insn translates so a host
             * fault inside its span replays only state live at the access,
             * never this insn's own (post-fault) results. */
            uint8_t site = latm_recovery_site_of(pir1->ir1.op);
            uint8_t nb = latm_regcache_snapshot_gpr_binds(binds);
            translate_one(pir1);
            latm_tb_insn_map_add(td, pir1->ir1.pc, start,
                                 (uint32_t)td->real_ir2_inst_num << 2, 0,
                                 site, i, binds, nb);
        }
        if (tcg_ctx->gen_insn_data) {
            tcg_ctx->gen_insn_end_off[i] =
                (lsenv->tr_data->real_ir2_inst_num) << 2;
            tcg_ctx->gen_insn_data[i * TARGET_INSN_START_WORDS] =
                pir1->ir1.pc;
        }
        pir1++;
    }

    pir1--;
    switch (pir1->base->is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        gen_goto_tb(pir1, 1, pir1->base->pc_next);
        break;
    case DISAS_UPDATE_EXIT:
    case DISAS_EXIT:
        lata_gen_exit_tb_ret_0(pir1);
        break;
    case DISAS_NORETURN:
        break;
    }
    /* The IR2 list for this block is complete here (tail terminator included);
     * dump it before label_dispose() rewrites labels. */
    {
        TRANSLATION_DATA *td = lsenv->tr_data;
        if (td->ir2_dump_wanted) {
            td->ir2_dump_wanted = false;
            latm_ir2_dump_hook(td);
        }
    }
    return true;
}
