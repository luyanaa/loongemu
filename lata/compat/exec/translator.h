#ifndef LATA_EXEC_TRANSLATOR_H
#define LATA_EXEC_TRANSLATOR_H
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "target/arm/tcg/translate.h"

/* Guest instruction fetch; implemented by the bare runtime. */
uint32_t lata_guest_ldl(uint64_t addr);

static inline bool bswap_code(bool sctlr_b) { return sctlr_b; }

static inline uint32_t translator_ldl_swap(CPUARMState *env, DisasContextBase *s,
                                           vaddr addr, bool swap)
{
    uint32_t insn = lata_guest_ldl(addr);
    return swap ? bswap32(insn) : insn;
}

static inline uint16_t translator_lduw_swap(CPUARMState *env, DisasContextBase *s,
                                            vaddr addr, bool swap)
{
    uint16_t insn = (uint16_t)lata_guest_ldl(addr);
    return swap ? bswap16(insn) : insn;
}
#endif
