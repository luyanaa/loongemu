/*
 * Per-TB native-offset to guest-PC map used by precise LATM exceptions.
 * The table is rebuilt for every translated block; host addresses recorded by
 * GETPC() resolve back to the exact guest instruction (and delay-slot bit).
 */
#include "qemu/osdep.h"
#include "latm.h"

void latm_tb_insn_map_begin(void *trd, const void *code, uint32_t code_size)
{
    TRANSLATION_DATA *t = trd;

    if (!t) {
        return;
    }
    t->insn_map.code_base = (uintptr_t)code;
    t->insn_map.code_size = code_size;
    t->insn_map.count = 0;
}

void latm_tb_insn_map_add(void *trd, uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint8_t delay_slot)
{
    TRANSLATION_DATA *t = trd;
    LatmTbInsnSpan *span;

    if (!t || t->insn_map.count >= LATM_TB_MAP_MAX) {
        return;
    }
    span = &t->insn_map.spans[t->insn_map.count++];
    span->guest_pc = guest_pc;
    span->native_start = native_start;
    span->native_end = native_end;
    span->delay_slot = delay_slot;
}

int latm_tb_insn_map_resolve(void *trd, uintptr_t host_pc,
                             uint64_t *guest_pc, uint8_t *delay_slot)
{
    TRANSLATION_DATA *t = trd;
    uintptr_t base;
    uint32_t off, best_len = UINT32_MAX;
    const LatmTbInsnSpan *best = NULL;

    if (!t || !t->insn_map.code_base || host_pc < t->insn_map.code_base) {
        return 0;
    }
    base = t->insn_map.code_base;
    off = (uint32_t)(host_pc - base);
    for (unsigned i = 0; i < t->insn_map.count; i++) {
        const LatmTbInsnSpan *span = &t->insn_map.spans[i];
        uint32_t len;

        if (off < span->native_start || off >= span->native_end) {
            continue;
        }
        len = span->native_end - span->native_start;
        if (!best || len < best_len) {
            best = span;
            best_len = len;
        }
    }
    if (!best) {
        return 0;
    }
    if (guest_pc) {
        *guest_pc = best->guest_pc;
    }
    if (delay_slot) {
        *delay_slot = best->delay_slot;
    }
    return 1;
}

