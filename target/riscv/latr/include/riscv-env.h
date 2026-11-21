#ifndef LATR_RISCV_ENV_H
#define LATR_RISCV_ENV_H

#include <stddef.h>
#include <stdint.h>

#define LATR_RISCV_CSR_COUNT 4096
#define LATR_RISCV_PRIV_U 0
#define LATR_RISCV_PRIV_S 1
#define LATR_RISCV_PRIV_M 3
#define LATR_RISCV_CSR_STIMECMP 0x14d
#define LATR_RISCV_CSR_MIP      0x344
#define LATR_RISCV_MIP_STIP     (UINT64_C(1) << 5)
#define LATR_RISCV_TIMEBASE_HZ  UINT64_C(1000000000)

typedef struct LoongemuVClock LoongemuVClock;


typedef struct CPUArchState CPUArchState;

struct CPUArchState {
    uint64_t gpr[32];
    uint64_t fpr[32];
    uint64_t pc;
    uint64_t fcsr;
    uint64_t exception;
    uint64_t badaddr;
    uint64_t csr[LATR_RISCV_CSR_COUNT];

    /* Architectural privilege (U=0, S=1, M=3); zero-initialized state is U. */
    uint64_t privilege;

    /* LR/SC reservation tracked independently of the host LLbit. */
    uint64_t reservation_addr;
    uint64_t reservation_size;
    uint64_t reservation_epoch;
    uint64_t reservation_valid;

    /* Host call / runtime integration state; appended to preserve IR offsets. */
    uint64_t host_return_address;
    /* Phase 3 sets the VM-shared clock before guest entry; CSR_TIME reads it. */
    LoongemuVClock *vclock;
    /* Phase 3 consumes this after TB exit for local instruction-cache invalidation. */
    uint64_t fence_i_pending;
    /*
     * Sstc contract for phase 3: csr[STIMECMP] is in vclock nanoseconds
     * (TIMEBASE_HZ); writes set this request bit. Runtime consumes/clears it
     * after TB exit and updates MIP.STIP as virtual time crosses the deadline.
     */
    uint64_t stimecmp_update_pending;
};

static inline int env_offset_gpr(int reg)
{
    return (int)offsetof(CPUArchState, gpr) + reg * (int)sizeof(uint64_t);
}

static inline int env_offset_fpr(int reg)
{
    return (int)offsetof(CPUArchState, fpr) + reg * (int)sizeof(uint64_t);
}

static inline int env_offset_pc(void)
{
    return (int)offsetof(CPUArchState, pc);
}

static inline int env_offset_fcsr(void)
{
    return (int)offsetof(CPUArchState, fcsr);
}

static inline int env_offset_csr(int csr)
{
    return (int)offsetof(CPUArchState, csr) +
           (csr & (LATR_RISCV_CSR_COUNT - 1)) * (int)sizeof(uint64_t);
}

static inline int env_offset_exception(void)
{
    return (int)offsetof(CPUArchState, exception);
}

static inline int env_offset_badaddr(void)
{
    return (int)offsetof(CPUArchState, badaddr);
}
static inline int env_offset_privilege(void)
{
    return (int)offsetof(CPUArchState, privilege);
}

static inline int env_offset_reservation_addr(void)
{
    return (int)offsetof(CPUArchState, reservation_addr);
}

static inline int env_offset_reservation_size(void)
{
    return (int)offsetof(CPUArchState, reservation_size);
}

static inline int env_offset_reservation_epoch(void)
{
    return (int)offsetof(CPUArchState, reservation_epoch);
}

static inline int env_offset_reservation_valid(void)
{
    return (int)offsetof(CPUArchState, reservation_valid);
}

static inline int env_offset_host_return_address(void)
{
    return (int)offsetof(CPUArchState, host_return_address);
}

static inline int env_offset_vclock(void)
{
    return (int)offsetof(CPUArchState, vclock);
}

static inline int env_offset_fence_i_pending(void)
{
    return (int)offsetof(CPUArchState, fence_i_pending);
}

static inline int env_offset_stimecmp_update_pending(void)
{
    return (int)offsetof(CPUArchState, stimecmp_update_pending);
}


#endif
