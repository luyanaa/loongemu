/*
 * Interrupt state maintenance and delivery for LATR.
 *
 * Sources wired here:
 *  - Sstc: MIP.STIP reflects (time >= stimecmp) against the shared vclock.
 *  - PLIC: MIP.SEIP reflects the hart's PLIC context having a claimable IRQ.
 *  - MSIP/MTIP/SSIP are driven by the VMM (mip writes) as usual.
 *
 * latr_deliver_interrupt() implements the privileged-spec trap selection
 * (priority order, mie/mip, mideleg, mstatus.MIE/SIE and privilege) and
 * latr_idle_advance() lets a runtime fast-forward the virtual clock while a
 * hart sits in WFI.
 */
#include "latr.h"
#include "translate.h"
#include "loongemu/vclock.h"
#include "loongemu/riscv_plic.h"

#define CSR_MSTATUS_ADDR 0x300
#define CSR_MIE_ADDR     0x304
#define CSR_MTVEC_ADDR   0x305
#define CSR_MEPC_ADDR    0x341
#define CSR_SCAUSE_ADDR  0x142
#define CSR_MCAUSE_ADDR  0x342
#define CSR_MIP_ADDR     0x344
#define CSR_STVEC_ADDR   0x105
#define CSR_SEPC_ADDR    0x141
#define CSR_STVAL_ADDR   0x143
#define CSR_MTVAL_ADDR   0x343
#define CSR_MIDELEG_ADDR 0x303
#define CSR_MEDELEG_ADDR 0x302
#define CSR_STIMECMP_ADDR LATR_RISCV_CSR_STIMECMP

#define MSTATUS_SIE  (UINT64_C(1) << 1)
#define MSTATUS_MIE  (UINT64_C(1) << 3)
#define MSTATUS_SPIE (UINT64_C(1) << 5)
#define MSTATUS_MPIE (UINT64_C(1) << 7)
#define MSTATUS_SPP  (UINT64_C(1) << 8)
#define MSTATUS_MPP  (UINT64_C(3) << 11)

#define MIP_SSIP (UINT64_C(1) << 1)
#define MIP_STIP (UINT64_C(1) << 5)
#define MIP_SEIP (UINT64_C(1) << 9)
#define MIP_MEIP (UINT64_C(1) << 11)

#define IRQ_BIT(cause) (UINT64_C(1) << (cause))

static void set_mip_bit(CPUArchState *env, unsigned bit, int level)
{
    uint64_t mip = env->csr[CSR_MIP_ADDR];

    if (level) {
        mip |= bit;
    } else {
        mip &= ~bit;
    }
    env->csr[CSR_MIP_ADDR] = mip;
}

/* Sstc: STIP is derived from the comparator, not stored by software. */
static void update_timer_interrupt(CPUArchState *env)
{
    uint64_t compare;

    if (env->vclock == NULL) {
        return;
    }
    compare = env->csr[CSR_STIMECMP_ADDR];
    uint64_t now = loongemu_vclock_now_ns(env->vclock);
    set_mip_bit(env, MIP_STIP, now >= compare);
}

static void update_external_interrupt(CPUArchState *env)
{
    int m_level = 0;
    int s_level = 0;

    if (env->plic != NULL) {
        /* QEMU virt convention: context 2*hart is M-mode and 2*hart+1 is
         * S-mode. plic_context holds the hart's M-mode context. */
        m_level = loongemu_plic_pending(env->plic,
                                        (uint32_t)env->plic_context);
        if (env->plic_context + 1 < env->plic->num_contexts) {
            s_level = loongemu_plic_pending(env->plic,
                                            (uint32_t)env->plic_context + 1);
        }
    }
    set_mip_bit(env, MIP_MEIP, m_level);
    set_mip_bit(env, MIP_SEIP, s_level);
}

void latr_update_interrupts(CPUArchState *env)
{
    update_timer_interrupt(env);
    update_external_interrupt(env);
}

/* True when an interrupt is pending and enabled, ignoring MIE/SIE. */
bool latr_interrupt_pending(CPUArchState *env)
{
    latr_update_interrupts(env);
    return (env->csr[CSR_MIP_ADDR] & env->csr[CSR_MIE_ADDR]) != 0;
}

uint64_t latr_read_mip(CPUArchState *env)
{
    latr_update_interrupts(env);
    return env->csr[CSR_MIP_ADDR];
}

bool latr_deliver_interrupt(CPUArchState *env)
{
    /* Spec priority order: MEI, MSI, MTI, SEI, SSI, STI. */
    static const unsigned order[] = { 11, 3, 7, 9, 1, 5 };
    uint64_t pending;
    uint64_t deleg;
    uint64_t status;
    unsigned privilege;

    latr_update_interrupts(env);
    pending = env->csr[CSR_MIP_ADDR] & env->csr[CSR_MIE_ADDR];
    if (pending == 0) {
        return false;
    }
    deleg = env->csr[CSR_MIDELEG_ADDR];
    status = env->csr[CSR_MSTATUS_ADDR];
    privilege = (unsigned)env->privilege;

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        unsigned cause = order[i];
        bool supervisor = (cause == 1 || cause == 5 || cause == 9);

        if ((pending & IRQ_BIT(cause)) == 0) {
            continue;
        }
        if (supervisor && (deleg & IRQ_BIT(cause))) {
            /* A delegated S interrupt is never taken in M-mode. U-mode is
             * lower than the S target, so sstatus.SIE does not gate it;
             * SIE is only consulted when the hart already runs in S-mode. */
            if (privilege == LATR_RISCV_PRIV_M ||
                (privilege == LATR_RISCV_PRIV_S &&
                 !(status & MSTATUS_SIE))) {
                continue;
            }
            env->csr[CSR_SEPC_ADDR] = env->pc;
            env->csr[CSR_SCAUSE_ADDR] = IRQ_BIT(63) | cause;
            env->exception = IRQ_BIT(63) | cause;
            status &= ~(MSTATUS_SPIE | MSTATUS_SPP);
            status |= (status & MSTATUS_SIE) ? MSTATUS_SPIE : 0;
            status |= (privilege == LATR_RISCV_PRIV_S) ? MSTATUS_SPP : 0;
            status &= ~MSTATUS_SIE;
            env->csr[CSR_MSTATUS_ADDR] = status;
            env->privilege = LATR_RISCV_PRIV_S;
            {
                uint64_t vec = env->csr[CSR_STVEC_ADDR];
                uint64_t base = vec & ~UINT64_C(3);
                if ((vec & 3) == 1) {
                    base += 4 * cause;
                }
                env->pc = base;
            }
            env->reservation_valid = 0;
            return true;
        }

        /* Machine-level interrupt. */
        if (!(privilege < LATR_RISCV_PRIV_M ||
              (privilege == LATR_RISCV_PRIV_M && (status & MSTATUS_MIE)))) {
            continue;
        }
        env->csr[CSR_MEPC_ADDR] = env->pc;
        env->csr[CSR_MCAUSE_ADDR] = IRQ_BIT(63) | cause;
        env->exception = IRQ_BIT(63) | cause;
        status &= ~(MSTATUS_MPIE | MSTATUS_MPP);
        status |= (status & MSTATUS_MIE) ? MSTATUS_MPIE : 0;
        status |= (uint64_t)(privilege & 3) << 11;
        status &= ~MSTATUS_MIE;
        env->csr[CSR_MSTATUS_ADDR] = status;
        env->privilege = LATR_RISCV_PRIV_M;
        {
            uint64_t vec = env->csr[CSR_MTVEC_ADDR];
            uint64_t base = vec & ~UINT64_C(3);
            if ((vec & 3) == 1) {
                base += 4 * cause;
            }
            env->pc = base;
        }
        env->reservation_valid = 0;
        return true;
    }
    return false;
}

/*
 * Idle fast-forward used while the hart is in WFI and nothing is deliverable:
 * jump to the next Sstc deadline, else advance a fixed slice so PLIC events
 * (injected by other harts/devices) get a chance to arrive.
 */
void latr_idle_advance(CPUArchState *env)
{
    uint64_t now;
    uint64_t next;
    uint64_t compare;

    if (env->vclock == NULL) {
        return;
    }
    now = loongemu_vclock_now_ns(env->vclock);
    compare = env->csr[CSR_STIMECMP_ADDR];
    if (compare > now) {
        next = compare;
    } else {
        next = now + UINT64_C(1000000); /* 1 ms slice */
    }
    loongemu_vclock_advance_ns(env->vclock, next - now);
}

/*
 * Runtime synchronous-exception entry, mirroring exactly what the generated
 * transfer_exception() emits for guest traps taken inside a block: M/S target
 * selection by privilege + medeleg, mstatus.{MPIE|MPP|SPIE|SPP|SIE|MIE},
 * {m,s}epc/cause/tval, the privilege change and the (non-vectored) trap-vector
 * PC. The one caller that needs it outside generated code is the native-fault
 * dispatcher: latr_machine_host_fault() resolves a host PC to the guest
 * instruction that caused it and hands that instruction to the guest's own
 * trap handler instead of silently resuming.
 *
 * Interrupt-form causes (bit 63) and vectored dispatch do not apply here:
 * vectored mode offsets only interrupt targets.
 */
void latr_take_guest_trap(CPUArchState *env, uint64_t cause, uint64_t tval)
{
    uint64_t status;
    uint64_t deleg;
    unsigned privilege;
    bool supervisor;

    privilege = (unsigned)env->privilege;
    deleg = env->csr[CSR_MEDELEG_ADDR];
    supervisor = privilege != LATR_RISCV_PRIV_M &&
                 (deleg & (UINT64_C(1) << (cause & 63))) != 0;

    status = env->csr[CSR_MSTATUS_ADDR];
    env->exception = cause;
    env->badaddr = tval;
    env->reservation_valid = 0;

    if (supervisor) {
        /* SPIE <- SIE, SIE <- 0, SPP <- previous privilege (bit 0). */
        status &= ~(MSTATUS_SPIE | MSTATUS_SPP);
        if ((status & MSTATUS_SIE) != 0) {
            status |= MSTATUS_SPIE;
        }
        status |= (uint64_t)(privilege & 1) << 8;
        status &= ~MSTATUS_SIE;
        env->csr[CSR_MSTATUS_ADDR] = status;
        env->csr[CSR_SEPC_ADDR] = env->pc & ~UINT64_C(1);
        env->csr[CSR_SCAUSE_ADDR] = cause;
        env->csr[CSR_STVAL_ADDR] = tval;
        env->privilege = LATR_RISCV_PRIV_S;
        env->pc = env->csr[CSR_STVEC_ADDR] & ~UINT64_C(3);
    } else {
        /* MPIE <- MIE, MIE <- 0, MPP <- previous privilege. */
        status &= ~(MSTATUS_MPIE | MSTATUS_MPP);
        if ((status & MSTATUS_MIE) != 0) {
            status |= MSTATUS_MPIE;
        }
        status |= (uint64_t)(privilege & 3) << 11;
        status &= ~MSTATUS_MIE;
        env->csr[CSR_MSTATUS_ADDR] = status;
        env->csr[CSR_MEPC_ADDR] = env->pc & ~UINT64_C(1);
        env->csr[CSR_MCAUSE_ADDR] = cause;
        env->csr[CSR_MTVAL_ADDR] = tval;
        env->privilege = LATR_RISCV_PRIV_M;
        env->pc = env->csr[CSR_MTVEC_ADDR] & ~UINT64_C(3);
    }
}
