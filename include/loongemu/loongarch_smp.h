#ifndef LOONGEMU_LOONGARCH_SMP_H
#define LOONGEMU_LOONGARCH_SMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOONGEMU_MAX_SMP_CPUS 4u

/* LoongArch64 IOCSR IPI / Mail register addresses (LoongArch Virt / 3A5000/3A6000). */
#define LOONGEMU_IOCSR_IPI_STATUS  0x1000u
#define LOONGEMU_IOCSR_IPI_ENABLE  0x1004u
#define LOONGEMU_IOCSR_IPI_SET     0x1008u
#define LOONGEMU_IOCSR_IPI_CLEAR   0x100cu
#define LOONGEMU_IOCSR_IPI_MBUF0   0x1020u
#define LOONGEMU_IOCSR_IPI_MBUF1   0x1028u
#define LOONGEMU_IOCSR_IPI_MBUF2   0x1030u
#define LOONGEMU_IOCSR_IPI_MBUF3   0x1038u
#define LOONGEMU_IOCSR_IPI_SEND    0x1040u
#define LOONGEMU_IOCSR_MAIL_SEND   0x1048u

/* Software IPI action bits delivered via IOCSR_IPI_SEND. */
#define LOONGEMU_IPI_WAKE          (1u << 0)
#define LOONGEMU_IPI_TLB_SHOOTDOWN (1u << 1)
#define LOONGEMU_IPI_SMC_BROADCAST (1u << 2)

/* LoongArch64 ECFG/ESTAT bit for IPI interrupt. */
#define LOONGEMU_IRQ_IPI           12u

typedef struct LoongemuSpinLock {
    volatile uint32_t locked;
} LoongemuSpinLock;

static inline void loongemu_spin_init(LoongemuSpinLock *lk)
{
    lk->locked = 0;
}

static inline void loongemu_spin_lock(LoongemuSpinLock *lk)
{
    while (__atomic_exchange_n(&lk->locked, 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&lk->locked, __ATOMIC_RELAXED) != 0u) {
#if defined(__loongarch__)
            __asm__ volatile("dbar 0x700" ::: "memory");
#endif
        }
    }
}

static inline void loongemu_spin_unlock(LoongemuSpinLock *lk)
{
    __atomic_store_n(&lk->locked, 0u, __ATOMIC_RELEASE);
}

static inline uint32_t loongemu_phys_cpu_id(void)
{
#if defined(__loongarch__)
    uint64_t id = 0;
    __asm__ volatile("csrrd %0, 0x20" : "=r"(id));
    return (uint32_t)(id & 0x1ffu);
#else
    return 0u;
#endif
}

uint32_t loongemu_ipi_status(void);
void loongemu_ipi_enable(uint32_t mask);
void loongemu_ipi_clear(uint32_t mask);
void loongemu_ipi_send(uint32_t target_cpu, uint32_t action_bit);
void loongemu_mail_send(uint32_t target_cpu, uint32_t mailbox_idx, uint64_t val);

/*
 * Wake a secondary LoongArch physical core by writing its entry point to
 * MBUF0, stack/arg to MBUF1, and asserting IPI bit 0.
 */
void loongemu_boot_secondary_core(uint32_t target_cpu,
                                  uint64_t entry_pc,
                                  uint64_t arg);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_LOONGARCH_SMP_H */
