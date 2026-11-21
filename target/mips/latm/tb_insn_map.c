/*
 * Per-TB native-offset to guest-PC map used by precise LATM exceptions.
 * The table is rebuilt for every translated block; host addresses recorded by
 * GETPC() resolve back to the exact guest instruction (and delay-slot bit).
 */
#include "qemu/osdep.h"
#include <stdlib.h>
#include "latm.h"

void latm_tb_insn_map_begin(void *trd, const void *code, uint32_t code_size)
{
    TRANSLATION_DATA *t = trd;
    TranslationBlock *tb = t ? t->curr_tb : NULL;
    LatmTbInsnMap *map = NULL;

    if (!t) {
        return;
    }
    if (tb && tb->latm_insn_map) {
        map = tb->latm_insn_map;
        if (!map->valid) {
            tb->latm_insn_map = NULL;
            map = NULL;
        }
    }
    if (!map) {
        /* Non-TB callers (host unit tests) reuse a map by code range. */
        for (LatmTbInsnMap *m = t->insn_map_list; m; m = m->next) {
            if (m->valid && m->code_base == (uintptr_t)code) {
                map = m;
                break;
            }
        }
    }
    if (!map) {
        map = calloc(1, sizeof(*map));
        if (!map) {
            return;
        }
        map->next = t->insn_map_list;
        t->insn_map_list = map;
        t->insn_map_count++;
    }
    map->code_base = (uintptr_t)code;
    map->code_size = code_size;
    map->count = 0;
    map->valid = 1;
    map->generation = ++t->insn_map_generation;
    t->active_insn_map = map;
    if (tb) {
        tb->latm_insn_map = map;
    }
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
    for (const LatmTbInsnMap *map = t->insn_map_list; map;
         map = map->next) {
        uint32_t off;

        if (!map->valid || !map->code_base || host_pc < map->code_base ||
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
    for (LatmTbInsnMap *m = t->insn_map_list; m; m = m->next) {
        m->valid = 0;
    }
    t->insn_map_list = NULL;
    t->insn_map_count = 0;
    t->active_insn_map = NULL;
    t->insn_map_generation++;
}

void latm_tb_insn_map_forget(void *trd, const void *code)
{
    TRANSLATION_DATA *t = trd;
    LatmTbInsnMap **link;
    if (!t) return;
    for (link = &t->insn_map_list; *link; link = &(*link)->next) {
        LatmTbInsnMap *map = *link;
        if (map->code_base == (uintptr_t)code) {
            if (map == t->active_insn_map) {
                t->active_insn_map = NULL;
            }
            if (t->curr_tb &&
                ((TranslationBlock *)t->curr_tb)->latm_insn_map == map) {
                ((TranslationBlock *)t->curr_tb)->latm_insn_map = NULL;
            }
            *link = map->next;
            free(map);
            t->insn_map_count--;
            return;
        }
    }
}

void latm_tb_insn_map_release(void *trd, void *tb_ptr)
{
    TRANSLATION_DATA *t = trd;
    TranslationBlock *tb = tb_ptr;
    LatmTbInsnMap **link;
    if (!t || !tb) return;
    LatmTbInsnMap *map = tb->latm_insn_map;
    if (!map) return;
    tb->latm_insn_map = NULL;
    for (link = &t->insn_map_list; *link; link = &(*link)->next) {
        if (*link == map) {
            if (map == t->active_insn_map) {
                t->active_insn_map = NULL;
            }
            *link = map->next;
            free(map);
            t->insn_map_count--;
            return;
        }
    }
    /* A map invalidated by SMC is no longer in the list; free it here. */
    free(map);
}

