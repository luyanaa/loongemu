/*
 * Guest FPSR accumulation from the host floating-point status register.
 *
 * LoongArch keeps the five IEEE exception flags in FCSR bits 4..0 as
 * V, Z, O, U, I; AArch64 wants IOC, DZC, OFC, UFC, IXC in FPSR bits 0..4,
 * i.e. the same five flags in reverse order.  The mapping is pure integer
 * work, so it is testable on any host.
 */
#ifndef LATA_FPSR_H
#define LATA_FPSR_H

#include <stddef.h>
#include <stdint.h>

/*
 * LoongArch Vol1, Table 6: FCSR0 bits 4:0 hold the *enables* and bits 20:16
 * hold the sticky *flags* (bit 20 = V, 19 = Z, 18 = O, 17 = U, 16 = I).
 * Reading bits 4:0 yields zero on a machine that does not enable traps.
 */
#define LATA_FCSR_FLAG_SHIFT 16u
#define LATA_FCSR_FLAG_V (1u << 20) /* invalid operation -> IOC (bit 0) */
#define LATA_FCSR_FLAG_Z (1u << 19) /* divide by zero    -> DZC (bit 1) */
#define LATA_FCSR_FLAG_O (1u << 18) /* overflow          -> OFC (bit 2) */
#define LATA_FCSR_FLAG_U (1u << 17) /* underflow         -> UFC (bit 3) */
#define LATA_FCSR_FLAG_I (1u << 16) /* inexact           -> IXC (bit 4) */

#define LATA_FPSR_IOC (1u << 0)
#define LATA_FPSR_DZC (1u << 1)
#define LATA_FPSR_OFC (1u << 2)
#define LATA_FPSR_UFC (1u << 3)
#define LATA_FPSR_IXC (1u << 4)
#define LATA_FPSR_FLAG_MASK 0x1fu
#define LATA_FCSR_FLAG_MASK 0x001f0000u

/* Map a host FCSR word to the AArch64 FPSR flag bits (no state involved). */
static inline uint32_t lata_fpsr_flags_from_fcsr(uint32_t fcsr)
{
    uint32_t flags = 0;

    fcsr = (fcsr >> LATA_FCSR_FLAG_SHIFT) & LATA_FPSR_FLAG_MASK;

    if (fcsr & (LATA_FCSR_FLAG_V >> LATA_FCSR_FLAG_SHIFT)) {
        flags |= LATA_FPSR_IOC;
    }
    if (fcsr & (LATA_FCSR_FLAG_Z >> LATA_FCSR_FLAG_SHIFT)) {
        flags |= LATA_FPSR_DZC;
    }
    if (fcsr & (LATA_FCSR_FLAG_O >> LATA_FCSR_FLAG_SHIFT)) {
        flags |= LATA_FPSR_OFC;
    }
    if (fcsr & (LATA_FCSR_FLAG_U >> LATA_FCSR_FLAG_SHIFT)) {
        flags |= LATA_FPSR_UFC;
    }
    if (fcsr & (LATA_FCSR_FLAG_I >> LATA_FCSR_FLAG_SHIFT)) {
        flags |= LATA_FPSR_IXC;
    }
    return flags;
}

/* Sticky accumulation into `&env->vfp.xregs[ARM_VFP_FPSR]`. */
void lata_fpsr_accumulate(uint64_t *slot, uint32_t la_fcsr);
uint32_t lata_fpsr_read(const uint64_t *slot);
void lata_fpsr_write(uint64_t *slot, uint32_t value);

#endif
