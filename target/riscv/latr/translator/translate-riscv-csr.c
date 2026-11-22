#include "latr.h"
#include "translate.h"
#include "loongemu/vclock.h"
#include "mmu.h"

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
#define CSR_SATP         0x180
#define CSR_VSTART       0x008
#define CSR_VXSAT        0x009
#define CSR_VXRM         0x00a
#define CSR_VCSR         0x00f
#define CSR_VL           0xc20
#define CSR_VTYPE        0xc21
#define CSR_VLENB        0xc22
#define CSR_MSTATUS      0x300
#define CSR_MISA         0x301
#define CSR_MEDELEG      0x302
#define CSR_MIDELEG      0x303
#define CSR_MIE          0x304
#define CSR_MTVEC        0x305
#define CSR_MCOUNTEREN   0x306
#define CSR_MENVCFG      0x30a
#define CSR_MSECCFG      0x747
#define CSR_MCOUNTINHIBIT 0x320
#define CSR_MSTATEEN0    0x30c
#define CSR_MSTATEEN1    0x30d
#define CSR_MSTATEEN2    0x30e
#define CSR_MSTATEEN3    0x30f
#define CSR_MHPMEVENT3   0x323
#define CSR_MHPMEVENT31  0x33f
#define CSR_MHPMCOUNTER3 0xb03
#define CSR_MHPMCOUNTER31 0xb1f
#define CSR_HPMCOUNTER3  0xc03
#define CSR_HPMCOUNTER31 0xc1f
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

/* PMP: RV64 has pmpcfg0/pmpcfg2 and pmpaddr0..15; pmpcfg1/3 do not exist. */
#define CSR_PMPCFG0      0x3a0
#define CSR_PMPCFG2      0x3a2
#define CSR_PMPADDR0     0x3b0
#define CSR_PMPADDR_LAST 0x3bf

#define CSR_USER_CUSTOM_FIRST 0x800
#define CSR_USER_CUSTOM_LAST  0x8ff

#define MSTATUS_SIE   (UINT64_C(1) << 1)
#define MSTATUS_MIE   (UINT64_C(1) << 3)
#define MSTATUS_SPIE  (UINT64_C(1) << 5)
#define MSTATUS_MPIE  (UINT64_C(1) << 7)
#define MSTATUS_SPP   (UINT64_C(1) << 8)
#define MSTATUS_MPP   (UINT64_C(3) << 11)
#define MSTATUS_VS    (UINT64_C(3) << 9)
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
                            MSTATUS_VS | MSTATUS_FS | MSTATUS_SUM | \
                            MSTATUS_MXR)
#define MSTATUS_WRITE_MASK (SSTATUS_WRITE_MASK | MSTATUS_MIE | MSTATUS_MPIE | \
                            MSTATUS_MPP | MSTATUS_MPRV | MSTATUS_TVM | \
                            MSTATUS_TW | MSTATUS_TSR)
#define MIE_WRITABLE_MASK (UINT64_C(0xaaa))
#define MIP_WRITABLE_MASK ((UINT64_C(1) << 1) | (UINT64_C(1) << 3))
#define MIDELEG_WRITABLE_MASK UINT64_C(0x222)
#define MEDELEG_WRITABLE_MASK UINT64_C(0xb3ff)
#define ENVCFG_WRITABLE_MASK ((UINT64_C(1) << 63) | (UINT64_C(1) << 62) | \
                              (UINT64_C(1) << 61) | UINT64_C(0xf0))
#define SENVCFG_WRITABLE_MASK UINT64_C(0xf0)
#define MISA_RV64GC (UINT64_C(0x800000000034112d)) /* S + U + V */
#define CSR_MVENDORID 0xf11
#define CSR_MARCHID   0xf12
#define CSR_MIMPID    0xf13
#define CSR_MHARTID   0xf14
#define CSR_MCONFIGPTR 0xf15

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
    LATR_CSR_HPMCOUNTER,
    LATR_CSR_VSTART,
    LATR_CSR_VXSAT,
    LATR_CSR_VXRM,
    LATR_CSR_VCSR,
    LATR_CSR_VL,
    LATR_CSR_VTYPE,
    LATR_CSR_VLENB,
    LATR_CSR_MHARTID,
    LATR_CSR_MZERO,
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
    case CSR_SATP:
    case CSR_MSTATUS:
    case CSR_MEDELEG:
    case CSR_MIDELEG:
    case CSR_MIE:
    case CSR_MTVEC:
    case CSR_MCOUNTEREN:
    case CSR_MENVCFG:
    case CSR_MSECCFG:
    case CSR_MCOUNTINHIBIT:
    case CSR_MSCRATCH:
    case CSR_MEPC:
    case CSR_MCAUSE:
    case CSR_MTVAL:
    case CSR_MCYCLE:
    case CSR_MINSTRET:
        *kind = LATR_CSR_STORAGE;
        return true;
    case CSR_VSTART: *kind = LATR_CSR_VSTART; return true;
    case CSR_VXSAT:  *kind = LATR_CSR_VXSAT; return true;
    case CSR_VXRM:   *kind = LATR_CSR_VXRM; return true;
    case CSR_VCSR:   *kind = LATR_CSR_VCSR; return true;
    case CSR_VL:     *kind = LATR_CSR_VL; return true;
    case CSR_VTYPE:  *kind = LATR_CSR_VTYPE; return true;
    case CSR_VLENB:  *kind = LATR_CSR_VLENB; return true;
    case CSR_HPMCOUNTER3 ... CSR_HPMCOUNTER31:
        *kind = LATR_CSR_HPMCOUNTER;
        return true;
    case CSR_MHARTID: *kind = LATR_CSR_MHARTID; return true;
    case CSR_MVENDORID:
    case CSR_MARCHID:
    case CSR_MIMPID:
    case CSR_MCONFIGPTR:
        *kind = LATR_CSR_MZERO;
        return true;
    default:
        if ((csr >= CSR_MHPMEVENT3 && csr <= CSR_MHPMEVENT31) ||
            (csr >= CSR_MHPMCOUNTER3 && csr <= CSR_MHPMCOUNTER31) ||
            (csr >= CSR_MSTATEEN0 && csr <= CSR_MSTATEEN3)) {
            *kind = LATR_CSR_STORAGE;
            return true;
        }
        if (csr >= CSR_PMPADDR0 && csr <= CSR_PMPADDR_LAST) {
            *kind = LATR_CSR_STORAGE;
            return true;
        }
        if (csr == CSR_PMPCFG0 || csr == CSR_PMPCFG2) {
            *kind = LATR_CSR_STORAGE;
            return true;
        }
        /* PMP has no placeholder storage; other unknown CSRs trap illegal. */
        if (csr >= CSR_USER_CUSTOM_FIRST && csr <= CSR_USER_CUSTOM_LAST) {
            *kind = LATR_CSR_USER_CUSTOM;
            return true;
        }
        return false;
    }
}

static bool csr_is_vector(int csr)
{
    return csr == CSR_VSTART || csr == CSR_VXSAT || csr == CSR_VXRM ||
           csr == CSR_VCSR || csr == CSR_VL || csr == CSR_VTYPE ||
           csr == CSR_VLENB;
}

static bool csr_is_read_only(int csr)
{
    return (csr & 0xc00) == 0xc00 || csr == CSR_MISA;
}

static bool csr_is_counter(int csr)
{
    return csr == CSR_CYCLE || csr == CSR_TIME || csr == CSR_INSTRET ||
           (csr >= CSR_HPMCOUNTER3 && csr <= CSR_HPMCOUNTER31);
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

/* Resolves a guest VA through the page walker. After the call a0 holds the
 * host address (0 on fault, recorded in env), and every itemp register has
 * been clobbered: callers must not keep temps live across this sequence. */
void latr_emit_resolve(IR2_OPND address, unsigned access, unsigned size)
{
    IR2_OPND target = ra_alloc_itemp();

    latr_env_store_d(address, env_offset_mem_va());
    la_mov64(a0_ir2_opnd, env_ir2_opnd);
    la_mov64(a1_ir2_opnd, address);
    li_d(a2_ir2_opnd, access);
    li_d(a3_ir2_opnd, size);
    latr_env_store_d(ra_ir2_opnd, env_offset_host_return_address());
    li_d(target, (int64_t)(uintptr_t)&latr_resolve_guest_address);
    la_jirl(ra_ir2_opnd, target, 0);
    ra_free_temp(target);
    latr_env_load_d(ra_ir2_opnd, env_offset_host_return_address());
}

/* Branches to `fault` when the last resolve failed (host address 0 may be a
 * legitimate physical page, so the fault flag is authoritative). */
void latr_emit_branch_on_fault(IR2_OPND fault)
{
    IR2_OPND cause = ra_alloc_itemp();

    latr_env_load_d(cause, env_offset_mem_fault_cause());
    la_bne(cause, zero_ir2_opnd, fault);
    ra_free_temp(cause);
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
        emit_runtime_call((uintptr_t)latr_read_mip);
        la_mov64(dst, a0_ir2_opnd);
        tmp = ra_alloc_itemp();
        latr_env_load_d(tmp, env_offset_csr(CSR_MIDELEG));
        la_and(dst, dst, tmp);
        ra_free_temp(tmp);
        break;
    case LATR_CSR_MIP:
        emit_runtime_call((uintptr_t)latr_read_mip);
        la_mov64(dst, a0_ir2_opnd);
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
    case LATR_CSR_VSTART:
        latr_env_load_d(dst, env_offset_vstart());
        break;
    case LATR_CSR_VXSAT:
        latr_env_load_d(dst, env_offset_vxsat());
        break;
    case LATR_CSR_VXRM:
        latr_env_load_d(dst, env_offset_vxrm());
        break;
    case LATR_CSR_VCSR:
        latr_env_load_d(dst, env_offset_vxrm());
        la_slli_d(dst, dst, 1);
        tmp = ra_alloc_itemp();
        latr_env_load_d(tmp, env_offset_vxsat());
        la_or(dst, dst, tmp);
        ra_free_temp(tmp);
        break;
    case LATR_CSR_VL:
        latr_env_load_d(dst, env_offset_vl());
        break;
    case LATR_CSR_VTYPE:
        latr_env_load_d(dst, env_offset_vtype());
        tmp = ra_alloc_itemp();
        latr_env_load_d(tmp, env_offset_vill());
        la_slli_d(tmp, tmp, 63);
        la_or(dst, dst, tmp);
        ra_free_temp(tmp);
        break;
    case LATR_CSR_VLENB:
        li_d(dst, LATR_RVV_VLENB);
        break;
    case LATR_CSR_HPMCOUNTER:
        /* User/supervisor view aliases the machine counter. */
        latr_env_load_d(dst, env_offset_csr(csr - 0x100));
        break;
    case LATR_CSR_MHARTID:
        latr_env_load_d(dst, env_offset_hartid());
        break;
    case LATR_CSR_MZERO:
        li_d(dst, 0);
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

static void emit_mark_fs_dirty(void)
{
    IR2_OPND status = ra_alloc_itemp();
    IR2_OPND bits = ra_alloc_itemp();

    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    li_d(bits, (int64_t)MSTATUS_FS);
    la_or(status, status, bits);
    latr_env_store_d(status, env_offset_csr(CSR_MSTATUS));
    ra_free_temp(bits);
    ra_free_temp(status);
}

static void emit_mark_vs_dirty(void)
{
    IR2_OPND status = ra_alloc_itemp();
    IR2_OPND bits = ra_alloc_itemp();

    latr_env_load_d(status, env_offset_csr(CSR_MSTATUS));
    li_d(bits, (int64_t)MSTATUS_VS);
    la_or(status, status, bits);
    latr_env_store_d(status, env_offset_csr(CSR_MSTATUS));
    ra_free_temp(bits);
    ra_free_temp(status);
}

void latr_emit_mark_fs_dirty(void)
{
    emit_mark_fs_dirty();
}

void latr_emit_mark_vs_dirty(void)
{
    emit_mark_vs_dirty();
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
        emit_mark_fs_dirty();
        break;
    case LATR_CSR_FRM:
        state = ra_alloc_itemp();
        la_ld_d(state, env_ir2_opnd, env_offset_fcsr());
        la_bstrpick_d(state, state, 7, 0);
        la_bstrins_d(state, value, 7, 5);
        la_st_d(state, env_ir2_opnd, env_offset_fcsr());
        ra_free_temp(state);
        emit_mark_fs_dirty();
        break;
    case LATR_CSR_FCSR:
        state = ra_alloc_itemp();
        la_bstrpick_d(state, value, 7, 0);
        la_st_d(state, env_ir2_opnd, env_offset_fcsr());
        ra_free_temp(state);
        emit_mark_fs_dirty();
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
    case LATR_CSR_VSTART:
        state = ra_alloc_itemp();
        mask = ra_alloc_itemp();
        li_d(mask, (int64_t)LATR_RVV_VSTART_MASK);
        la_and(state, value, mask);
        latr_env_store_d(state, env_offset_vstart());
        ra_free_temp(mask);
        ra_free_temp(state);
        emit_mark_vs_dirty();
        break;
    case LATR_CSR_VXSAT:
        latr_env_store_d(value, env_offset_vxsat());
        emit_mark_vs_dirty();
        break;
    case LATR_CSR_VXRM:
        latr_env_store_d(value, env_offset_vxrm());
        emit_mark_vs_dirty();
        break;
    case LATR_CSR_VCSR:
        state = ra_alloc_itemp();
        mask = ra_alloc_itemp();
        la_srli_d(state, value, 1);
        li_d(mask, 3);
        la_and(state, state, mask);
        latr_env_store_d(state, env_offset_vxrm());
        li_d(mask, 1);
        la_and(state, value, mask);
        latr_env_store_d(state, env_offset_vxsat());
        ra_free_temp(mask);
        ra_free_temp(state);
        emit_mark_vs_dirty();
        break;
    case LATR_CSR_VL:
    case LATR_CSR_VTYPE:
    case LATR_CSR_VLENB:
    case LATR_CSR_HPMCOUNTER:
    case LATR_CSR_MHARTID:
    case LATR_CSR_MZERO:
        /* Read-only; the write-intent check already traps. */
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
        case CSR_SATP: {
            /* QEMU's legalize_xatp(): accept the write only when MODE is a
             * supported translation mode (Bare/Sv39/Sv48/Sv57); otherwise
             * keep the previous value. There is no TLB to flush yet. */
            IR2_OPND mode = ra_alloc_itemp();
            IR2_OPND supported = ra_alloc_itemp();
            IR2_OPND accept = ir2_opnd_new_type(IR2_OPND_LABEL);
            IR2_OPND done = ir2_opnd_new_type(IR2_OPND_LABEL);

            la_srli_d(mode, value, 60);
            la_beqz(mode, accept);
            li_d(supported, 8);
            la_beq(mode, supported, accept);
            li_d(supported, 9);
            la_beq(mode, supported, accept);
            li_d(supported, 10);
            la_beq(mode, supported, accept);
            la_b(done);
            la_label(accept);
            latr_env_store_d(value, env_offset_csr(csr));
            la_label(done);
            ra_free_temp(supported);
            ra_free_temp(mode);
            break;
        }
        case CSR_PMPCFG0:
        case CSR_PMPCFG2:
            /* Per-byte WARL: a locked byte (L=1) is immutable; an entry
             * with W=1,R=0 is reserved while mseccfg.MML=0. Keep the old
             * byte in both cases. */
            state = ra_alloc_itemp();
            field = ra_alloc_itemp();
            mask = ra_alloc_itemp();
            latr_env_load_d(state, env_offset_csr(csr));
            li_d(mask, 2);
            for (int i = 0; i < 8; i++) {
                IR2_OPND keep_byte = ir2_opnd_new_type(IR2_OPND_LABEL);
                la_bstrpick_d(field, state, 7 + 8 * i, 8 * i);
                la_andi(field, field, 0x80);
                la_bnez(field, keep_byte);
                la_bstrpick_d(field, value, 7 + 8 * i, 8 * i);
                la_andi(field, field, 3);
                la_beq(field, mask, keep_byte);
                la_bstrpick_d(field, value, 7 + 8 * i, 8 * i);
                la_bstrins_d(state, field, 7 + 8 * i, 8 * i);
                la_label(keep_byte);
            }
            latr_env_store_d(state, env_offset_csr(csr));
            ra_free_temp(mask);
            ra_free_temp(field);
            ra_free_temp(state);
            break;
        case CSR_PMPADDR0 ... CSR_PMPADDR_LAST: {
            unsigned pmp_index = (unsigned)(csr - CSR_PMPADDR0);
            int cfg_csr = pmp_index < 8 ? CSR_PMPCFG0 : CSR_PMPCFG2;
            unsigned shift = 8 * (pmp_index & 7u);
            IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);
            IR2_OPND masked = ra_alloc_itemp();

            state = ra_alloc_itemp();
            field = ra_alloc_itemp();
            mask = ra_alloc_itemp();
            latr_env_load_d(state, env_offset_csr(cfg_csr));
            la_bstrpick_d(field, state, (int)shift + 7, (int)shift);
            la_andi(field, field, 0x80);
            la_bnez(field, skip);
            li_d(mask, (int64_t)((UINT64_C(1) << 54) - 1));
            la_and(masked, value, mask);
            latr_env_store_d(masked, env_offset_csr(csr));
            la_label(skip);
            ra_free_temp(mask);
            ra_free_temp(masked);
            ra_free_temp(field);
            ra_free_temp(state);
            break;
        }
        case CSR_MSECCFG:
            store_masked_csr(value, csr, 3);
            break;
        case CSR_MSTATEEN0:
            /* STATEEN, HSENVCFG and FCSR gating bits are modelled as
             * storage; unsupported extension bits read as zero. */
            store_masked_csr(value, csr,
                             (UINT64_C(1) << 63) | (UINT64_C(1) << 62) |
                             (UINT64_C(1) << 1));
            break;
        case CSR_MSTATEEN1:
        case CSR_MSTATEEN2:
        case CSR_MSTATEEN3:
            store_masked_csr(value, csr, UINT64_C(1) << 63);
            break;
        case CSR_MIE:
            store_masked_csr(value, csr, MIE_WRITABLE_MASK);
            break;
        case CSR_MCOUNTEREN:
        case CSR_SCOUNTEREN:
            store_masked_csr(value, csr, UINT64_C(0xffffffff));
            break;
        case CSR_MCOUNTINHIBIT:
            /* TIME (bit 1) is not implemented; all other counter inhibit
             * bits are storage until the event backend exists. */
            store_masked_csr(value, csr, UINT64_C(0xfffffffd));
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
        case CSR_MIDELEG:
            store_masked_csr(value, csr, MIDELEG_WRITABLE_MASK);
            break;
        case CSR_MEDELEG:
            store_masked_csr(value, csr, MEDELEG_WRITABLE_MASK);
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
    /* Any trap invalidates an LR reservation (context switches, signals and
     * the trap handler itself must not inherit it). The field sits past
     * csr[4096], so it needs the large-offset store helper. */
    latr_env_store_d(zero_ir2_opnd, env_offset_reservation_valid());
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

/* Causes 4 and 6: load / store-AMO address misaligned, with mtval = address. */
void latr_raise_misaligned(const RiscvIR1 *ir1, IR2_OPND address, bool is_store)
{
    IR2_OPND cause = a0_ir2_opnd;
    IR2_OPND trap_value = a1_ir2_opnd;

    la_mov64(trap_value, address);
    li_d(cause, is_store ? 6 : 4);
    transfer_exception(cause, ir1->pc, trap_value);
}

/* Instruction-fetch page fault: the translator could not fetch the bytes. */
void latr_raise_fetch_fault(const RiscvIR1 *ir1)
{
    IR2_OPND cause = a0_ir2_opnd;
    IR2_OPND trap_value = a1_ir2_opnd;

    li_d(cause, ir1->fault_cause > 0 ? ir1->fault_cause
                                     : LATR_EXCP_INST_PAGE_FAULT);
    li_d(trap_value, (int64_t)ir1->fault_va);
    transfer_exception(cause, ir1->pc, trap_value);
}

/* Page fault recorded by latr_resolve_guest_address(); mtval = guest VA. */
void latr_raise_mem_fault(const RiscvIR1 *ir1)
{
    IR2_OPND cause = a0_ir2_opnd;
    IR2_OPND trap_value = a1_ir2_opnd;

    latr_env_load_d(cause, env_offset_mem_fault_cause());
    latr_env_load_d(trap_value, env_offset_mem_va());
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

    if (csr == CSR_FFLAGS || csr == CSR_FRM || csr == CSR_FCSR) {
        /* QEMU require_fp: FP CSRs are illegal while FS == Off. */
        latr_env_load_d(value, env_offset_csr(CSR_MSTATUS));
        la_bstrpick_d(value, value, 14, 13);
        la_beq(value, zero_ir2_opnd, illegal);
    }

    if (csr_is_vector(csr)) {
        /* QEMU's `vs` predicate: vector CSRs are illegal while VS is Off. */
        latr_env_load_d(value, env_offset_csr(CSR_MSTATUS));
        li_d(mask, (int64_t)MSTATUS_VS);
        la_and(value, value, mask);
        la_beq(value, zero_ir2_opnd, illegal);
    }

    if (csr == CSR_SATP) {
        /* mstatus.TVM traps S-mode satp access; M-mode is unaffected. */
        IR2_OPND not_supervisor = ir2_opnd_new_type(IR2_OPND_LABEL);
        li_d(value, LATR_RISCV_PRIV_S);
        la_bne(privilege, value, not_supervisor);
        latr_env_load_d(value, env_offset_csr(CSR_MSTATUS));
        la_bstrpick_d(value, value, 20, 20);
        la_bne(value, zero_ir2_opnd, illegal);
        la_label(not_supervisor);
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
    int source_index = ir1->arg.rs1;
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
    /* SRET returns to S/U; MPRV must not leak into the new mode. */
    la_bstrins_d(status, zero_ir2_opnd, 17, 17);
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
    /* WFI resumes immediately here; the runtime sees wfi_pending and may
     * fast-forward the vclock until an interrupt is deliverable. */
    li_d(tmp, 1);
    latr_env_store_d(tmp, env_offset_wfi_pending());
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
    /* Record the shootdown request; the runtime invalidates TBs after exit.
     * rs2 == x0 means "all ASIDs"; a specific ASID invalidates that ASID's
     * TBs (a VA-specific invalidation is conservative: whole ASID). */
    {
        IR2_OPND request = ra_alloc_itemp();

        li_d(tmp, 1);
        latr_env_store_d(tmp, env_offset_sfence_pending());
        if (ir1->arg.rs2 == 0) {
            li_d(request, (int64_t)UINT32_MAX);
        } else {
            IR2_OPND asid = alloc_gpr_src(ir1->arg.rs2);
            la_bstrpick_d(request, asid, 15, 0);
            free_alloc_gpr(asid);
        }
        latr_env_store_d(request, env_offset_sfence_asid());
        ra_free_temp(request);
    }
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
        IR2_OPND fault = ir2_opnd_new_type(IR2_OPND_LABEL);
        IR2_OPND zero_ok = ir2_opnd_new_type(IR2_OPND_LABEL);

        la_dbar(0);
        li_d(mask, (int64_t)~UINT64_C(63));
        la_and(aligned, base, mask);
        free_alloc_gpr(base);
        /* A 64-byte aligned block never straddles a 4 KiB page. */
        latr_emit_resolve(aligned, LATR_ACCESS_STORE, 8);
        latr_emit_branch_on_fault(fault);
        {
            IR2_OPND mmio_flag = ra_alloc_itemp();
            IR2_OPND not_mmio = ir2_opnd_new_type(IR2_OPND_LABEL);
            latr_env_load_d(mmio_flag, env_offset_mem_mmio());
            la_beqz(mmio_flag, not_mmio);
            li_d(mmio_flag, LATR_EXCP_STORE_ACCESS_FAULT);
            latr_env_store_d(mmio_flag, env_offset_mem_fault_cause());
            la_b(fault);
            la_label(not_mmio);
            ra_free_temp(mmio_flag);
        }
        for (int offset = 0; offset < 64; offset += 8) {
            latr_emit_store(a0_ir2_opnd, zero_ir2_opnd, 8);
            la_addi_d(a0_ir2_opnd, a0_ir2_opnd, 8);
        }
        la_dbar(0);
        ra_free_temp(mask);
        ra_free_temp(aligned);
        la_b(zero_ok);
        la_label(fault);
        latr_raise_mem_fault(ir1);
        la_label(zero_ok);
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
