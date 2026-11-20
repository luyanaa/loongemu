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
#include "latm.h"

/* QEMU's generic debug/HALT exception numbers are host-side control
 * signals, not MIPS Cause.ExcCode values.  LATM uses the MIPS break and
 * interrupt codes for the freestanding harness. */
#undef EXCP_DEBUG
#undef EXCP_HLT
#undef EXCP_SYSCALL
#undef EXCP_INTERRUPT
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

static int mips_cause_code(uint32_t exception)
{
    switch (exception) {
    case EXCP_EXT_INTERRUPT: return 0;
    case EXCP_LTLBL:         return 1;
    case EXCP_TLBL:
    case EXCP_TLBF:          return 2;
    case EXCP_TLBS:          return 3;
    case EXCP_AdEL:          return 4;
    case EXCP_AdES:          return 5;
    case EXCP_IBE:           return 6;
    case EXCP_DBE:           return 7;
    case EXCP_SYSCALL:       return 8;
    case EXCP_BREAK:         return 9;
    case EXCP_RI:            return 10;
    case EXCP_CpU:           return 11;
    case EXCP_OVERFLOW:      return 12;
    case EXCP_TRAP:          return 13;
    case EXCP_MSAFPE:        return 14;
    case EXCP_FPE:           return 15;
    case EXCP_C2E:           return 18;
    case EXCP_TLBRI:         return 19;
    case EXCP_TLBXI:         return 20;
    case EXCP_MSADIS:        return 21;
    case EXCP_MDMX:          return 22;
    case EXCP_DWATCH:        return 23;
    case EXCP_MCHECK:        return 24;
    case EXCP_THREAD:        return 25;
    case EXCP_DSPDIS:        return 26;
    case EXCP_CACHE:         return 30;
    default:                 return 0;
    }
}

void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc)
{
    target_ulong epc = exception_resume_pc(env);
    bool was_exl = (env->CP0_Status & (1u << CP0St_EXL)) != 0;
    bool in_delay = (env->hflags & MIPS_HFLAG_BMASK) != 0;
    int cause = mips_cause_code(exception);
    uint64_t guest_pc;
    uint8_t delay_slot;

    if (pc && latm_tb_insn_map_resolve(env->latm_tr_data, pc,
                                       &guest_pc, &delay_slot)) {
        epc = guest_pc;
        in_delay = delay_slot != 0;
        if (in_delay) {
            epc -= 4;
        }
    }

    env->llvalid = 0;
    env->lladdr = UINT64_MAX;
    env->error_code = error_code;
    env->CP0_Cause = (env->CP0_Cause & ~(0x1f << 2)) |
                     ((uint32_t)cause << 2);
    env->CP0_Status |= 1u << CP0St_EXL;
    if (!was_exl) {
        env->CP0_EPC = epc;
        if (in_delay) {
            env->CP0_Cause |= 1u << 31;
        } else {
            env->CP0_Cause &= ~(1u << 31);
        }
    }
    /* A nested exception keeps the outer handler's EPC/BD state. */

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
        if (!(env->CP0_Status & (1u << CP0St_BEV)) &&
            (env->CP0_EBase & ~UINT64_C(0xfff))) {
            base = env->CP0_EBase & ~UINT64_C(0xfff);
        }
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

void latm_check_interrupts(CPUMIPSState *env)
{
    (void)cpu_mips_get_count(env);
    if ((env->CP0_Status & (1u << CP0St_IE)) &&
        !(env->CP0_Status & (1u << CP0St_EXL)) &&
        !(env->CP0_Status & (1u << CP0St_ERL)) &&
        ((env->CP0_Cause & env->CP0_Status) & 0xff00u)) {
        env->active_tc.CP0_TCHalt = 0;
        do_raise_exception_err(env, EXCP_EXT_INTERRUPT, 0, 0);
    }
}

void helper_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                                int error_code)
{
    do_raise_exception_err(env, exception, error_code, GETPC());
}

void helper_raise_exception(CPUMIPSState *env, uint32_t exception)
{
    do_raise_exception_err(env, exception, 0, GETPC());
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
