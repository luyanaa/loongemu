#include "latr.h"
#include "translate.h"
#include "loongemu/vclock.h"

#define CSR_FFLAGS       0x001
#define CSR_FRM          0x002
#define CSR_FCSR         0x003
#define CSR_SSTATUS      0x100
#define CSR_SIE          0x104
#define CSR_STVEC        0x105
#define CSR_SCOUNTEREN   0x106
#define CSR_SENVCFG      0x10a
#define CSR_SSCRATCH     0x140
#define CSR_SEPC         0x141
#define CSR_SCAUSE       0x142
#define CSR_STVAL        0x143
#define CSR_SIP          0x144
#define CSR_STIMECMP     LATR_RISCV_CSR_STIMECMP
#define CSR_MSTATUS      0x300
#define CSR_MISA         0x301
#define CSR_MEDELEG      0x302
#define CSR_MIDELEG      0x303
#define CSR_MIE          0x304
#define CSR_MTVEC        0x305
#define CSR_MCOUNTEREN   0x306
#define CSR_MENVCFG      0x30a
#define CSR_MCOUNTINHIBIT 0x320
#define CSR_MSCRATCH     0x340
#define CSR_MEPC         0x341
#define CSR_MCAUSE       0x342
#define CSR_MTVAL        0x343
#define CSR_MIP          LATR_RISCV_CSR_MIP
#define CSR_MCYCLE       0xb00
#define CSR_MINSTRET     0xb02
#define CSR_CYCLE        0xc00
#define CSR_TIME         0xc01
#define CSR_INSTRET      0xc02

#define CSR_USER_CUSTOM_FIRST 0x800
#define CSR_USER_CUSTOM_LAST  0x8ff

#define MSTATUS_SIE   (UINT64_C(1) << 1)
#define MSTATUS_MIE   (UINT64_C(1) << 3)
#define MSTATUS_SPIE  (UINT64_C(1) << 5)
#define MSTATUS_MPIE  (UINT64_C(1) << 7)
#define MSTATUS_SPP   (UINT64_C(1) << 8)
#define MSTATUS_MPP   (UINT64_C(3) << 11)
#define MSTATUS_FS    (UINT64_C(3) << 13)
#define MSTATUS_XS    (UINT64_C(3) << 15)
#define MSTATUS_MPRV  (UINT64_C(1) << 17)
#define MSTATUS_SUM   (UINT64_C(1) << 18)
#define MSTATUS_MXR   (UINT64_C(1) << 19)
#define MSTATUS_TVM   (UINT64_C(1) << 20)
#define MSTATUS_TW    (UINT64_C(1) << 21)
#define MSTATUS_TSR   (UINT64_C(1) << 22)
#define MSTATUS_UXL  (UINT64_C(2) << 32)
#define MSTATUS_SXL  (UINT64_C(2) << 34)

#define SSTATUS_WRITE_MASK (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | \
                            MSTATUS_FS | MSTATUS_SUM | MSTATUS_MXR)
#define MSTATUS_WRITE_MASK (SSTATUS_WRITE_MASK | MSTATUS_MIE | MSTATUS_MPIE | \
                            MSTATUS_MPP | MSTATUS_MPRV | MSTATUS_TVM | \
                            MSTATUS_TW | MSTATUS_TSR)
#define MIE_WRITABLE_MASK (UINT64_C(0xaaa))
#define MIP_WRITABLE_MASK (UINT64_C(1) << 1)
#define ENVCFG_WRITABLE_MASK ((UINT64_C(1) << 63) | UINT64_C(0xf0))
#define SENVCFG_WRITABLE_MASK UINT64_C(0xf0)
#define MISA_RV64GC (UINT64_C(0x800000000000112d))

typedef enum LatrCsrKind {
    LATR_CSR_FFLAGS,
    LATR_CSR_FRM,
    LATR_CSR_FCSR,
    LATR_CSR_STORAGE,
    LATR_CSR_SSTATUS,
    LATR_CSR_SIE,
    LATR_CSR_SIP,
    LATR_CSR_MIP,
    LATR_CSR_MISA,
    LATR_CSR_CYCLE,
    LATR_CSR_TIME,
    LATR_CSR_INSTRET,
    LATR_CSR_USER_CUSTOM,
} LatrCsrKind;

static bool csr_kind(int csr, LatrCsrKind *kind)
{
    switch (csr) {
    case CSR_FFLAGS:       *kind = LATR_CSR_FFLAGS; return true;
    case CSR_FRM:          *kind = LATR_CSR_FRM; return true;
    case CSR_FCSR:         *kind = LATR_CSR_FCSR; return true;
    case CSR_SSTATUS:      *kind = LATR_CSR_SSTATUS; return true;
    case CSR_SIE:          *kind = LATR_CSR_SIE; return true;
    case CSR_SIP:          *kind = LATR_CSR_SIP; return true;
    case CSR_MIP:          *kind = LATR_CSR_MIP; return true;
    case CSR_MISA:         *kind = LATR_CSR_MISA; return true;
    case CSR_CYCLE:        *kind = LATR_CSR_CYCLE; return true;
    case CSR_TIME:         *kind = LATR_CSR_TIME; return true;
    case CSR_INSTRET:      *kind = LATR_CSR_INSTRET; return true;
    case CSR_STVEC:
    case CSR_SCOUNTEREN:
    case CSR_SENVCFG:
    case CSR_SSCRATCH:
    case CSR_SEPC:
    case CSR_SCAUSE:
    case CSR_STVAL:
    case CSR_STIMECMP:
    case CSR_MSTATUS:
    case CSR_MEDELEG:
    case CSR_MIDELEG:
    case CSR_MIE:
    case CSR_MTVEC:
    case CSR_MCOUNTEREN:
    case CSR_MENVCFG:
    case CSR_MCOUNTINHIBIT:
    case CSR_MSCRATCH:
    case CSR_MEPC:
    case CSR_MCAUSE:
    case CSR_MTVAL:
    case CSR_MCYCLE:
    case CSR_MINSTRET:
        *kind = LATR_CSR_STORAGE;
        return true;
    default:
        /* PMP has no placeholder storage; other unknown CSRs trap illegal. */
        if (csr >= CSR_USER_CUSTOM_FIRST && csr <= CSR_USER_CUSTOM_LAST) {
            *kind = LATR_CSR_USER_CUSTOM;
            return true;
        }
        return false;
    }
}

static bool csr_is_read_only(int csr)
{
    return (csr & 0xc00) == 0xc00 || csr == CSR_MISA;
}

static bool csr_is_counter(int csr)
{
    return csr == CSR_CYCLE || csr == CSR_TIME || csr == CSR_INSTRET;
}

static void emit_runtime_call(uintptr_t function)
{
    IR2_OPND call_target = ra_alloc_itemp();

    latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    li_d(call_target, (int64_t)function);
    la_jirl(ra_ir2_opnd, call_target, 0);
    ra_free_temp(call_target);
    latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
}

static uint64_t latr_read_time(CPUArchState *env)
{
    return loongemu_vclock_now_ns(env->vclock);
}

static void emit_status_sd(IR2_OPND status)
{
    IR2_OPND fs = a0_ir2_opnd;
    IR2_OPND xs = a1_ir2_opnd;
    IR2_OPND three = a2_ir2_opnd;
    IR2_OPND dirty = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

    la_bstrins_d(status, zero_ir2_opnd, 63, 63);
    la_bstrpick_d(fs, status, 14, 13);
    li_d(three, 3);
    la_beq(fs, three, dirty);
    la_bstrpick_d(xs, status, 16, 15);
    la_bne(xs, three, done);
    la_label(dirty);
    li_d(fs, 1);
    la_bstrins_d(status, fs, 63, 63);
    la_label(done);
}

static void load_csr(IR2_OPND dst, LatrCsrKind kind, int csr)
{
    IR2_OPND tmp;

    switch (kind) {
    case LATR_CSR_FFLAGS:
        la_ld_d(dst, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(dst, dst, 4, 0);
        break;
    case LATR_CSR_FRM:
        la_ld_d(dst, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(dst, dst, 7, 5);
        break;
    case LATR_CSR_FCSR:
        la_ld_d(dst, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(dst, dst, 7, 0);
        break;
    case LATR_CSR_SSTATUS:
        latr_env_load_d(dst, env_offset_csr(CSR_MSTATUS));
        tmp = ra_alloc_itemp();
        li_d(tmp, (int64_t)SSTATUS_WRITE_MASK);
        la_and(dst, dst, tmp);
        li_d(tmp, (int64_t)MSTATUS_UXL);
        la_or(dst, dst, tmp);
        emit_status_sd(dst);
        ra_free_temp(tmp);
        break;
    case LATR_CSR_SIE:
        latr_env_load_d(dst, env_offset_csr(CSR_MIE));
        tmp = ra_alloc_itemp();
        latr_env_load_d(tmp, env_offset_csr(CSR_MIDELEG));
        la_and(dst, dst, tmp);
        ra_free_temp(tmp);
        break;
    case LATR_CSR_SIP:
        latr_env_load_d(dst, env_offset_csr(CSR_MIP));
        tmp = ra_alloc_itemp();
        latr_env_load_d(tmp, env_offset_csr(CSR_MIDELEG));
        la_and(dst, dst, tmp);
        ra_free_temp(tmp);
        break;
    case LATR_CSR_MIP:
        latr_env_load_d(dst, env_offset_csr(CSR_MIP));
        break;
    case LATR_CSR_MISA:
        li_d(dst, (int64_t)MISA_RV64GC);
        break;
    case LATR_CSR_CYCLE:
        latr_env_load_d(dst, env_offset_csr(CSR_MCYCLE));
        break;
    case LATR_CSR_TIME:
        emit_runtime_call((uintptr_t)latr_read_time);
        la_mov64(dst, a0_ir2_opnd);
        break;
    case LATR_CSR_INSTRET:
        latr_env_load_d(dst, env_offset_csr(CSR_MINSTRET));
        break;
    case LATR_CSR_USER_CUSTOM:
    case LATR_CSR_STORAGE:
        latr_env_load_d(dst, env_offset_csr(csr));
        if (csr == CSR_MSTATUS) {
            tmp = ra_alloc_itemp();
            li_d(tmp, (int64_t)(MSTATUS_UXL | MSTATUS_SXL));
            la_or(dst, dst, tmp);
            emit_status_sd(dst);
            ra_free_temp(tmp);
        }
        break;
    }
}

static void store_masked_csr(IR2_OPND value, int csr, uint64_t writable_mask)
{
    IR2_OPND old_value = ra_alloc_itemp();
    IR2_OPND mask = ra_alloc_itemp();
    IR2_OPND inverse_mask = ra_alloc_itemp();

    latr_env_load_d(old_value, env_offset_csr(csr));
    li_d(mask, (int64_t)writable_mask);
    la_nor(inverse_mask, mask, zero_ir2_opnd);
    la_and(old_value, old_value, inverse_mask);
    la_and(inverse_mask, value, mask);
    la_or(old_value, old_value, inverse_mask);
    latr_env_store_d(old_value, env_offset_csr(csr));

    ra_free_temp(inverse_mask);
    ra_free_temp(mask);
    ra_free_temp(old_value);
}

static void store_csr(IR2_OPND value, LatrCsrKind kind, int csr)
{
    IR2_OPND state, mask, old_value, delegated, inverse_mask, filtered, field;

    switch (kind) {
    case LATR_CSR_FFLAGS:
        state = ra_alloc_itemp();
        la_ld_d(state, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(state, state, 7, 0);
        la_bstrins_d(state, value, 4, 0);
        la_st_d(state, env_ir2_opnd, env_offset_fcsr());
        ra_free_temp(state);
        break;
    case LATR_CSR_FRM:
        state = ra_alloc_itemp();
        la_ld_d(state, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(state, state, 7, 0);
        la_bstrins_d(state, value, 7, 5);
        la_st_d(state, env_ir2_opnd, env_offset_fcsr());
        ra_free_temp(state);
        break;
    case LATR_CSR_FCSR:
        state = ra_alloc_itemp();
        la_bstrpick_d(state, value, 7, 0);
        la_st_d(state, env_ir2_opnd, env_offset_fcsr());
        ra_free_temp(state);
        break;
    case LATR_CSR_SSTATUS:
        store_masked_csr(value, CSR_MSTATUS, SSTATUS_WRITE_MASK);
        break;
    case LATR_CSR_SIE:
    case LATR_CSR_SIP:
        csr = kind == LATR_CSR_SIE ? CSR_MIE : CSR_MIP;
        old_value = ra_alloc_itemp();
        delegated = ra_alloc_itemp();
        filtered = ra_alloc_itemp();
        latr_env_load_d(old_value, env_offset_csr(csr));
        latr_env_load_d(delegated, env_offset_csr(CSR_MIDELEG));
        if (kind == LATR_CSR_SIE) {
            la_andi(delegated, delegated, 0xaaa);
        } else {
            la_andi(delegated, delegated, 2);
        }
        la_andn(old_value, old_value, delegated);
        la_and(filtered, value, delegated);
        la_or(filtered, filtered, old_value);
        latr_env_store_d(filtered, env_offset_csr(csr));
        ra_free_temp(filtered);
        ra_free_temp(delegated);
        ra_free_temp(old_value);
        break;
    case LATR_CSR_MIP:
        store_masked_csr(value, CSR_MIP, MIP_WRITABLE_MASK);
        break;
    case LATR_CSR_MISA:
    case LATR_CSR_CYCLE:
    case LATR_CSR_TIME:
    case LATR_CSR_INSTRET:
        break;
    case LATR_CSR_USER_CUSTOM:
        latr_env_store_d(value, env_offset_csr(csr));
        break;
    case LATR_CSR_STORAGE:
        switch (csr) {
        case CSR_MSTATUS:
            store_masked_csr(value, CSR_MSTATUS, MSTATUS_WRITE_MASK);
            field = ra_alloc_itemp();
            mask = ra_alloc_itemp();
            latr_env_load_d(field, env_offset_csr(CSR_MSTATUS));
            la_bstrpick_d(field, field, 12, 11);
            li_d(mask, 2);
            {
                IR2_OPND mpp_ok = ir2_opnd_new_type(IR2_OPND_LABEL);
                la_bne(field, mask, mpp_ok);
                latr_env_load_d(field, env_offset_csr(CSR_MSTATUS));
                la_bstrins_d(field, zero_ir2_opnd, 12, 11);
                latr_env_store_d(field, env_offset_csr(CSR_MSTATUS));
                la_label(mpp_ok);
            }
            ra_free_temp(mask);
            ra_free_temp(field);
            break;
        case CSR_STIMECMP:
            latr_env_store_d(value, env_offset_csr(csr));
            state = ra_alloc_itemp();
            li_d(state, 1);
            latr_env_store_d(state, env_offset_stimecmp_update_pending());
            ra_free_temp(state);
            break;
        case CSR_MIE:
            store_masked_csr(value, csr, MIE_WRITABLE_MASK);
            break;
        case CSR_MCOUNTEREN:
        case CSR_SCOUNTEREN:
            store_masked_csr(value, csr, UINT64_C(7));
            break;
        case CSR_MCOUNTINHIBIT:
            store_masked_csr(value, csr, UINT64_C(5));
            break;
        case CSR_MENVCFG:
            store_masked_csr(value, csr, ENVCFG_WRITABLE_MASK);
            break;
        case CSR_SENVCFG:
            store_masked_csr(value, csr, SENVCFG_WRITABLE_MASK);
            break;
        case CSR_MIP:
            store_masked_csr(value, csr, MIP_WRITABLE_MASK);
            break;
        case CSR_MTVEC:
        case CSR_STVEC:
            state = ra_alloc_itemp();
            field = ra_alloc_itemp();
            mask = ra_alloc_itemp();
            la_mov64(field, value);
            la_bstrpick_d(mask, field, 1, 0);
            li_d(state, 1);
            {
                IR2_OPND mode_ok = ir2_opnd_new_type(IR2_OPND_LABEL);
                la_beq(mask, state, mode_ok);
                la_bstrins_d(field, zero_ir2_opnd, 1, 0);
                la_label(mode_ok);
            }
            latr_env_store_d(field, env_offset_csr(csr));
            ra_free_temp(mask);
            ra_free_temp(field);
            ra_free_temp(state);
            break;
        case CSR_MEPC:
        case CSR_SEPC:
            field = ra_alloc_itemp();
            la_mov64(field, value);
            la_bstrins_d(field, zero_ir2_opnd, 0, 0);
            latr_env_store_d(field, env_offset_csr(csr));
            ra_free_temp(field);
            break;
        default:
            latr_env_store_d(value, env_offset_csr(csr));
            break;
        }
        break;
    }
}

static void emit_mstatus_s_trap(IR2_OPND cause, uint64_t pc,
                                IR2_OPND trap_value, IR2_OPND privilege,
                                IR2_OPND status, IR2_OPND tmp,
                                IR2_OPND target)
{
    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(tmp, status, 1, 1);
    la_bstrins_d(status, tmp, 5, 5);
    la_bstrins_d(status, zero_ir2_opnd, 1, 1);
    la_bstrins_d(status, privilege, 8, 8);
    latr_env_store_d(status, env_offset_csr(CSR_MSTATUS));

    li_d(tmp, (int64_t)(pc & ~UINT64_C(1)));
    latr_env_store_d(tmp, env_offset_csr(CSR_SEPC));
    latr_env_store_d(cause, env_offset_csr(CSR_SCAUSE));
    latr_env_store_d(trap_value, env_offset_csr(CSR_STVAL));
    li_d(tmp, LATR_RISCV_PRIV_S);
    latr_env_store_d(tmp, env_offset_privilege());
    latr_env_load_d(target, env_offset_csr(CSR_STVEC));
    la_bstrins_d(target, zero_ir2_opnd, 1, 0);
    la_st_d(target, env_ir2_opnd, env_offset_pc());
}

static void emit_mstatus_m_trap(IR2_OPND cause, uint64_t pc,
                                IR2_OPND trap_value, IR2_OPND privilege,
                                IR2_OPND status, IR2_OPND tmp,
                                IR2_OPND target)
{
    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(tmp, status, 3, 3);
    la_bstrins_d(status, tmp, 7, 7);
    la_bstrins_d(status, zero_ir2_opnd, 3, 3);
    la_bstrins_d(status, privilege, 12, 11);
    latr_env_store_d(status, env_offset_csr(CSR_MSTATUS));

    li_d(tmp, (int64_t)(pc & ~UINT64_C(1)));
    latr_env_store_d(tmp, env_offset_csr(CSR_MEPC));
    latr_env_store_d(cause, env_offset_csr(CSR_MCAUSE));
    latr_env_store_d(trap_value, env_offset_csr(CSR_MTVAL));
    li_d(tmp, LATR_RISCV_PRIV_M);
    latr_env_store_d(tmp, env_offset_privilege());
    latr_env_load_d(target, env_offset_csr(CSR_MTVEC));
    la_bstrins_d(target, zero_ir2_opnd, 1, 0);
    la_st_d(target, env_ir2_opnd, env_offset_pc());
}

static void transfer_exception(IR2_OPND cause, uint64_t pc,
                               IR2_OPND trap_value)
{
    IR2_OPND privilege = a2_ir2_opnd;
    IR2_OPND delegated = a3_ir2_opnd;
    IR2_OPND bit = a4_ir2_opnd;
    IR2_OPND status = a5_ir2_opnd;
    IR2_OPND tmp = a6_ir2_opnd;
    IR2_OPND target = a7_ir2_opnd;
    IR2_OPND machine = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

    la_st_d(cause, env_ir2_opnd, env_offset_exception());
    la_st_d(trap_value, env_ir2_opnd, env_offset_badaddr());
    latr_env_load_d(privilege, env_offset_privilege());
    li_d(tmp, LATR_RISCV_PRIV_M);
    la_beq(privilege, tmp, machine);
    latr_env_load_d(delegated, env_offset_csr(CSR_MEDELEG));
    li_d(bit, 1);
    la_sll_d(bit, bit, cause);
    la_and(delegated, delegated, bit);
    la_beq(delegated, zero_ir2_opnd, machine);

    emit_mstatus_s_trap(cause, pc, trap_value, privilege, status, tmp, target);
    la_b(done);

    la_label(machine);
    emit_mstatus_m_trap(cause, pc, trap_value, privilege, status, tmp, target);
    la_label(done);
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
}

void latr_raise_illegal_instruction(const RiscvIR1 *ir1)
{
    IR2_OPND cause = a0_ir2_opnd;
    IR2_OPND trap_value = a1_ir2_opnd;

    li_d(cause, 2);
    li_d(trap_value, ir1->insn);
    transfer_exception(cause, ir1->pc, trap_value);
}

void latr_emit_instret(void)
{
    IR2_OPND inhibit = a0_ir2_opnd;
    IR2_OPND count = a1_ir2_opnd;
    IR2_OPND one = a2_ir2_opnd;
    IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(inhibit, env_offset_csr(CSR_MCOUNTINHIBIT));
    la_andi(inhibit, inhibit, 4);
    la_bne(inhibit, zero_ir2_opnd, skip);
    latr_env_load_d(count, env_offset_csr(CSR_MINSTRET));
    li_d(one, 1);
    la_add_d(count, count, one);
    latr_env_store_d(count, env_offset_csr(CSR_MINSTRET));
    la_label(skip);
}

static void emit_csr_access_checks(int csr, IR2_OPND illegal)
{
    IR2_OPND privilege = ra_alloc_itemp();
    IR2_OPND value = ra_alloc_itemp();
    IR2_OPND mask = ra_alloc_itemp();
    IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(privilege, env_offset_privilege());
    li_d(value, 2);
    la_beq(privilege, value, illegal);
    li_d(value, (csr >> 8) & 3);
    la_bltu(privilege, value, illegal);

    if (csr_is_counter(csr)) {
        IR2_OPND user_only = ir2_opnd_new_type(IR2_OPND_LABEL);
        li_d(value, LATR_RISCV_PRIV_M);
        la_beq(privilege, value, skip);
        latr_env_load_d(value, env_offset_csr(CSR_MCOUNTEREN));
        li_d(mask, UINT64_C(1) << (csr - CSR_CYCLE));
        la_and(value, value, mask);
        la_beq(value, zero_ir2_opnd, illegal);
        la_bne(privilege, zero_ir2_opnd, user_only);
        latr_env_load_d(value, env_offset_csr(CSR_SCOUNTEREN));
        li_d(mask, UINT64_C(1) << (csr - CSR_CYCLE));
        la_and(value, value, mask);
        la_beq(value, zero_ir2_opnd, illegal);
        la_label(user_only);
        la_label(skip);
    }

    if (csr == CSR_STIMECMP) {
        IR2_OPND supervisor = ir2_opnd_new_type(IR2_OPND_LABEL);
        li_d(value, LATR_RISCV_PRIV_S);
        la_bne(privilege, value, supervisor);
        latr_env_load_d(value, env_offset_csr(CSR_MENVCFG));
        la_bstrpick_d(value, value, 63, 63);
        la_beq(value, zero_ir2_opnd, illegal);
        la_label(supervisor);
    }

    ra_free_temp(mask);
    ra_free_temp(value);
    ra_free_temp(privilege);
}

static bool decode_csr_op(LatrRiscvOpcode opcode, bool *immediate,
                          bool *csrrw, bool *set, bool *clear)
{
    *immediate = false;
    *csrrw = false;
    *set = false;
    *clear = false;

    switch (opcode) {
    case LATR_RV_CSRRW:  *csrrw = true; return true;
    case LATR_RV_CSRRS:  *set = true; return true;
    case LATR_RV_CSRRC:  *clear = true; return true;
    case LATR_RV_CSRRWI: *immediate = true; *csrrw = true; return true;
    case LATR_RV_CSRRSI: *immediate = true; *set = true; return true;
    case LATR_RV_CSRRCI: *immediate = true; *clear = true; return true;
    default: return false;
    }
}

static bool translate_csr(const RiscvIR1 *ir1)
{
    bool immediate, csrrw, set, clear;
    if (!decode_csr_op(ir1->opcode, &immediate, &csrrw, &set, &clear)) {
        return false;
    }

    int csr = ir1->arg.csr;
    int rd = ir1->arg.rd;
    int source_index = immediate ? ir1->arg.zimm : ir1->arg.rs1;
    LatrCsrKind kind;
    if (csr < 0 || csr >= LATR_RISCV_CSR_COUNT || rd < 0 || rd >= 32 ||
        source_index < 0 || source_index >= 32 || !csr_kind(csr, &kind)) {
        latr_raise_illegal_instruction(ir1);
        return true;
    }

    bool write_intent = csrrw || source_index != 0;
    if (write_intent && csr_is_read_only(csr)) {
        latr_raise_illegal_instruction(ir1);
        return true;
    }

    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    emit_csr_access_checks(csr, illegal);

    IR2_OPND source;
    bool free_source = false;
    if (immediate) {
        if (source_index == 0) {
            source = zero_ir2_opnd;
        } else {
            source = ra_alloc_itemp();
            li_d(source, source_index);
            free_source = true;
        }
    } else {
        source = alloc_gpr_src(source_index);
        free_source = source_index != 0;
    }

    IR2_OPND old_value = zero_ir2_opnd;
    bool have_old_value = !csrrw || rd != 0;
    if (have_old_value) {
        old_value = ra_alloc_itemp();
        load_csr(old_value, kind, csr);
    }

    if (rd != 0 && have_old_value) {
        IR2_OPND dst = alloc_gpr_dst(rd);
        la_mov64(dst, old_value);
        store_gpr_dst(rd, dst);
        free_alloc_gpr(dst);
    }

    if (write_intent) {
        IR2_OPND new_value;
        bool free_new_value = false;
        if (csrrw) {
            new_value = source;
        } else {
            new_value = ra_alloc_itemp();
            free_new_value = true;
            if (set) {
                la_or(new_value, old_value, source);
            } else {
                IR2_OPND inverted_source = ra_alloc_itemp();
                la_nor(inverted_source, source, zero_ir2_opnd);
                la_and(new_value, old_value, inverted_source);
                ra_free_temp(inverted_source);
            }
        }
        store_csr(new_value, kind, csr);
        if (free_new_value) {
            ra_free_temp(new_value);
        }
    }

    if (have_old_value) {
        ra_free_temp(old_value);
    }
    if (free_source) {
        free_alloc_gpr(source);
    }
    la_b(done);
    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(done);
    return true;
}


static void emit_cbo_permission_check(IR2_OPND illegal, bool invalidate,
                                      unsigned config_bit)
{
    IR2_OPND privilege = ra_alloc_itemp();
    IR2_OPND value = ra_alloc_itemp();
    IR2_OPND mask = ra_alloc_itemp();
    IR2_OPND allowed = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(privilege, env_offset_privilege());
    li_d(value, 2);
    la_beq(privilege, value, illegal);
    li_d(value, LATR_RISCV_PRIV_M);
    la_beq(privilege, value, allowed);

    latr_env_load_d(value, env_offset_csr(CSR_MENVCFG));
    if (invalidate) {
        la_bstrpick_d(value, value, 5, 4);
        la_beq(value, zero_ir2_opnd, illegal);
        li_d(mask, 3);
        la_beq(value, mask, illegal);
    } else {
        li_d(mask, config_bit);
        la_srl_d(value, value, mask);
        la_andi(value, value, 1);
        la_beq(value, zero_ir2_opnd, illegal);
    }

    li_d(value, LATR_RISCV_PRIV_U);
    la_bne(privilege, value, allowed);
    latr_env_load_d(value, env_offset_csr(CSR_SENVCFG));
    if (invalidate) {
        la_bstrpick_d(value, value, 5, 4);
        la_beq(value, zero_ir2_opnd, illegal);
        li_d(mask, 3);
        la_beq(value, mask, illegal);
    } else {
        li_d(mask, config_bit);
        la_srl_d(value, value, mask);
        la_andi(value, value, 1);
        la_beq(value, zero_ir2_opnd, illegal);
    }
    la_label(allowed);
    ra_free_temp(mask);
    ra_free_temp(value);
    ra_free_temp(privilege);
}

static void translate_mret(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND privilege = ra_alloc_itemp();
    IR2_OPND status = ra_alloc_itemp();
    IR2_OPND new_privilege = ra_alloc_itemp();
    IR2_OPND tmp = ra_alloc_itemp();
    IR2_OPND target = ra_alloc_itemp();
    IR2_OPND mode_ok = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(privilege, env_offset_privilege());
    li_d(tmp, LATR_RISCV_PRIV_M);
    la_bne(privilege, tmp, illegal);
    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(new_privilege, status, 12, 11);
    li_d(tmp, 2);
    la_bne(new_privilege, tmp, mode_ok);
    li_d(new_privilege, LATR_RISCV_PRIV_U);
    la_label(mode_ok);

    la_bstrpick_d(tmp, status, 7, 7);
    la_bstrins_d(status, tmp, 3, 3);
    li_d(tmp, 1);
    la_bstrins_d(status, tmp, 7, 7);
    la_bstrins_d(status, zero_ir2_opnd, 12, 11);
    {
        IR2_OPND keep_mprv = ir2_opnd_new_type(IR2_OPND_LABEL);
        li_d(tmp, LATR_RISCV_PRIV_M);
        la_beq(new_privilege, tmp, keep_mprv);
        la_bstrins_d(status, zero_ir2_opnd, 17, 17);
        la_label(keep_mprv);
    }
    latr_env_store_d(status, env_offset_csr(CSR_MSTATUS));
    latr_env_store_d(new_privilege, env_offset_privilege());
    latr_env_load_d(target, env_offset_csr(CSR_MEPC));
    la_bstrins_d(target, zero_ir2_opnd, 0, 0);
    la_st_d(target, env_ir2_opnd, env_offset_pc());
    latr_emit_instret();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);

    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    ra_free_temp(target);
    ra_free_temp(tmp);
    ra_free_temp(new_privilege);
    ra_free_temp(status);
    ra_free_temp(privilege);
}

static void translate_sret(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND privilege = ra_alloc_itemp();
    IR2_OPND status = ra_alloc_itemp();
    IR2_OPND new_privilege = ra_alloc_itemp();
    IR2_OPND tmp = ra_alloc_itemp();
    IR2_OPND target = ra_alloc_itemp();
    IR2_OPND sret_allowed = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(privilege, env_offset_privilege());
    li_d(tmp, LATR_RISCV_PRIV_U);
    la_beq(privilege, tmp, illegal);
    li_d(tmp, 2);
    la_beq(privilege, tmp, illegal);
    li_d(tmp, LATR_RISCV_PRIV_S);
    la_bne(privilege, tmp, sret_allowed);
    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(tmp, status, 22, 22);
    la_bne(tmp, zero_ir2_opnd, illegal);
    la_label(sret_allowed);

    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(new_privilege, status, 8, 8);
    la_bstrpick_d(tmp, status, 5, 5);
    la_bstrins_d(status, tmp, 1, 1);
    li_d(tmp, 1);
    la_bstrins_d(status, tmp, 5, 5);
    la_bstrins_d(status, zero_ir2_opnd, 8, 8);
    latr_env_store_d(status, env_offset_csr(CSR_MSTATUS));
    latr_env_store_d(new_privilege, env_offset_privilege());
    latr_env_load_d(target, env_offset_csr(CSR_SEPC));
    la_bstrins_d(target, zero_ir2_opnd, 0, 0);
    la_st_d(target, env_ir2_opnd, env_offset_pc());
    latr_emit_instret();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);

    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    ra_free_temp(target);
    ra_free_temp(tmp);
    ra_free_temp(new_privilege);
    ra_free_temp(status);
    ra_free_temp(privilege);
}

static void translate_wfi(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND valid = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND privilege = ra_alloc_itemp();
    IR2_OPND status = ra_alloc_itemp();
    IR2_OPND tmp = ra_alloc_itemp();

    latr_env_load_d(privilege, env_offset_privilege());
    li_d(tmp, 2);
    la_beq(privilege, tmp, illegal);
    li_d(tmp, LATR_RISCV_PRIV_M);
    la_beq(privilege, tmp, valid);
    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(status, status, 21, 21);
    la_bne(status, zero_ir2_opnd, illegal);
    la_label(valid);
    /* WFI is permitted to resume immediately; phase 3 supplies wakeups. */
    latr_emit_instret();
    la_b(done);

    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(done);
    ra_free_temp(tmp);
    ra_free_temp(status);
    ra_free_temp(privilege);
}

static void translate_sfence_vma(const RiscvIR1 *ir1)
{
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND privilege = ra_alloc_itemp();
    IR2_OPND status = ra_alloc_itemp();
    IR2_OPND tmp = ra_alloc_itemp();
    IR2_OPND supervisor = ir2_opnd_new_type(IR2_OPND_LABEL);

    latr_env_load_d(privilege, env_offset_privilege());
    li_d(tmp, LATR_RISCV_PRIV_U);
    la_beq(privilege, tmp, illegal);
    li_d(tmp, 2);
    la_beq(privilege, tmp, illegal);
    li_d(tmp, LATR_RISCV_PRIV_S);
    la_bne(privilege, tmp, supervisor);
    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    la_bstrpick_d(status, status, 20, 20);
    la_bne(status, zero_ir2_opnd, illegal);
    la_label(supervisor);
    /* Phase 2 adds a TLB; there is no translation cache to fence yet. */
    latr_emit_instret();
    la_b(done);

    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(done);
    ra_free_temp(tmp);
    ra_free_temp(status);
    ra_free_temp(privilege);
}

static void translate_cbo(const RiscvIR1 *ir1)
{
    bool invalidate = ir1->opcode == LATR_RV_CBO_INVAL;
    IR2_OPND illegal = ir2_opnd_new_type(IR2_OPND_LABEL);
    IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

    emit_cbo_permission_check(illegal, invalidate,
                              ir1->opcode == LATR_RV_CBO_ZERO ? 7 : 6);
    if (ir1->opcode == LATR_RV_CBO_ZERO) {
        IR2_OPND base = alloc_gpr_src(ir1->arg.rs1);
        IR2_OPND aligned = ra_alloc_itemp();
        IR2_OPND mask = ra_alloc_itemp();

        la_dbar(0);
        li_d(mask, (int64_t)~UINT64_C(63));
        la_and(aligned, base, mask);
        for (int offset = 0; offset < 64; offset += 8) {
            latr_emit_store(aligned, zero_ir2_opnd, 8);
            la_addi_d(aligned, aligned, 8);
        }
        la_dbar(0);
        ra_free_temp(mask);
        ra_free_temp(aligned);
        free_alloc_gpr(base);
    } else {
        /* There are no data caches in this memory model. */
        la_dbar(0);
    }
    latr_emit_instret();
    la_b(done);

    la_label(illegal);
    latr_raise_illegal_instruction(ir1);
    la_label(done);
}

bool latr_translate_privileged(const RiscvIR1 *ir1)
{
    if (ir1 == NULL) {
        return false;
    }

    switch (ir1->opcode) {
    case LATR_RV_MRET:
        translate_mret(ir1);
        return true;
    case LATR_RV_SRET:
        translate_sret(ir1);
        return true;
    case LATR_RV_WFI:
        translate_wfi(ir1);
        return true;
    case LATR_RV_SFENCE_VMA:
        translate_sfence_vma(ir1);
        return true;
    case LATR_RV_PAUSE:
        latr_emit_instret();
        return true;
    case LATR_RV_CBO_CLEAN:
    case LATR_RV_CBO_FLUSH:
    case LATR_RV_CBO_INVAL:
    case LATR_RV_CBO_ZERO:
        translate_cbo(ir1);
        return true;
    case LATR_RV_FENCE_I:
        la_dbar(0);
        {
            IR2_OPND value = ra_alloc_itemp();
            li_d(value, (int64_t)(ir1->pc + ir1->raw_len));
            la_st_d(value, env_ir2_opnd, env_offset_pc());
            li_d(value, 1);
            latr_env_store_d(value, env_offset_fence_i_pending());
            ra_free_temp(value);
        }
        latr_emit_instret();
        /* The owning runtime consumes the invalidation request after exit. */
        la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
        return true;
    default:
        return false;
    }
}

bool latr_translate_csr(const RiscvIR1 *ir1)
{
    if (ir1 == NULL) {
        return false;
    }

    switch (ir1->opcode) {
    case LATR_RV_CSRRW:
    case LATR_RV_CSRRS:
    case LATR_RV_CSRRC:
    case LATR_RV_CSRRWI:
    case LATR_RV_CSRRSI:
    case LATR_RV_CSRRCI:
        return translate_csr(ir1);
    case LATR_RV_ECALL: {
        IR2_OPND cause = ra_alloc_itemp();
        IR2_OPND privilege = ra_alloc_itemp();
        latr_env_load_d(privilege, env_offset_privilege());
        la_addi_d(cause, privilege, 8);
        transfer_exception(cause, ir1->pc, zero_ir2_opnd);
        ra_free_temp(privilege);
        ra_free_temp(cause);
        return true;
    }
    case LATR_RV_EBREAK: {
        IR2_OPND cause = ra_alloc_itemp();
        IR2_OPND trap_value = ra_alloc_itemp();
        li_d(cause, 3);
        li_d(trap_value, (int64_t)ir1->pc);
        transfer_exception(cause, ir1->pc, trap_value);
        ra_free_temp(trap_value);
        ra_free_temp(cause);
        return true;
    }
    default:
        return false;
    }
}
