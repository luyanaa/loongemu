#ifndef LOONGARCH_IR2_PATCH_H
#define LOONGARCH_IR2_PATCH_H

#include <stdbool.h>
#include <stdint.h>

/* Patch one LoongArch `b`: opcode 0x50000000 and signed imm26 in words. */
static inline bool loongarch_patch_b(uint32_t *insn_site, uintptr_t target)
{
    uintptr_t pc = (uintptr_t)insn_site;
    uintptr_t words;
    uint32_t original;

    if (insn_site == 0 || ((pc | target) & 3u) != 0) {
        return false;
    }
    original = __atomic_load_n(insn_site, __ATOMIC_RELAXED);
    if ((original & UINT32_C(0xfc000000)) != UINT32_C(0x50000000)) {
        return false;
    }
    if (target >= pc) {
        uintptr_t delta = target - pc;
        if (delta > UINT32_C(0x07fffffc)) {
            return false;
        }
        words = delta >> 2;
    } else {
        uintptr_t delta = pc - target;
        if (delta > UINT32_C(0x08000000)) {
            return false;
        }
        words = (uintptr_t)0 - (delta >> 2);
    }

    __atomic_store_n(insn_site,
                     (original & UINT32_C(0xfc000000)) |
                         ((uint32_t)words & UINT32_C(0x03ffffff)),
                     __ATOMIC_RELEASE);
    __builtin___clear_cache((char *)insn_site,
                            (char *)(insn_site + 1));
    return true;
}

#endif
