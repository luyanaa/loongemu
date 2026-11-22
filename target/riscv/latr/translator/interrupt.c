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
#define CSR_MIDELEG_ADDR 0x303
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
    int level = 0;

    if (env->plic != NULL) {
        level = loongemu_plic_pending(env->plic, (uint32_t)env->plic_context);
    }
    /* PLIC context 0 is the machine-level external interrupt (QEMU virt). */
    set_mip_bit(env, MIP_MEIP, level);
    set_mip_bit(env, MIP_SEIP, 0);
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
            /* Delegated S interrupt: not taken in M-mode; in U/S the
             * S-mode global enable (SIE) must be set. */
            if (privilege == LATR_RISCV_PRIV_M ||
                !(status & MSTATUS_SIE)) {
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
