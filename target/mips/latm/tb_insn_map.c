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
    LatmTbInsnMap *map = NULL;

    if (!t) {
        return;
    }
    for (uint32_t i = 0; i < t->insn_map_count; i++) {
        if (t->insn_maps[i].code_base == (uintptr_t)code) {
            map = &t->insn_maps[i];
            break;
        }
    }
    if (!map) {
        if (t->insn_map_count < LATM_TB_MAP_POOL) {
            map = &t->insn_maps[t->insn_map_count++];
        } else {
            /* Evict the oldest reconstructed map. */
            map = &t->insn_maps[0];
            for (uint32_t i = 1; i < LATM_TB_MAP_POOL; i++) {
                if (t->insn_maps[i].generation < map->generation) {
                    map = &t->insn_maps[i];
                }
            }
        }
    }
    map->code_base = (uintptr_t)code;
    map->code_size = code_size;
    map->count = 0;
    map->generation = ++t->insn_map_generation;
    t->active_insn_map = map;
}

void latm_tb_insn_map_add(void *trd, uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint8_t delay_slot)
{
    TRANSLATION_DATA *t = trd;
    LatmTbInsnSpan *span;

    if (!t || !t->active_insn_map ||
        t->active_insn_map->count >= LATM_TB_MAP_MAX) {
        return;
    }
    span = &t->active_insn_map->spans[t->active_insn_map->count++];
    span->guest_pc = guest_pc;
    span->native_start = native_start;
    span->native_end = native_end;
    span->delay_slot = delay_slot;
}

int latm_tb_insn_map_resolve(void *trd, uintptr_t host_pc,
                             uint64_t *guest_pc, uint8_t *delay_slot)
{
    TRANSLATION_DATA *t = trd;
    uint32_t best_len = UINT32_MAX;
    uint32_t best_gen = 0;
    const LatmTbInsnSpan *best = NULL;

    if (!t) {
        return 0;
    }
    for (uint32_t m = 0; m < t->insn_map_count; m++) {
        const LatmTbInsnMap *map = &t->insn_maps[m];
        uint32_t off;

        if (!map->code_base || host_pc < map->code_base ||
            host_pc - map->code_base > UINT32_MAX) {
            continue;
        }
        off = (uint32_t)(host_pc - map->code_base);
        for (unsigned i = 0; i < map->count; i++) {
            const LatmTbInsnSpan *span = &map->spans[i];
            uint32_t len;

            if (off < span->native_start || off >= span->native_end) {
                continue;
            }
            len = span->native_end - span->native_start;
            if (!best || len < best_len ||
                (len == best_len && map->generation > best_gen)) {
                best = span;
                best_len = len;
                best_gen = map->generation;
            }
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

void latm_tb_insn_map_flush(void *trd)
{
    TRANSLATION_DATA *t = trd;
    if (!t) return;
    memset(t->insn_maps, 0, sizeof(t->insn_maps));
    t->insn_map_count = 0;
    t->active_insn_map = NULL;
    t->insn_map_generation++;
}

void latm_tb_insn_map_forget(void *trd, const void *code)
{
    TRANSLATION_DATA *t = trd;
    if (!t) return;
    for (uint32_t i = 0; i < t->insn_map_count; i++) {
        if (t->insn_maps[i].code_base == (uintptr_t)code) {
            if (&t->insn_maps[i] == t->active_insn_map) {
                t->active_insn_map = NULL;
            }
            t->insn_maps[i] = t->insn_maps[--t->insn_map_count];
            memset(&t->insn_maps[t->insn_map_count], 0,
                   sizeof(t->insn_maps[0]));
            return;
        }
    }
}

