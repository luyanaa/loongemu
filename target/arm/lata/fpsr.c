/*
 * Guest FPSR state.  The five IEEE flags accumulate (they are sticky), so the
 * merge is an OR; QC (bit 27) is owned by the saturating vector paths.
 *
 * These helpers work on the storage slot itself (`&env->vfp.xregs[ARM_VFP_FPSR]`)
 * so they stay testable without the CPU headers.
 */
#include "lata-fpsr.h"

void lata_fpsr_accumulate(uint64_t *slot, uint32_t la_fcsr)
{
    if (slot == NULL) {
        return;
    }
    *slot |= (uint64_t)(lata_fpsr_flags_from_fcsr(la_fcsr) &
                        LATA_FPSR_FLAG_MASK);
}

uint32_t lata_fpsr_read(const uint64_t *slot)
{
    return (slot == NULL) ? 0u : (uint32_t)*slot;
}

void lata_fpsr_write(uint64_t *slot, uint32_t value)
{
    if (slot == NULL) {
        return;
    }
    *slot = value;
}
