#include "loongemu/soft_ras.h"

static void zero_bytes(void *ptr, uint32_t size)
{
    unsigned char *p = (unsigned char *)ptr;

    for (uint32_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

void soft_ras_init(SoftRas *ras)
{
    if (ras == 0) {
        return;
    }
    zero_bytes(ras, (uint32_t)sizeof(*ras));
}

void soft_ras_flush(SoftRas *ras)
{
    if (ras == 0) {
        return;
    }
    for (uint32_t i = 0; i < SOFT_RAS_DEPTH; i++) {
        ras->entries[i].valid = 0;
        ras->entries[i].native_pc = 0;
    }
    ras->top = 0;
    ras->count = 0;
    ras->flushes++;
}

void soft_ras_push(SoftRas *ras, uint64_t guest_pc, uint64_t native_pc,
                   uint64_t cr3, uint8_t cpl, uint8_t mode_flags)
{
    SoftRasEntry *e;

    if (ras == 0) {
        return;
    }
    e = &ras->entries[ras->top];
    e->guest_pc = guest_pc;
    e->native_pc = native_pc;
    e->cr3 = cr3;
    e->cpl = cpl;
    e->mode_flags = mode_flags;
    e->valid = 1;

    ras->top = (ras->top + 1u) & (SOFT_RAS_DEPTH - 1u);
    if (ras->count < SOFT_RAS_DEPTH) {
        ras->count++;
    }
    ras->pushes++;
}

uint64_t soft_ras_lookup(SoftRas *ras, uint64_t guest_pc, uint64_t cr3,
                         uint8_t cpl, uint8_t mode_flags)
{
    uint32_t max_probe;

    if (ras == 0) {
        return 0;
    }
    ras->lookups++;
    if (ras->count == 0u) {
        ras->misses++;
        return 0;
    }

    /* Check top first, then 1 frame deeper to absorb a single tail-call/skew. */
    max_probe = ras->count < 2u ? ras->count : 2u;
    for (uint32_t depth = 0; depth < max_probe; depth++) {
        uint32_t idx = (ras->top - 1u - depth) & (SOFT_RAS_DEPTH - 1u);
        SoftRasEntry *e = &ras->entries[idx];

        if (e->valid &&
            e->guest_pc == guest_pc &&
            e->cr3 == cr3 &&
            e->cpl == cpl &&
            e->mode_flags == mode_flags) {
            uint64_t target = e->native_pc;

            for (uint32_t k = 0; k <= depth; k++) {
                uint32_t pop_idx = (ras->top - 1u - k) & (SOFT_RAS_DEPTH - 1u);
                ras->entries[pop_idx].valid = 0;
                ras->entries[pop_idx].native_pc = 0;
            }
            ras->top = idx;
            ras->count -= (depth + 1u);
            if (target != 0u) {
                ras->hits++;
                return target;
            }
            ras->misses++;
            return 0;
        }
    }

    ras->misses++;
    return 0;
}

uint32_t soft_ras_backfill(SoftRas *ras, uint64_t guest_pc, uint64_t cr3,
                           uint8_t cpl, uint8_t mode_flags,
                           uint64_t native_pc)
{
    uint32_t filled = 0;

    if (ras == 0 || native_pc == 0u || ras->count == 0u) {
        return 0;
    }
    for (uint32_t d = 0; d < ras->count; d++) {
        uint32_t idx = (ras->top - 1u - d) & (SOFT_RAS_DEPTH - 1u);
        SoftRasEntry *e = &ras->entries[idx];

        if (e->valid &&
            e->native_pc == 0u &&
            e->guest_pc == guest_pc &&
            e->cr3 == cr3 &&
            e->cpl == cpl &&
            e->mode_flags == mode_flags) {
            e->native_pc = native_pc;
            ras->backfills++;
            filled++;
        }
    }
    return filled;
}

void soft_ras_invalidate_range(SoftRas *ras, uint64_t start_pc,
                               uint64_t end_pc)
{
    if (ras == 0 || start_pc >= end_pc) {
        return;
    }
    for (uint32_t i = 0; i < SOFT_RAS_DEPTH; i++) {
        SoftRasEntry *e = &ras->entries[i];

        if (e->valid && e->guest_pc >= start_pc && e->guest_pc < end_pc) {
            e->valid = 0;
            e->native_pc = 0;
        }
    }
}
