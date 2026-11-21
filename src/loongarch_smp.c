#include "loongemu/loongarch_smp.h"

#if defined(__loongarch__)

static inline uint32_t iocsrrd_w(uint32_t reg)
{
    uint32_t val;
    __asm__ volatile("iocsrrd.w %0, %1" : "=r"(val) : "r"(reg) : "memory");
    return val;
}

static inline void iocsrwr_w(uint32_t val, uint32_t reg)
{
    __asm__ volatile("iocsrwr.w %0, %1" :: "r"(val), "r"(reg) : "memory");
}

static inline void iocsrwr_d(uint64_t val, uint32_t reg)
{
    __asm__ volatile("iocsrwr.d %0, %1" :: "r"(val), "r"(reg) : "memory");
}

#else

static inline uint32_t iocsrrd_w(uint32_t reg) { (void)reg; return 0; }
static inline void iocsrwr_w(uint32_t val, uint32_t reg) { (void)val; (void)reg; }
static inline void iocsrwr_d(uint64_t val, uint32_t reg) { (void)val; (void)reg; }

#endif

uint32_t loongemu_ipi_status(void)
{
    return iocsrrd_w(LOONGEMU_IOCSR_IPI_STATUS);
}

void loongemu_ipi_enable(uint32_t mask)
{
    iocsrwr_w(mask, LOONGEMU_IOCSR_IPI_ENABLE);
}

void loongemu_ipi_clear(uint32_t mask)
{
    iocsrwr_w(mask, LOONGEMU_IOCSR_IPI_CLEAR);
}

void loongemu_ipi_send(uint32_t target_cpu, uint32_t action_bit)
{
    /*
     * Call sites pass a bit index (0 wake, 1 TLB, 2 SMC), while IOCSR
     * carries a one-hot action mask.  Passing the index through made a TLB
     * shootdown arrive as action 2 and be handled as an SMC broadcast.
     */
    uint32_t val = (1u << 31) | ((target_cpu & 0x3ffu) << 16) |
                   (1u << (action_bit & 0x1fu));
    iocsrwr_w(val, LOONGEMU_IOCSR_IPI_SEND);
}

void loongemu_mail_send(uint32_t target_cpu, uint32_t mailbox_idx, uint64_t val)
{
    uint32_t mb_lo = (mailbox_idx & 0x3u) * 2u;
    uint32_t mb_hi = mb_lo + 1u;
    uint64_t cmd_lo = (UINT64_C(1) << 31) |
                      ((uint64_t)(target_cpu & 0x3ffu) << 16) |
                      ((uint64_t)(mb_lo & 0x7u) << 2) |
                      ((val & UINT64_C(0xffffffff)) << 32);
    uint64_t cmd_hi = (UINT64_C(1) << 31) |
                      ((uint64_t)(target_cpu & 0x3ffu) << 16) |
                      ((uint64_t)(mb_hi & 0x7u) << 2) |
                      (val & UINT64_C(0xffffffff00000000));

    iocsrwr_d(cmd_lo, LOONGEMU_IOCSR_MAIL_SEND);
    iocsrwr_d(cmd_hi, LOONGEMU_IOCSR_MAIL_SEND);
}

void loongemu_boot_secondary_core(uint32_t target_cpu,
                                  uint64_t entry_pc,
                                  uint64_t arg)
{
    loongemu_mail_send(target_cpu, 0u, entry_pc);
    loongemu_mail_send(target_cpu, 1u, arg);
#if defined(__loongarch__)
    __asm__ volatile("dbar 0" ::: "memory");
#endif
    loongemu_ipi_send(target_cpu, 0u);
}
