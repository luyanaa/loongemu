#include "latr.h"
#include "translate.h"
#include "mmu.h"

typedef IR2_INST *(*LatrBinaryOp)(IR2_OPND, IR2_OPND, IR2_OPND);
typedef IR2_INST *(*LatrAmoOp)(IR2_OPND, IR2_OPND, IR2_OPND);

/* Keep large CPUArchState fields off signed 12-bit load/store immediates. */

void latr_env_load_d(IR2_OPND dst, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_ld_d(dst, env_ir2_opnd, offset);
        return;
    }
    li_d(r21_ir2_opnd, offset);
    la_add_d(r21_ir2_opnd, env_ir2_opnd, r21_ir2_opnd);
    la_ld_d(dst, r21_ir2_opnd, 0);
}

void latr_env_store_d(IR2_OPND value, int offset)
{
    if (offset >= -2048 && offset <= 2047) {
        la_st_d(value, env_ir2_opnd, offset);
        return;
    }
    li_d(r21_ir2_opnd, offset);
    la_add_d(r21_ir2_opnd, env_ir2_opnd, r21_ir2_opnd);
    la_st_d(value, r21_ir2_opnd, 0);
}
typedef struct LatrStoreLock {
    IR2_OPND address;
    IR2_OPND epoch;
    IR2_OPND status;
} LatrStoreLock;

/* Every guest write conservatively invalidates all outstanding reservations.
 * The word itself lives in mmu.c so C memory paths share the same domain. */

static LatrStoreLock emit_store_lock_acquire(void)
{
    latr_regcache_flush();
    LatrStoreLock lock = {
        .address = ra_alloc_itemp(),
        .epoch = ra_alloc_itemp(),
        .status = ra_alloc_itemp(),
    };
    IR2_OPND retry = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_emit_code_symbol(lock.address, (const void *)&latr_guest_store_lock);
    la_label(retry);
    la_ll_d(lock.epoch, lock.address, 0);
    la_andi(lock.status, lock.epoch, 1);
    la_bne(lock.status, zero_ir2_opnd, retry);
    la_addi_d(lock.status, lock.epoch, 1);
    la_sc_d(lock.status, lock.address, 0);
    la_beq(lock.status, zero_ir2_opnd, retry);
    la_dbar(0);
    return lock;
}

static void emit_store_lock_release(LatrStoreLock lock)
{
    la_addi_d(lock.status, lock.epoch, 2);
    la_dbar(0);
    la_st_d(lock.status, lock.address, 0);
    la_dbar(0);
    ra_free_temp(lock.status);
    ra_free_temp(lock.epoch);
    ra_free_temp(lock.address);
}

static void emit_store_lock_release_sc(LatrStoreLock lock, IR2_OPND sc_result)
{
    IR2_OPND success = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

    la_beq(sc_result, zero_ir2_opnd, success);
    la_mov64(lock.status, lock.epoch);
    la_b(done);
    la_label(success);
    la_addi_d(lock.status, lock.epoch, 2);
    la_label(done);
    la_dbar(0);
    la_st_d(lock.status, lock.address, 0);
    la_dbar(0);
    ra_free_temp(lock.status);
    ra_free_temp(lock.epoch);
    ra_free_temp(lock.address);
}

void latr_emit_store(IR2_OPND address, IR2_OPND value, unsigned size)
{
    LatrStoreLock lock = emit_store_lock_acquire();
    switch (size) {
    case 1: la_st_b(value, address, 0); break;
    case 2: la_st_h(value, address, 0); break;
    case 4: la_st_w(value, address, 0); break;
    case 8: la_st_d(value, address, 0); break;
    default: lsassertm(0, "invalid LATR store size");
    }
    emit_store_lock_release(lock);
}

static void emit_binary(const RiscvIR1 *ir1, LatrBinaryOp op)
{
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    op(dst, lhs, rhs);
    store_gpr_dst(ir1->arg.rd, dst);
    free_alloc_gpr(dst);
    free_alloc_gpr(rhs);
    free_alloc_gpr(lhs);
}

static unsigned latr_mem_size(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_LB: case LATR_RV_LBU: case LATR_RV_SB: return 1;
    case LATR_RV_LH: case LATR_RV_LHU: case LATR_RV_SH: return 2;
    case LATR_RV_LW: case LATR_RV_LWU: case LATR_RV_SW: return 4;
    default: return 8;
    }
}

static void emit_load(const RiscvIR1 *ir1)
{
    IR2_OPND base = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND address = ra_alloc_itemp();
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND direct = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND after = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND slow = ir2_opnd_new_type(IR2_OPND_LABEL);
    unsigned size = latr_mem_size(ir1->opcode);

    li_d(address, ir1->arg.imm);
    la_add_d(address, base, address);
    free_alloc_gpr(base);
    if (size > 1) {
        IR2_OPND offset = ra_alloc_itemp();
        IR2_OPND limit = ra_alloc_itemp();
        la_andi(offset, address, 0xfff);
        li_d(limit, (int64_t)(0x1000 - size));
        la_bltu(limit, offset, slow);
        ra_free_temp(limit);
        ra_free_temp(offset);
    }
    latr_emit_resolve(address, LATR_ACCESS_LOAD, size);
    latr_emit_branch_on_fault(fault);

    {
        IR2_OPND mmio = ra_alloc_itemp();
        latr_env_load_d(mmio, env_offset_mem_mmio());
        la_beqz(mmio, direct);
        ra_free_temp(mmio);
    }
    {
        /* a0 carries the guest physical address; a1 receives it. */
        IR2_OPND target = ra_alloc_itemp();
        la_mov64(a1_ir2_opnd, a0_ir2_opnd);
        li_d(a2_ir2_opnd, size);
        la_mov64(a0_ir2_opnd, env_ir2_opnd);
        latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
        latr_emit_code_symbol(target, (const void *)&latr_mmio_load);
        la_jirl(ra_ir2_opnd, target, 0);
        ra_free_temp(target);
        latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
    }
    latr_emit_branch_on_fault(fault);
    {
        /* The helper returns the raw device value; apply the same width
         * semantics as a direct load. */
        IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
        switch (ir1->opcode) {
        case LATR_RV_LB:  la_ext_w_b(dst, a0_ir2_opnd); break;
        case LATR_RV_LH:  la_ext_w_h(dst, a0_ir2_opnd); break;
        case LATR_RV_LW:  la_mov32_sx(dst, a0_ir2_opnd); break;
        case LATR_RV_LBU: la_bstrpick_d(dst, a0_ir2_opnd, 7, 0); break;
        case LATR_RV_LHU: la_bstrpick_d(dst, a0_ir2_opnd, 15, 0); break;
        case LATR_RV_LWU: la_bstrpick_d(dst, a0_ir2_opnd, 31, 0); break;
        case LATR_RV_LD:  la_mov64(dst, a0_ir2_opnd); break;
        default: lsassertm(0, "invalid LATR MMIO load opcode");
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
    }
    la_b(done);

    la_label(direct);
    {
        IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
        switch (ir1->opcode) {
        case LATR_RV_LB:  la_ld_b(dst, a0_ir2_opnd, 0); break;
        case LATR_RV_LH:  la_ld_h(dst, a0_ir2_opnd, 0); break;
        case LATR_RV_LW:  la_ld_w(dst, a0_ir2_opnd, 0); break;
        case LATR_RV_LBU: la_ld_bu(dst, a0_ir2_opnd, 0); break;
        case LATR_RV_LHU: la_ld_hu(dst, a0_ir2_opnd, 0); break;
        case LATR_RV_LWU: la_ld_wu(dst, a0_ir2_opnd, 0); break;
        case LATR_RV_LD:  la_ld_d(dst, a0_ir2_opnd, 0); break;
        default: lsassertm(0, "invalid LATR load opcode");
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
    }
    la_label(done);
    la_b(after);

    la_label(slow);
    {
        IR2_OPND target = ra_alloc_itemp();
        IR2_OPND dst;
        la_mov64(a1_ir2_opnd, address);
        li_d(a2_ir2_opnd, size);
        la_mov64(a0_ir2_opnd, env_ir2_opnd);
        latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
        latr_emit_code_symbol(target, (const void *)&latr_cross_load);
        la_jirl(ra_ir2_opnd, target, 0);
        ra_free_temp(target);
        latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
        latr_emit_branch_on_fault(fault);
        dst = alloc_gpr_dst(ir1->arg.rd);
        switch (ir1->opcode) {
        case LATR_RV_LB:  la_ext_w_b(dst, a0_ir2_opnd); break;
        case LATR_RV_LH:  la_ext_w_h(dst, a0_ir2_opnd); break;
        case LATR_RV_LW:  la_mov32_sx(dst, a0_ir2_opnd); break;
        case LATR_RV_LBU: la_bstrpick_d(dst, a0_ir2_opnd, 7, 0); break;
        case LATR_RV_LHU: la_bstrpick_d(dst, a0_ir2_opnd, 15, 0); break;
        case LATR_RV_LWU: la_bstrpick_d(dst, a0_ir2_opnd, 31, 0); break;
        case LATR_RV_LD:  la_mov64(dst, a0_ir2_opnd); break;
        default: lsassertm(0, "invalid cross-page load opcode");
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
    }
    free_alloc_gpr(address);
    la_b(after);

    la_label(fault);
    latr_raise_mem_fault(ir1);
    la_label(after);
}

static void emit_store(const RiscvIR1 *ir1)
{
    IR2_OPND base = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND value = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND address = ra_alloc_itemp();
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND direct = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND after = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND slow = ir2_opnd_new_type(IR2_OPND_LABEL);
    unsigned size = latr_mem_size(ir1->opcode);

    li_d(address, ir1->arg.imm);
    la_add_d(address, base, address);
    free_alloc_gpr(base);
    /* The value does not survive the resolver/MMIO helper calls. */
    latr_env_store_d(value, env_offset_mem_value());
    free_alloc_gpr(value);
    if (size > 1) {
        IR2_OPND offset = ra_alloc_itemp();
        IR2_OPND limit = ra_alloc_itemp();
        la_andi(offset, address, 0xfff);
        li_d(limit, (int64_t)(0x1000 - size));
        la_bltu(limit, offset, slow);
        ra_free_temp(limit);
        ra_free_temp(offset);
    }
    latr_emit_resolve(address, LATR_ACCESS_STORE, size);
    latr_emit_branch_on_fault(fault);

    {
        IR2_OPND mmio = ra_alloc_itemp();
        latr_env_load_d(mmio, env_offset_mem_mmio());
        la_beqz(mmio, direct);
        ra_free_temp(mmio);
    }
    {
        IR2_OPND stored = ra_alloc_itemp();
        IR2_OPND target = ra_alloc_itemp();

        latr_env_load_d(stored, env_offset_mem_value());
        la_mov64(a1_ir2_opnd, a0_ir2_opnd); /* guest physical address */
        li_d(a2_ir2_opnd, size);
        la_mov64(a3_ir2_opnd, stored);
        ra_free_temp(stored);
        la_mov64(a0_ir2_opnd, env_ir2_opnd);
        latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
        latr_emit_code_symbol(target, (const void *)&latr_mmio_store);
        la_jirl(ra_ir2_opnd, target, 0);
        ra_free_temp(target);
        latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
    }
    latr_emit_branch_on_fault(fault);
    la_b(done);

    la_label(direct);
    {
        IR2_OPND stored = ra_alloc_itemp();
        latr_env_load_d(stored, env_offset_mem_value());
        latr_emit_store(a0_ir2_opnd, stored, size);
        ra_free_temp(stored);
    }
    la_label(done);
    la_b(after);

    la_label(slow);
    {
        IR2_OPND target = ra_alloc_itemp();
        IR2_OPND stored = ra_alloc_itemp();
        latr_env_load_d(stored, env_offset_mem_value());
        la_mov64(a1_ir2_opnd, address);
        li_d(a2_ir2_opnd, size);
        la_mov64(a3_ir2_opnd, stored);
        ra_free_temp(stored);
        la_mov64(a0_ir2_opnd, env_ir2_opnd);
        latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
        latr_emit_code_symbol(target, (const void *)&latr_cross_store);
        la_jirl(ra_ir2_opnd, target, 0);
        ra_free_temp(target);
        latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
        latr_emit_branch_on_fault(fault);
    }
    free_alloc_gpr(address);
    la_b(after);

    la_label(fault);
    latr_raise_mem_fault(ir1);
    la_label(after);
}

static void emit_immediate_logic(const RiscvIR1 *ir1, LatrBinaryOp op)
{
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND imm = ra_alloc_itemp();
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    li_d(imm, ir1->arg.imm);
    op(dst, lhs, imm);
    store_gpr_dst(ir1->arg.rd, dst);
    free_alloc_gpr(dst);
    ra_free_temp(imm);
    free_alloc_gpr(lhs);
}

static void emit_multiply_high_unsigned_signed(const RiscvIR1 *ir1)
{
    /* mulhsu(a, b) = mulh(a, b) + (b < 0 ? a : 0), where mulh treats b
     * as signed. */
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND negative = ra_alloc_itemp();
    IR2_OPND correction = ra_alloc_itemp();
    la_mulh_d(dst, lhs, rhs);
    la_slt(negative, rhs, zero_ir2_opnd);
    la_sub_d(negative, zero_ir2_opnd, negative);
    la_and(correction, negative, lhs);
    la_add_d(dst, dst, correction);
    store_gpr_dst(ir1->arg.rd, dst);
    ra_free_temp(correction);
    ra_free_temp(negative);
    free_alloc_gpr(dst);
    free_alloc_gpr(rhs);
    free_alloc_gpr(lhs);
}

static void emit_division(const RiscvIR1 *ir1)
{
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND zero_label = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done_label = ir2_opnd_new_type(IR2_OPND_LABEL);
    bool is_word = ir1->opcode == LATR_RV_DIVW || ir1->opcode == LATR_RV_DIVUW ||
                   ir1->opcode == LATR_RV_REMW || ir1->opcode == LATR_RV_REMUW;
    bool is_unsigned = ir1->opcode == LATR_RV_DIVU || ir1->opcode == LATR_RV_REMU ||
                       ir1->opcode == LATR_RV_DIVUW || ir1->opcode == LATR_RV_REMUW;
    bool is_remainder = ir1->opcode == LATR_RV_REM || ir1->opcode == LATR_RV_REMU ||
                        ir1->opcode == LATR_RV_REMW || ir1->opcode == LATR_RV_REMUW;
    IR2_OPND zero_check = rhs;
    if (is_word) {
        zero_check = ra_alloc_itemp();
        la_mov32_zx(zero_check, rhs);
    }

    la_beq(zero_check, zero_ir2_opnd, zero_label);
    if (is_remainder) {
        if (is_word) {
            (is_unsigned ? la_mod_wu : la_mod_w)(dst, lhs, rhs);
        } else {
            (is_unsigned ? la_mod_du : la_mod_d)(dst, lhs, rhs);
        }
    } else if (is_word) {
        (is_unsigned ? la_div_wu : la_div_w)(dst, lhs, rhs);
    } else {
        (is_unsigned ? la_div_du : la_div_d)(dst, lhs, rhs);
    }
    la_b(done_label);
    la_label(zero_label);
    if (is_remainder) {
        if (is_word) {
            la_mov32_sx(dst, lhs);
        } else {
            la_mov64(dst, lhs);
        }
    } else {
        li_d(dst, -1);
    }
    la_label(done_label);
    store_gpr_dst(ir1->arg.rd, dst);
    free_alloc_gpr(dst);
    free_alloc_gpr(rhs);
    free_alloc_gpr(lhs);
    if (is_word) {
        ra_free_temp(zero_check);
    }
}

/* LR/SC/AMO must be naturally aligned; the check must precede any access
 * so that a misaligned atomic traps instead of reaching the host LL/SC. */
static void emit_aligned_or_trap(const RiscvIR1 *ir1, IR2_OPND address,
                                 unsigned size, bool is_store)
{
    IR2_OPND scratch = ra_alloc_itemp();
    IR2_OPND ok = ir2_opnd_new_type(IR2_OPND_LABEL);

    li_d(scratch, (int64_t)(size - 1));
    la_and(scratch, address, scratch);
    la_beqz(scratch, ok);
    latr_raise_misaligned(ir1, address, is_store);
    la_label(ok);
    ra_free_temp(scratch);
}

/* Atomics to MMIO are not supported: they trap as store/AMO access faults. */
static void emit_mmio_atomic_guard(const RiscvIR1 *ir1)
{
    IR2_OPND mmio = ra_alloc_itemp();
    IR2_OPND ok = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(mmio, env_offset_mem_mmio());
    la_beqz(mmio, ok);
    li_d(mmio, LATR_EXCP_STORE_ACCESS_FAULT);
    latr_env_store_d(mmio, env_offset_mem_fault_cause());
    latr_raise_mem_fault(ir1);
    la_label(ok);
    ra_free_temp(mmio);
}

static void emit_amo(const RiscvIR1 *ir1, LatrAmoOp op, bool word)
{
    latr_regcache_flush();
    IR2_OPND address = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND source = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    unsigned size = word ? 4 : 8;

    emit_aligned_or_trap(ir1, address, size, true);
    latr_env_store_d(source, env_offset_mem_value());
    free_alloc_gpr(source);
    latr_emit_resolve(address, LATR_ACCESS_STORE, size);
    free_alloc_gpr(address);
    latr_emit_branch_on_fault(fault);
    emit_mmio_atomic_guard(ir1);

    IR2_OPND result = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND value = ra_alloc_itemp();
    LatrStoreLock lock = emit_store_lock_acquire();

    latr_env_load_d(value, env_offset_mem_value());
    if (ir1->arg.rl) {
        la_dbar(0);
    }
    op(result, value, a0_ir2_opnd);
    if (ir1->arg.aq) {
        la_dbar(0);
    }
    if (word) {
        la_mov32_sx(result, result);
    }
    emit_store_lock_release(lock);
    store_gpr_dst(ir1->arg.rd, result);
    free_alloc_gpr(result);
    ra_free_temp(value);
    la_b(done);
    la_label(fault);
    latr_raise_mem_fault(ir1);
    la_label(done);
}

/*
 * Address translation is delegated to latr_resolve_guest_address(); the
 * reservation records the *physical* address so it survives a satp change,
 * and every page fault traps with mtval = guest VA.
 */

static void emit_lr(const RiscvIR1 *ir1, bool word)
{
    latr_regcache_flush();
    IR2_OPND address = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND inval = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND set_done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND tail = ir2_opnd_new_type(IR2_OPND_LABEL);
    unsigned size = word ? 4 : 8;

    emit_aligned_or_trap(ir1, address, size, false);
    latr_emit_resolve(address, LATR_ACCESS_LOAD, size);
    free_alloc_gpr(address);
    latr_emit_branch_on_fault(fault);
    emit_mmio_atomic_guard(ir1);

    IR2_OPND result = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND lock_address = ra_alloc_itemp();
    IR2_OPND before = ra_alloc_itemp();
    IR2_OPND after = ra_alloc_itemp();
    IR2_OPND scratch = ra_alloc_itemp();

    la_dbar(0);
    latr_emit_code_symbol(lock_address, (const void *)&latr_guest_store_lock);
    la_ld_d(before, lock_address, 0);
    if (word) {
        la_ld_w(result, a0_ir2_opnd, 0);
        la_mov32_sx(result, result);
    } else {
        la_ld_d(result, a0_ir2_opnd, 0);
    }
    la_dbar(0);
    la_ld_d(after, lock_address, 0);
    la_andi(scratch, before, 1);
    la_bne(scratch, zero_ir2_opnd, inval);
    la_bne(before, after, inval);

    latr_env_load_d(scratch, env_offset_mem_phys());
    latr_env_store_d(scratch, env_offset_reservation_addr());
    li_d(scratch, size);
    latr_env_store_d(scratch, env_offset_reservation_size());
    latr_env_store_d(before, env_offset_reservation_epoch());
    li_d(scratch, 1);
    latr_env_store_d(scratch, env_offset_reservation_valid());
    la_b(set_done);

    la_label(inval);
    li_d(scratch, 0);
    latr_env_store_d(scratch, env_offset_reservation_valid());
    la_label(set_done);
    if (ir1->arg.aq) {
        la_dbar(0);
    }
    store_gpr_dst(ir1->arg.rd, result);
    free_alloc_gpr(result);
    ra_free_temp(scratch);
    ra_free_temp(after);
    ra_free_temp(before);
    ra_free_temp(lock_address);
    la_b(tail);
    la_label(fault);
    latr_raise_mem_fault(ir1);
    la_label(tail);
}

static void emit_sc(const RiscvIR1 *ir1, bool word)
{
    latr_regcache_flush();
    IR2_OPND address = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND value = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND failed = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND sc_done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND tail = ir2_opnd_new_type(IR2_OPND_LABEL);
    unsigned size = word ? 4 : 8;

    emit_aligned_or_trap(ir1, address, size, true);
    latr_env_store_d(value, env_offset_mem_value());
    free_alloc_gpr(value);
    latr_emit_resolve(address, LATR_ACCESS_STORE, size);
    free_alloc_gpr(address);
    latr_emit_branch_on_fault(fault);
    emit_mmio_atomic_guard(ir1);

    /* Five live temporaries at most: result, scratch and the three lock
     * registers. The comparison reuses result/scratch so the six-register
     * itemp pool never overflows. */
    IR2_OPND result = ir1->arg.rd == 0
                          ? ra_alloc_itemp()
                          : alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND scratch = ra_alloc_itemp();
    LatrStoreLock lock = emit_store_lock_acquire();

    if (ir1->arg.rl) {
        la_dbar(0);
    }
    latr_env_load_d(scratch, env_offset_reservation_valid());
    la_beq(scratch, zero_ir2_opnd, failed);
    latr_env_load_d(result, env_offset_reservation_addr());
    latr_env_load_d(scratch, env_offset_mem_phys());
    la_bne(result, scratch, failed);
    latr_env_load_d(scratch, env_offset_reservation_size());
    la_addi_d(result, scratch, -(int)size);
    la_bne(result, zero_ir2_opnd, failed);
    latr_env_load_d(scratch, env_offset_reservation_epoch());
    la_bne(scratch, lock.epoch, failed);

    li_d(scratch, 0);
    latr_env_store_d(scratch, env_offset_reservation_valid());
    latr_env_load_d(result, env_offset_mem_value());
    if (word) {
        la_ll_w(scratch, a0_ir2_opnd, 0);
        la_sc_w(result, a0_ir2_opnd, 0);
    } else {
        la_ll_d(scratch, a0_ir2_opnd, 0);
        la_sc_d(result, a0_ir2_opnd, 0);
    }
    /* LoongArch SC reports 1 on success; RISC-V SC reports 0. */
    la_xori(result, result, 1);
    la_b(sc_done);

    la_label(failed);
    li_d(result, 1);
    li_d(scratch, 0);
    latr_env_store_d(scratch, env_offset_reservation_valid());
    la_label(sc_done);
    if (ir1->arg.aq) {
        la_dbar(0);
    }
    store_gpr_dst(ir1->arg.rd, result);
    emit_store_lock_release_sc(lock, result);
    ra_free_temp(scratch);
    if (ir1->arg.rd == 0) {
        ra_free_temp(result);
    } else {
        free_alloc_gpr(result);
    }
    la_b(tail);
    la_label(fault);
    latr_raise_mem_fault(ir1);
    la_label(tail);
}

static void emit_branch(const LatrTranslateContext *ctx)
{
    const RiscvIR1 *ir1 = ctx->ir1;
    lsassertm(ctx->has_target_label, "missing LATR direct branch target");
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_gpr_src(ir1->arg.rs2);
    switch (ir1->opcode) {
    case LATR_RV_BEQ:  la_beq(lhs, rhs, ctx->target_label); break;
    case LATR_RV_BNE:  la_bne(lhs, rhs, ctx->target_label); break;
    case LATR_RV_BLT:  la_blt(lhs, rhs, ctx->target_label); break;
    case LATR_RV_BGE:  la_bge(lhs, rhs, ctx->target_label); break;
    case LATR_RV_BLTU: la_bltu(lhs, rhs, ctx->target_label); break;
    case LATR_RV_BGEU: la_bgeu(lhs, rhs, ctx->target_label); break;
    default: lsassertm(0, "invalid LATR branch opcode");
    }
    free_alloc_gpr(rhs);
    free_alloc_gpr(lhs);
}

static void emit_jump(const LatrTranslateContext *ctx)
{
    const RiscvIR1 *ir1 = ctx->ir1;
    lsassertm(ctx->has_target_label, "missing LATR JAL target");
    IR2_OPND link = alloc_gpr_dst(ir1->arg.rd);
    li_d(link, (int64_t)(ir1->pc + ir1->raw_len));
    store_gpr_dst(ir1->arg.rd, link);
    free_alloc_gpr(link);
    la_b(ctx->target_label);
}

static void emit_jump_register(const RiscvIR1 *ir1)
{
    IR2_OPND base = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND target = ra_alloc_itemp();
    IR2_OPND link = alloc_gpr_dst(ir1->arg.rd);
    li_d(target, ir1->arg.imm);
    la_add_d(target, base, target);
    la_bstrins_d(target, zero_ir2_opnd, 0, 0);
    li_d(link, (int64_t)(ir1->pc + ir1->raw_len));
    store_gpr_dst(ir1->arg.rd, link);
    la_st_d(target, env_ir2_opnd, env_offset_pc());
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    free_alloc_gpr(link);
    ra_free_temp(target);
    free_alloc_gpr(base);
}

static void emit_mcycle(void)
{
    IR2_OPND inhibit = a0_ir2_opnd;
    IR2_OPND count = a1_ir2_opnd;
    IR2_OPND one = a2_ir2_opnd;
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(inhibit, env_offset_csr(0x320));
    la_andi(inhibit, inhibit, 1);
    la_bne(inhibit, zero_ir2_opnd, done);
    latr_env_load_d(count, env_offset_csr(0xb00));
    li_d(one, 1);
    la_add_d(count, count, one);
    latr_env_store_d(count, env_offset_csr(0xb00));
    la_label(done);
}

static void emit_popcount(const RiscvIR1 *ir1, bool word)
{
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND mask = ra_alloc_itemp();
    IR2_OPND shifted = a5_ir2_opnd;

    if (word) {
        la_mov32_zx(dst, lhs);
    } else {
        la_mov64(dst, lhs);
    }
    li_d(mask, 0x5555555555555555);
    la_srli_d(shifted, dst, 1);
    la_and(shifted, shifted, mask);
    la_sub_d(dst, dst, shifted);
    li_d(mask, 0x3333333333333333);
    la_and(shifted, dst, mask);
    la_srli_d(dst, dst, 2);
    la_and(dst, dst, mask);
    la_add_d(dst, dst, shifted);
    li_d(mask, 0x0f0f0f0f0f0f0f0f);
    la_srli_d(shifted, dst, 4);
    la_add_d(dst, dst, shifted);
    la_and(dst, dst, mask);
    la_srli_d(shifted, dst, 8);
    la_add_d(dst, dst, shifted);
    la_srli_d(shifted, dst, 16);
    la_add_d(dst, dst, shifted);
    la_srli_d(shifted, dst, 32);
    la_add_d(dst, dst, shifted);
    la_andi(dst, dst, 0x7f);
    if (word) {
        la_mov32_sx(dst, dst);
    }
    store_gpr_dst(ir1->arg.rd, dst);
    ra_free_temp(mask);
    free_alloc_gpr(dst);
    free_alloc_gpr(lhs);
}

static void emit_minmax(const RiscvIR1 *ir1, bool maximum, bool is_unsigned)
{
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND rhs = alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

    la_mov64(dst, lhs);
    if (maximum) {
        (is_unsigned ? la_bgeu : la_bge)(lhs, rhs, done);
    } else {
        (is_unsigned ? la_bltu : la_blt)(lhs, rhs, done);
    }
    la_mov64(dst, rhs);
    la_label(done);
    store_gpr_dst(ir1->arg.rd, dst);
    free_alloc_gpr(dst);
    free_alloc_gpr(rhs);
    free_alloc_gpr(lhs);
}

static void emit_zba(const RiscvIR1 *ir1)
{
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    bool immediate = ir1->opcode == LATR_RV_SLLI_UW;
    IR2_OPND rhs = immediate ? zero_ir2_opnd : alloc_gpr_src(ir1->arg.rs2);
    unsigned shift = 0;

    switch (ir1->opcode) {
    case LATR_RV_SH1ADD: case LATR_RV_SH1ADD_UW: shift = 1; break;
    case LATR_RV_SH2ADD: case LATR_RV_SH2ADD_UW: shift = 2; break;
    case LATR_RV_SH3ADD: case LATR_RV_SH3ADD_UW: shift = 3; break;
    case LATR_RV_SLLI_UW: shift = (unsigned)ir1->arg.shamt; break;
    default: break;
    }

    if (ir1->opcode == LATR_RV_ADD_UW ||
        ir1->opcode == LATR_RV_SH1ADD_UW ||
        ir1->opcode == LATR_RV_SH2ADD_UW ||
        ir1->opcode == LATR_RV_SH3ADD_UW ||
        ir1->opcode == LATR_RV_SLLI_UW) {
        la_bstrpick_d(dst, lhs, 31, 0);
    } else {
        la_mov64(dst, lhs);
    }
    if (ir1->opcode == LATR_RV_SLLI_UW) {
        la_slli_d(dst, dst, (int)shift);
    } else if (ir1->opcode == LATR_RV_ADD_UW) {
        la_add_d(dst, dst, rhs);
    } else {
        la_slli_d(dst, dst, (int)shift);
        la_add_d(dst, dst, rhs);
    }
    store_gpr_dst(ir1->arg.rd, dst);
    if (!immediate) {
        free_alloc_gpr(rhs);
    }
    free_alloc_gpr(dst);
    free_alloc_gpr(lhs);
}

static void emit_zbb(const RiscvIR1 *ir1)
{
    IR2_OPND lhs, rhs, dst;

    switch (ir1->opcode) {
    case LATR_RV_CPOP:
        emit_popcount(ir1, false);
        return;
    case LATR_RV_CPOPW:
        emit_popcount(ir1, true);
        return;
    case LATR_RV_MIN: case LATR_RV_MAX:
        emit_minmax(ir1, ir1->opcode == LATR_RV_MAX, false);
        return;
    case LATR_RV_MINU: case LATR_RV_MAXU:
        emit_minmax(ir1, ir1->opcode == LATR_RV_MAXU, true);
        return;
    case LATR_RV_ORC_B:
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        la_mov64(dst, lhs);
        la_srli_d(a5_ir2_opnd, dst, 4);
        la_or(dst, dst, a5_ir2_opnd);
        la_srli_d(a5_ir2_opnd, dst, 2);
        la_or(dst, dst, a5_ir2_opnd);
        la_srli_d(a5_ir2_opnd, dst, 1);
        la_or(dst, dst, a5_ir2_opnd);
        li_d(a5_ir2_opnd, 0x0101010101010101);
        la_and(dst, dst, a5_ir2_opnd);
        li_d(a5_ir2_opnd, 0xff);
        la_mul_d(dst, dst, a5_ir2_opnd);
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        return;
    case LATR_RV_SEXT_B: case LATR_RV_SEXT_H:
    case LATR_RV_ZEXT_H_64: case LATR_RV_REV8_64:
    case LATR_RV_CLZ: case LATR_RV_CTZ:
    case LATR_RV_CLZW: case LATR_RV_CTZW:
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        switch (ir1->opcode) {
        case LATR_RV_SEXT_B:
            la_ext_w_b(dst, lhs);
            la_mov32_sx(dst, dst);
            break;
        case LATR_RV_SEXT_H:
            la_ext_w_h(dst, lhs);
            la_mov32_sx(dst, dst);
            break;
        case LATR_RV_ZEXT_H_64:
            la_bstrpick_d(dst, lhs, 15, 0);
            break;
        case LATR_RV_REV8_64:
            la_revb_d(dst, lhs);
            break;
        case LATR_RV_CLZ:
            la_clz_d(dst, lhs);
            break;
        case LATR_RV_CTZ:
            la_ctz_d(dst, lhs);
            break;
        case LATR_RV_CLZW:
            la_clz_w(dst, lhs);
            break;
        default:
            la_ctz_w(dst, lhs);
            break;
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        return;
    case LATR_RV_ANDN: case LATR_RV_ORN: case LATR_RV_XNOR:
        lhs = alloc_gpr_src(ir1->arg.rs1);
        rhs = alloc_gpr_src(ir1->arg.rs2);
        dst = alloc_gpr_dst(ir1->arg.rd);
        if (ir1->opcode == LATR_RV_ANDN) {
            la_andn(dst, lhs, rhs);
        } else if (ir1->opcode == LATR_RV_ORN) {
            la_orn(dst, lhs, rhs);
        } else {
            la_xor(dst, lhs, rhs);
            la_nor(dst, dst, zero_ir2_opnd);
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(rhs);
        free_alloc_gpr(lhs);
        return;
    case LATR_RV_ROL: case LATR_RV_ROR: case LATR_RV_RORI:
    case LATR_RV_ROLW: case LATR_RV_RORW: case LATR_RV_RORIW:
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        if (ir1->opcode == LATR_RV_RORI ||
            ir1->opcode == LATR_RV_RORIW) {
            if (ir1->opcode == LATR_RV_RORIW) {
                la_rotri_w(dst, lhs, ir1->arg.shamt);
                la_mov32_sx(dst, dst);
            } else {
                la_rotri_d(dst, lhs, ir1->arg.shamt);
            }
        } else {
            rhs = alloc_gpr_src(ir1->arg.rs2);
            if (ir1->opcode == LATR_RV_ROL ||
                ir1->opcode == LATR_RV_ROLW) {
                la_sub_d(a5_ir2_opnd, zero_ir2_opnd, rhs);
                if (ir1->opcode == LATR_RV_ROLW) {
                    la_rotr_w(dst, lhs, a5_ir2_opnd);
                    la_mov32_sx(dst, dst);
                } else {
                    la_rotr_d(dst, lhs, a5_ir2_opnd);
                }
            } else if (ir1->opcode == LATR_RV_RORW) {
                la_rotr_w(dst, lhs, rhs);
                la_mov32_sx(dst, dst);
            } else {
                la_rotr_d(dst, lhs, rhs);
            }
            free_alloc_gpr(rhs);
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        return;
    default:
        lsassertm(0, "invalid Zbb opcode");
        return;
    }
}

static void emit_zbs(const RiscvIR1 *ir1)
{
    bool immediate = ir1->opcode == LATR_RV_BSETI ||
                     ir1->opcode == LATR_RV_BCLRI ||
                     ir1->opcode == LATR_RV_BINVI ||
                     ir1->opcode == LATR_RV_BEXTI;
    IR2_OPND lhs = alloc_gpr_src(ir1->arg.rs1);
    IR2_OPND rhs = immediate ? zero_ir2_opnd : alloc_gpr_src(ir1->arg.rs2);
    IR2_OPND dst = alloc_gpr_dst(ir1->arg.rd);
    IR2_OPND mask = ra_alloc_itemp();

    if (ir1->opcode == LATR_RV_BEXTI) {
        la_bstrpick_d(dst, lhs, ir1->arg.shamt, ir1->arg.shamt);
    } else if (ir1->opcode == LATR_RV_BEXT) {
        la_srl_d(dst, lhs, rhs);
        la_andi(dst, dst, 1);
    } else {
        li_d(mask, 1);
        if (immediate) {
            la_slli_d(mask, mask, ir1->arg.shamt);
        } else {
            la_sll_d(mask, mask, rhs);
        }
        switch (ir1->opcode) {
        case LATR_RV_BSET: case LATR_RV_BSETI:
            la_or(dst, lhs, mask);
            break;
        case LATR_RV_BCLR: case LATR_RV_BCLRI:
            la_andn(dst, lhs, mask);
            break;
        default:
            la_xor(dst, lhs, mask);
            break;
        }
    }
    store_gpr_dst(ir1->arg.rd, dst);
    ra_free_temp(mask);
    free_alloc_gpr(dst);
    if (!immediate) {
        free_alloc_gpr(rhs);
    }
    free_alloc_gpr(lhs);
}

bool latr_translate_ir1(LatrTranslateContext *ctx)
{
    const RiscvIR1 *ir1 = ctx->ir1;
    IR2_OPND lhs, dst, imm;

#define LATR_RETIRE() do { latr_emit_instret(); return true; } while (0)

    emit_mcycle();
    /* FP instruction gate: mstatus.FS must not be Off. */
    if (ir1->opcode >= LATR_RV_FLW && ir1->opcode <= LATR_RV_FMV_D_X) {
        latr_emit_fs_gate(ir1);
    }
    if (latr_translate_rvv(ir1)) {
        return true;
    }
    if (latr_translate_privileged(ir1)) {
        return true;
    }
    if (latr_translate_csr(ir1)) {
        latr_emit_instret();
        return true;
    }
    if (ir1->opcode == LATR_RV_FSW || ir1->opcode == LATR_RV_FSD) {
        LatrStoreLock lock = emit_store_lock_acquire();
        bool handled = latr_translate_fp(ir1);
        emit_store_lock_release(lock);
        if (handled) {
            latr_emit_mark_fs_dirty();
            latr_emit_instret();
        }
        return handled;
    }
    if (latr_translate_fp(ir1)) {
        latr_emit_mark_fs_dirty();
        latr_emit_instret();
        return true;
    }
    switch (ir1->opcode) {
    case LATR_RV_ILLEGAL:
        latr_raise_illegal_instruction(ir1);
        return true;
    case LATR_RV_FETCH_FAULT:
        latr_raise_fetch_fault(ir1);
        return true;
    case LATR_RV_LUI:
        dst = alloc_gpr_dst(ir1->arg.rd);
        li_d(dst, ir1->arg.imm);
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        LATR_RETIRE();
    case LATR_RV_AUIPC:
        dst = alloc_gpr_dst(ir1->arg.rd);
        imm = ra_alloc_itemp();
        li_d(imm, (int64_t)ir1->pc + ir1->arg.imm);
        la_mov64(dst, imm);
        store_gpr_dst(ir1->arg.rd, dst);
        ra_free_temp(imm);
        free_alloc_gpr(dst);
        LATR_RETIRE();
    case LATR_RV_JAL:
        latr_emit_instret();
        emit_jump(ctx);
        return true;
    case LATR_RV_JALR:
        latr_emit_instret();
        emit_jump_register(ir1);
        return true;
    case LATR_RV_BEQ: case LATR_RV_BNE: case LATR_RV_BLT:
    case LATR_RV_BGE: case LATR_RV_BLTU: case LATR_RV_BGEU:
        latr_emit_instret();
        emit_branch(ctx);
        return true;
    case LATR_RV_LB: case LATR_RV_LH: case LATR_RV_LW:
    case LATR_RV_LBU: case LATR_RV_LHU: case LATR_RV_LWU: case LATR_RV_LD:
        emit_load(ir1);
        LATR_RETIRE();
    case LATR_RV_SB: case LATR_RV_SH: case LATR_RV_SW: case LATR_RV_SD:
        emit_store(ir1);
        LATR_RETIRE();
    case LATR_RV_ADDI: case LATR_RV_ADDIW:
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        if (ir1->opcode == LATR_RV_ADDI) {
            la_addi_d(dst, lhs, ir1->arg.imm);
        } else {
            la_addi_w(dst, lhs, ir1->arg.imm);
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        LATR_RETIRE();
    case LATR_RV_SLTI:
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        la_slti(dst, lhs, ir1->arg.imm);
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        LATR_RETIRE();
    case LATR_RV_SLTIU:
        emit_immediate_logic(ir1, la_sltu);
        LATR_RETIRE();
    case LATR_RV_XORI: emit_immediate_logic(ir1, la_xor); LATR_RETIRE();
    case LATR_RV_ORI:  emit_immediate_logic(ir1, la_or); LATR_RETIRE();
    case LATR_RV_ANDI: emit_immediate_logic(ir1, la_and); LATR_RETIRE();
    case LATR_RV_SLLI: case LATR_RV_SRLI: case LATR_RV_SRAI:
        if (ir1->arg.shamt < 0 || ir1->arg.shamt >= 64) {
            return false;
        }
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        if (ir1->opcode == LATR_RV_SLLI) {
            la_slli_d(dst, lhs, ir1->arg.shamt);
        } else if (ir1->opcode == LATR_RV_SRLI) {
            la_srli_d(dst, lhs, ir1->arg.shamt);
        } else {
            la_srai_d(dst, lhs, ir1->arg.shamt);
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        LATR_RETIRE();
    case LATR_RV_SLLIW: case LATR_RV_SRLIW: case LATR_RV_SRAIW:
        if (ir1->arg.shamt < 0 || ir1->arg.shamt >= 32) {
            return false;
        }
        lhs = alloc_gpr_src(ir1->arg.rs1);
        dst = alloc_gpr_dst(ir1->arg.rd);
        if (ir1->opcode == LATR_RV_SLLIW) {
            la_slli_w(dst, lhs, ir1->arg.shamt);
        } else if (ir1->opcode == LATR_RV_SRLIW) {
            la_srli_w(dst, lhs, ir1->arg.shamt);
        } else {
            la_srai_w(dst, lhs, ir1->arg.shamt);
        }
        store_gpr_dst(ir1->arg.rd, dst);
        free_alloc_gpr(dst);
        free_alloc_gpr(lhs);
        LATR_RETIRE();
    case LATR_RV_ADD: emit_binary(ir1, la_add_d); LATR_RETIRE();
    case LATR_RV_SUB: emit_binary(ir1, la_sub_d); LATR_RETIRE();
    case LATR_RV_SLL: emit_binary(ir1, la_sll_d); LATR_RETIRE();
    case LATR_RV_SLT: emit_binary(ir1, la_slt); LATR_RETIRE();
    case LATR_RV_SLTU: emit_binary(ir1, la_sltu); LATR_RETIRE();
    case LATR_RV_XOR: emit_binary(ir1, la_xor); LATR_RETIRE();
    case LATR_RV_SRL: emit_binary(ir1, la_srl_d); LATR_RETIRE();
    case LATR_RV_SRA: emit_binary(ir1, la_sra_d); LATR_RETIRE();
    case LATR_RV_OR: emit_binary(ir1, la_or); LATR_RETIRE();
    case LATR_RV_AND: emit_binary(ir1, la_and); LATR_RETIRE();
    case LATR_RV_SH1ADD: case LATR_RV_SH2ADD: case LATR_RV_SH3ADD:
    case LATR_RV_ADD_UW: case LATR_RV_SH1ADD_UW: case LATR_RV_SH2ADD_UW:
    case LATR_RV_SH3ADD_UW:
        emit_zba(ir1);
        LATR_RETIRE();
    case LATR_RV_SLLI_UW:
        if (ir1->arg.shamt < 0 || ir1->arg.shamt >= 64) {
            return false;
        }
        emit_zba(ir1);
        LATR_RETIRE();
    case LATR_RV_ANDN: case LATR_RV_ORN: case LATR_RV_XNOR:
    case LATR_RV_CLZ: case LATR_RV_CTZ: case LATR_RV_CPOP:
    case LATR_RV_MIN: case LATR_RV_MAX: case LATR_RV_MINU: case LATR_RV_MAXU:
    case LATR_RV_SEXT_B: case LATR_RV_SEXT_H: case LATR_RV_ZEXT_H_64:
    case LATR_RV_ROL: case LATR_RV_ROR: case LATR_RV_ORC_B:
    case LATR_RV_REV8_64: case LATR_RV_CLZW: case LATR_RV_CTZW:
    case LATR_RV_CPOPW: case LATR_RV_ROLW: case LATR_RV_RORW:
        emit_zbb(ir1);
        LATR_RETIRE();
    case LATR_RV_RORI:
        if (ir1->arg.shamt < 0 || ir1->arg.shamt >= 64) {
            return false;
        }
        emit_zbb(ir1);
        LATR_RETIRE();
    case LATR_RV_RORIW:
        if (ir1->arg.shamt < 0 || ir1->arg.shamt >= 32) {
            return false;
        }
        emit_zbb(ir1);
        LATR_RETIRE();
    case LATR_RV_BSET: case LATR_RV_BCLR: case LATR_RV_BINV: case LATR_RV_BEXT:
        emit_zbs(ir1);
        LATR_RETIRE();
    case LATR_RV_BSETI: case LATR_RV_BCLRI: case LATR_RV_BINVI: case LATR_RV_BEXTI:
        if (ir1->arg.shamt < 0 || ir1->arg.shamt >= 64) {
            return false;
        }
        emit_zbs(ir1);
        LATR_RETIRE();
    case LATR_RV_ADDW: emit_binary(ir1, la_add_w); LATR_RETIRE();
    case LATR_RV_SUBW: emit_binary(ir1, la_sub_w); LATR_RETIRE();
    case LATR_RV_SLLW: emit_binary(ir1, la_sll_w); LATR_RETIRE();
    case LATR_RV_SRLW: emit_binary(ir1, la_srl_w); LATR_RETIRE();
    case LATR_RV_SRAW: emit_binary(ir1, la_sra_w); LATR_RETIRE();
    case LATR_RV_MUL: emit_binary(ir1, la_mul_d); LATR_RETIRE();
    case LATR_RV_MULH: emit_binary(ir1, la_mulh_d); LATR_RETIRE();
    case LATR_RV_MULHSU: emit_multiply_high_unsigned_signed(ir1); LATR_RETIRE();
    case LATR_RV_MULHU: emit_binary(ir1, la_mulh_du); LATR_RETIRE();
    case LATR_RV_MULW: emit_binary(ir1, la_mul_w); LATR_RETIRE();
    case LATR_RV_DIV: case LATR_RV_DIVU: case LATR_RV_REM: case LATR_RV_REMU:
    case LATR_RV_DIVW: case LATR_RV_DIVUW: case LATR_RV_REMW: case LATR_RV_REMUW:
        emit_division(ir1);
        LATR_RETIRE();
    case LATR_RV_LR_W: case LATR_RV_LR_D:
        emit_lr(ir1, ir1->opcode == LATR_RV_LR_W);
        LATR_RETIRE();
    case LATR_RV_SC_W: case LATR_RV_SC_D:
        emit_sc(ir1, ir1->opcode == LATR_RV_SC_W);
        LATR_RETIRE();
    case LATR_RV_AMOSWAP_W: emit_amo(ir1, la_amswap_w, true); LATR_RETIRE();
    case LATR_RV_AMOADD_W: emit_amo(ir1, la_amadd_w, true); LATR_RETIRE();
    case LATR_RV_AMOXOR_W: emit_amo(ir1, la_amxor_w, true); LATR_RETIRE();
    case LATR_RV_AMOAND_W: emit_amo(ir1, la_amand_w, true); LATR_RETIRE();
    case LATR_RV_AMOOR_W: emit_amo(ir1, la_amor_w, true); LATR_RETIRE();
    case LATR_RV_AMOMIN_W: emit_amo(ir1, la_ammin_w, true); LATR_RETIRE();
    case LATR_RV_AMOMAX_W: emit_amo(ir1, la_ammax_w, true); LATR_RETIRE();
    case LATR_RV_AMOMINU_W: emit_amo(ir1, la_ammin_wu, true); LATR_RETIRE();
    case LATR_RV_AMOMAXU_W: emit_amo(ir1, la_ammax_wu, true); LATR_RETIRE();
    case LATR_RV_AMOSWAP_D: emit_amo(ir1, la_amswap_d, false); LATR_RETIRE();
    case LATR_RV_AMOADD_D: emit_amo(ir1, la_amadd_d, false); LATR_RETIRE();
    case LATR_RV_AMOXOR_D: emit_amo(ir1, la_amxor_d, false); LATR_RETIRE();
    case LATR_RV_AMOAND_D: emit_amo(ir1, la_amand_d, false); LATR_RETIRE();
    case LATR_RV_AMOOR_D: emit_amo(ir1, la_amor_d, false); LATR_RETIRE();
    case LATR_RV_AMOMIN_D: emit_amo(ir1, la_ammin_d, false); LATR_RETIRE();
    case LATR_RV_AMOMAX_D: emit_amo(ir1, la_ammax_d, false); LATR_RETIRE();
    case LATR_RV_AMOMINU_D: emit_amo(ir1, la_ammin_du, false); LATR_RETIRE();
    case LATR_RV_AMOMAXU_D: emit_amo(ir1, la_ammax_du, false); LATR_RETIRE();
    case LATR_RV_FENCE:
        la_dbar(0);
        LATR_RETIRE();
    default:
        return false;
    }

#undef LATR_RETIRE
}
