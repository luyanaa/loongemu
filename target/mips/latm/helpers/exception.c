/*
 * LATM MIPS exception delivery.
 *
 * Semantics follow QEMU target/mips/tcg/exception.c.  The QOM/CPUState
 * plumbing is replaced with the freestanding state carried in CPUMIPSState.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "exec/helper-proto.h"

/* QEMU's generic debug/HALT exception numbers are host-side control
 * signals, not MIPS Cause.ExcCode values.  LATM uses the MIPS break and
 * interrupt codes for the freestanding harness. */
#define EXCP_DEBUG EXCP_BREAK
#define EXCP_HLT   EXCP_EXT_INTERRUPT

target_ulong exception_resume_pc(CPUMIPSState *env)
{
    target_ulong bad_pc;
    target_ulong isa_mode;

    isa_mode = !!(env->hflags & MIPS_HFLAG_M16);
    bad_pc = env->active_tc.PC | isa_mode;
    if (env->hflags & MIPS_HFLAG_BMASK) {
        bad_pc -= (env->hflags & MIPS_HFLAG_B16 ? 2 : 4);
    }
    return bad_pc;
}

void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc)
{
    target_ulong epc = exception_resume_pc(env);
    bool was_exl = (env->CP0_Status & (1u << CP0St_EXL)) != 0;

    env->llvalid = 0;
    env->lladdr = UINT64_MAX;
    env->error_code = error_code;
    env->CP0_Cause = (env->CP0_Cause & ~(0x1f << 2)) |
                     ((exception & 0x1f) << 2);
    env->CP0_EPC = epc;
    env->CP0_Status |= 1u << CP0St_EXL;
    if (env->hflags & MIPS_HFLAG_BMASK) {
        env->CP0_Cause |= 1u << 31;
    } else {
        env->CP0_Cause &= ~(1u << 31);
    }

    switch (exception) {
    case EXCP_AdEL:
    case EXCP_AdES:
    case EXCP_TLBL:
    case EXCP_TLBS:
    case EXCP_TLBXI:
    case EXCP_TLBRI:
        break;
    default:
        break;
    }

    {
        target_ulong base = (env->CP0_Status & (1u << CP0St_BEV)) ?
                            0xbfc00000u : env->exception_base;
        if (exception == EXCP_EXT_INTERRUPT &&
            (env->CP0_Cause & (1u << 23))) {
            env->active_tc.PC = base + 0x200;
        } else if ((exception == EXCP_TLBL || exception == EXCP_TLBS) &&
                   (error_code & EXCP_TLB_NOMATCH) && !was_exl) {
#if defined(TARGET_MIPS64)
            int r = env->CP0_BadVAddr >> 62;
            int ux = (env->CP0_Status & (1u << CP0St_UX)) != 0;
            int kx = (env->CP0_Status & (1u << CP0St_KX)) != 0;
            if ((r != 0 || ux) && (r != 3 || kx)) {
                env->active_tc.PC = base + 0x080;
            } else
#endif
            {
                env->active_tc.PC = base + 0x000;
            }
        } else {
            env->active_tc.PC = base + 0x180;
        }
    }

    (void)pc;
    cpu_loop_exit(NULL);
}

void do_raise_exception(CPUMIPSState *env, uint32_t exception, uintptr_t pc)
{
    do_raise_exception_err(env, exception, 0, pc);
}

void helper_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                                int error_code)
{
    do_raise_exception_err(env, exception, error_code, 0);
}

void helper_raise_exception(CPUMIPSState *env, uint32_t exception)
{
    do_raise_exception_err(env, exception, 0, 0);
}

void helper_raise_exception_debug(CPUMIPSState *env)
{
    do_raise_exception_err(env, EXCP_DEBUG, 0, 0);
}

void helper_wait(CPUMIPSState *env)
{
    /* The embedder may treat this as a halt request.  Keep the state
     * consistent with QEMU: PC has already been advanced by the caller. */
    do_raise_exception_err(env, EXCP_HLT, 0, 0);
}
