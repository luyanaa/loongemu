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
    do_raise_exception(env, EXCP_OVERFLOW, 0);
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
    return (env->active_fpu.fcr31 >> (23 + cc)) & 1;
}

void latm_helper_sync(CPUMIPSState *env, uint32_t stype)
{
    (void)env;
    (void)stype;
}

void latm_helper_synci(CPUMIPSState *env, uint64_t addr)
{
    extern void latm_tb_flush_range(uint64_t start, uint64_t end)
        __attribute__((weak));
    (void)env;
    if (latm_tb_flush_range) {
        latm_tb_flush_range(addr, addr + 32);
    }
}

void latm_helper_cache(CPUMIPSState *env, uint32_t op, uint64_t addr)
{
    (void)env;
    (void)op;
    (void)addr;
}

void latm_helper_pref(CPUMIPSState *env, uint32_t hint, uint64_t addr)
{
    (void)env;
    (void)hint;
    (void)addr;
}

uint64_t latm_helper_ll(CPUMIPSState *env, uint32_t op, uint64_t addr)
{
    uint64_t v;
    if (op == MIPS_MEM_LL) {
        v = cpu_ldl_mmuidx_ra(env, addr, 0, 0);
        env->lladdr = addr;
        env->llval = v;
        return (int64_t)(int32_t)v;
    }
    v = cpu_ldq_mmuidx_ra(env, addr, 0, 0);
    env->lladdr = addr;
    env->llval = v;
    return v;
}

uint64_t latm_helper_sc(CPUMIPSState *env, uint32_t op, uint64_t addr,
                        uint64_t val)
{
    bool ok;

    if (op == MIPS_MEM_SC) {
        ok = addr == env->lladdr &&
             (uint32_t)cpu_ldl_mmuidx_ra(env, addr, 0, 0) ==
             (uint32_t)env->llval;
        if (ok) {
            cpu_stl_mmuidx_ra(env, addr, (uint32_t)val, 0, 0);
        }
    } else {
        ok = addr == env->lladdr &&
             cpu_ldq_mmuidx_ra(env, addr, 0, 0) == env->llval;
        if (ok) {
            cpu_stq_mmuidx_ra(env, addr, val, 0, 0);
        }
    }
    env->lladdr = UINT64_MAX;
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
        do_raise_exception(env, EXCP_RI, 0);
        return 0;
    }
}

void latm_helper_overflow(CPUMIPSState *env)
{
    raise_overflow(env);
}

void latm_helper_syscall(CPUMIPSState *env)
{
    do_raise_exception(env, EXCP_SYSCALL, 0);
}

void latm_helper_break(CPUMIPSState *env, uint32_t code)
{
    env->error_code = code;
    do_raise_exception(env, EXCP_BREAK, 0);
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
        do_raise_exception(env, EXCP_TRAP, 0);
    }
}
