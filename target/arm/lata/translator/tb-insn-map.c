/*
 * C2 precise recovery for the LATA (AArch64 -> LoongArch) port.
 *
 * The span table maps a native code range back to the guest instruction that
 * produced it, and -- for the insns that can fault architecturally (loads,
 * stores, atomics and explicit guest traps) -- records the regcache bindings
 * that were live at that point. LATA has no host signal path yet; the apply
 * side is exercised synthetically from a captured host register file, exactly
 * the shape a ucontext entry would feed.
 *
 * Mirrored from the LATR C1 implementation (target/riscv/latr).
 */

#include "lata.h"
#include "env.h"

void lata_tb_insn_map_begin(void *code, uint32_t code_size)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    LataTbInsnMap *map;
    uint32_t oldest_gen = 0;
    int slot = -1;

    if (t == NULL) {
        return;
    }
    for (int i = 0; i < LATA_TB_MAP_SLOTS; i++) {
        if (!t->insn_maps[i].valid) {
            slot = i;
            break;
        }
        if (slot < 0 || t->insn_maps[i].generation < oldest_gen) {
            oldest_gen = t->insn_maps[i].generation;
            slot = i;
        }
    }
    map = &t->insn_maps[slot < 0 ? 0 : slot];
    map->code_base = (uintptr_t)code;
    map->code_size = code_size;
    map->count = 0;
    map->bind_pool_used = 0;
    map->valid = true;
    map->generation = ++t->insn_map_generation;
    t->active_insn_map = map;
}

void lata_tb_insn_map_add(uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint32_t raw_len,
                          uint8_t site, uint32_t retired_before,
                          const LataRecoveryBinding *binds, uint8_t bind_cnt)
{
    LataTbInsnMap *map = lsenv && lsenv->tr_data
        ? lsenv->tr_data->active_insn_map : NULL;

    if (map == NULL || map->count >= LATA_TB_MAP_SPANS ||
        native_end <= native_start) {
        return;
    }
    LataTbInsnSpan *span = &map->spans[map->count++];
    span->guest_pc = guest_pc;
    span->native_start = native_start;
    span->native_end = native_end;
    span->raw_len = raw_len;
    span->site = site;
    span->retired_before = retired_before;
    span->bind_cnt = 0;
    span->bind_first = 0;
    if (binds != NULL && bind_cnt != 0 &&
        map->bind_pool_used + bind_cnt <= LATA_TB_RECOVERY_BINDS) {
        uint8_t n = bind_cnt > LATA_RECOVERY_MAX_BINDS_PER_SPAN
                        ? LATA_RECOVERY_MAX_BINDS_PER_SPAN : bind_cnt;
        span->bind_first = (uint16_t)map->bind_pool_used;
        for (uint8_t i = 0; i < n; i++) {
            map->binds[map->bind_pool_used++] = binds[i];
        }
        span->bind_cnt = n;
    }
}

const LataTbInsnSpan *lata_tb_recovery_resolve(uintptr_t host_pc,
                                               const LataTbInsnMap **map_out)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    const LataTbInsnSpan *best = NULL;
    const LataTbInsnMap *best_map = NULL;
    uint32_t best_generation = 0;

    if (t == NULL) {
        return NULL;
    }
    for (int i = 0; i < LATA_TB_MAP_SLOTS; i++) {
        const LataTbInsnMap *map = &t->insn_maps[i];
        if (!map->valid || host_pc < map->code_base ||
            host_pc >= map->code_base + map->code_size) {
            continue;
        }
        uint32_t offset = (uint32_t)(host_pc - map->code_base);
        for (uint32_t j = 0; j < map->count; j++) {
            const LataTbInsnSpan *span = &map->spans[j];
            if (offset < span->native_start || offset >= span->native_end) {
                continue;
            }
            if (best == NULL || map->generation >= best_generation) {
                best = span;
                best_map = map;
                best_generation = map->generation;
            }
        }
    }
    if (best == NULL) {
        return NULL;
    }
    if (map_out != NULL) {
        *map_out = best_map;
    }
    return best;
}

/*
 * Rebuild architectural guest state at `span` from a captured host register
 * file. Two classes:
 *
 *  - dedicated regs: arm_la_map[g] >= 0 means guest gpr g *always* lives in
 *    host phys reg arm_la_map[g], so env->xregs[g] = host_gprs[arm_la_map[g]]
 *    unconditionally (the regcache never defers these).
 *  - env-resident regs: arm_la_map[g] < 0 may hold a deferred value in a host
 *    temp; the span's bindings carry {guest_reg, temp} pairs captured at the
 *    fault span, so env->xregs[g] = host_gprs[temp].
 *
 * `host_gprs` is indexed by phys reg number ($r0..$r31), what a ucontext
 * provides. Returns false for NATIVE_INTERNAL -- a fault there is a translator
 * bug and must never be dressed up as a guest exception.
 */
bool lata_tb_recovery_apply(const LataTbInsnSpan *span,
                            const LataTbInsnMap *map,
                            const uint64_t host_gprs[32],
                            CPUARMState *env)
{
    if (span == NULL || env == NULL ||
        span->site == LATA_RECOV_NATIVE_INTERNAL) {
        return false;
    }
    for (int g = 0; g < 32; g++) {
        int host_reg = arm_la_map[g];
        if (host_reg >= 0 && host_reg < 32) {
            env->xregs[g] = host_gprs[host_reg];
        }
    }
    for (uint8_t i = 0; i < span->bind_cnt; i++) {
        const LataRecoveryBinding *b = &map->binds[span->bind_first + i];
        int host_reg = b->temp;
        if (host_reg >= 0 && host_reg < 32 && b->guest_reg < 32) {
            env->xregs[b->guest_reg] = host_gprs[host_reg];
        }
    }
    env->pc = span->guest_pc;
    return true;
}
