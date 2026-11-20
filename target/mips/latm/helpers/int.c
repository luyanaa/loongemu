/*
 * LATM MIPS64r2 integer helpers.
 *
 * Overflow, divide-by-zero, HI/LO merge and trap semantics follow
 * QEMU target/mips/tcg/translate.c (gen_arith, gen_muldiv, gen_trap).
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"
#include "latm-helpers.h"

static void raise_overflow(CPUMIPSState *env)
{
    do_raise_exception(env, EXCP_OVERFLOW, GETPC());
}

uint64_t latm_helper_arith(CPUMIPSState *env, uint32_t op,
                           uint64_t a, uint64_t b)
{
    switch (op) {
    case MIPS_ALU_ADD:
    case MIPS_IMM_ADDI: {
        int32_t x = (int32_t)a, y = (int32_t)b, r = x + y;
        if (((x ^ r) & (y ^ r)) < 0) {
            raise_overflow(env);
        }
        return (int64_t)r;
    }
    case MIPS_ALU_SUB: {
        int32_t x = (int32_t)a, y = (int32_t)b, r = x - y;
        if (((x ^ y) & (x ^ r)) < 0) {
            raise_overflow(env);
        }
        return (int64_t)r;
    }
    case MIPS_ALU_ADDU:
    case MIPS_IMM_ADDIU:
        return (int64_t)(int32_t)((uint32_t)a + (uint32_t)b);
    case MIPS_ALU_SUBU:
        return (int64_t)(int32_t)((uint32_t)a - (uint32_t)b);
    case MIPS_ALU_DADD:
    case MIPS_IMM_DADDI: {
        int64_t x = (int64_t)a, y = (int64_t)b, r = x + y;
        if (((x ^ r) & (y ^ r)) < 0) {
            raise_overflow(env);
        }
        return (uint64_t)r;
    }
    case MIPS_ALU_DSUB: {
        int64_t x = (int64_t)a, y = (int64_t)b, r = x - y;
        if (((x ^ y) & (x ^ r)) < 0) {
            raise_overflow(env);
        }
        return (uint64_t)r;
    }
    case MIPS_ALU_DADDU:
    case MIPS_IMM_DADDIU:
        return a + b;
    case MIPS_ALU_DSUBU:
        return a - b;
    default:
        return 0;
    }
}

static void set_hilo(CPUMIPSState *env, uint64_t lo, uint64_t hi)
{
    env->active_tc.LO[0] = lo;
    env->active_tc.HI[0] = hi;
}

void latm_helper_div(CPUMIPSState *env, uint32_t op, uint64_t a, uint64_t b)
{
    switch (op) {
    case MIPS_MD_DIV: {
        int32_t x = (int32_t)a, y = (int32_t)b;
        if (y == 0 || (x == INT32_MIN && y == -1)) {
            y = 1;
        }
        set_hilo(env, (int64_t)(x / y), (int64_t)(x % y));
        break;
    }
    case MIPS_MD_DIVU: {
        uint32_t x = (uint32_t)a, y = (uint32_t)b;
        if (y == 0) {
            y = 1;
        }
        set_hilo(env, (int64_t)(int32_t)(x / y), (int64_t)(int32_t)(x % y));
        break;
    }
    case MIPS_MD_DDIV: {
        int64_t x = (int64_t)a, y = (int64_t)b;
        if (y == 0 || (x == INT64_MIN && y == -1)) {
            y = 1;
        }
        set_hilo(env, (uint64_t)(x / y), (uint64_t)(x % y));
        break;
    }
    case MIPS_MD_DDIVU: {
        uint64_t x = a, y = b;
        if (y == 0) {
            y = 1;
        }
        set_hilo(env, x / y, x % y);
        break;
    }
    }
}

void latm_helper_madd(CPUMIPSState *env, uint32_t op, uint64_t a, uint64_t b)
{
    uint64_t prod;
    uint64_t acc = (env->active_tc.HI[0] << 32) |
                   (env->active_tc.LO[0] & UINT64_C(0xffffffff));
    bool sub = (op == MIPS_MD_MSUB || op == MIPS_MD_MSUBU);

    if (op == MIPS_MD_MADDU || op == MIPS_MD_MSUBU) {
        prod = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    } else {
        prod = (uint64_t)((int64_t)a * (int64_t)b);
    }
    acc = sub ? acc - prod : acc + prod;
    set_hilo(env, (int64_t)(int32_t)acc, (int64_t)acc >> 32);
}

uint64_t latm_helper_fcc(CPUMIPSState *env, uint32_t cc)
{
    return (env->active_fpu.fcr31 >> latm_fcc_bit(cc)) & 1;
}

void latm_helper_sync(CPUMIPSState *env, uint32_t stype)
{
    (void)env;
    (void)stype;
}

void latm_helper_synci(CPUMIPSState *env, uint64_t addr)
{
    extern void latm_tb_flush_range(void *cpu, uint64_t start, uint64_t end)
        __attribute__((weak));
    if (latm_tb_flush_range) {
        latm_tb_flush_range(env, addr, addr + 32);
    }
}

void latm_helper_cache(CPUMIPSState *env, uint32_t op, uint64_t addr)
{
    extern void latm_tb_flush_range(void *cpu, uint64_t start, uint64_t end)
        __attribute__((weak));
    /* Index/Hit Invalidate for the instruction cache. */
    if (latm_tb_flush_range && (op == 0x00 || op == 0x10)) {
        latm_tb_flush_range(env, addr & ~UINT64_C(0x1f),
                            (addr & ~UINT64_C(0x1f)) + 32);
    }
}

void latm_helper_pref(CPUMIPSState *env, uint32_t hint, uint64_t addr)
{
    (void)env;
    (void)hint;
    (void)addr;
}

uint64_t latm_helper_ll(CPUMIPSState *env, uint32_t op, uint64_t addr)
{
    uint64_t v, phys;
    unsigned size = op == MIPS_MEM_LL ? 4 : 8;

    if (addr & (size - 1)) {
        do_raise_exception(env, EXCP_AdEL, GETPC());
        return 0;
    }
    phys = cpu_mips_translate_address(env, addr, MMU_DATA_LOAD, 0);
    if (size == 4) {
        v = cpu_ldl_mmuidx_ra(env, addr, 0, 0);
        env->llval = v;
        env->llsize = 4;
    } else {
        v = cpu_ldq_mmuidx_ra(env, addr, 0, 0);
        env->llval = v;
        env->llsize = 8;
    }
    env->lladdr = phys;
    env->llpaddr = phys;
    env->llvalid = 1;
    return size == 4 ? (int64_t)(int32_t)v : v;
}

uint64_t latm_helper_sc(CPUMIPSState *env, uint32_t op, uint64_t addr,
                        uint64_t val)
{
    unsigned size = op == MIPS_MEM_SC ? 4 : 8;
    bool ok = false;

    if (addr & (size - 1)) {
        do_raise_exception(env, EXCP_AdES, GETPC());
        return 0;
    }
    {
        uint64_t phys = cpu_mips_translate_address(env, addr,
                                                   MMU_DATA_STORE, 0);
        if (env->llvalid && env->llsize == size &&
            phys == env->llpaddr) {
        if (size == 4) {
            uint32_t old = (uint32_t)env->llval;
            uint32_t new = (uint32_t)val;
            ok = __atomic_compare_exchange_n((uint32_t *)(uintptr_t)phys,
                                             &old, new, false,
                                             __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        } else {
            uint64_t old = env->llval;
            ok = __atomic_compare_exchange_n((uint64_t *)(uintptr_t)phys,
                                             &old, val, false,
                                             __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
    }
    env->llvalid = 0;
    env->lladdr = UINT64_MAX;
    env->llsize = 0;
    return ok;
}

uint64_t latm_helper_rdhwr(CPUMIPSState *env, uint32_t rd, uint32_t sel)
{
    (void)sel;
    switch (rd) {
    case 0:
        return env->CP0_EBase & 0x3ff;
    case 1:
        return env->SYNCI_Step;
    case 2:
        return (int32_t)env->CP0_Count;
    case 3:
        return env->CCRes;
    case 29:
        return env->active_tc.CP0_UserLocal;
    default:
        do_raise_exception(env, EXCP_RI, GETPC());
        return 0;
    }
}

void latm_helper_overflow(CPUMIPSState *env)
{
    raise_overflow(env);
}

void latm_helper_syscall(CPUMIPSState *env)
{
    do_raise_exception(env, EXCP_SYSCALL, GETPC());
}

void latm_helper_break(CPUMIPSState *env, uint32_t code)
{
    env->error_code = code;
    do_raise_exception(env, EXCP_BREAK, GETPC());
}

void latm_helper_trap(CPUMIPSState *env, uint32_t op, uint64_t a, uint64_t b,
                      uint32_t code)
{
    bool take = false;

    switch (op) {
    case MIPS_TRAP_TGE:  take = (int64_t)a >= (int64_t)b; break;
    case MIPS_TRAP_TGEU: take = a >= b; break;
    case MIPS_TRAP_TLT:  take = (int64_t)a <  (int64_t)b; break;
    case MIPS_TRAP_TLTU: take = a <  b; break;
    case MIPS_TRAP_TEQ:  take = a == b; break;
    case MIPS_TRAP_TNE:  take = a != b; break;
    }
    if (take) {
        env->error_code = code;
        do_raise_exception(env, EXCP_TRAP, GETPC());
    }
}
