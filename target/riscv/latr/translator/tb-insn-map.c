/*
 * Per-TB native-offset -> guest-PC map for precise LATR exception
 * attribution. A host fault (for example an address error raised by a
 * misaligned LoongArch access) can be blamed on the exact guest instruction
 * that produced it.
 *
 * Storage is a fixed pool inside TRANSLATION_DATA: the freestanding core has
 * no general allocator and TBs are small. Slots are keyed by code range so a
 * reassembled TB replaces its previous entry, and the oldest slot is recycled
 * once the pool is full.
 */
#include "latr.h"

static LatrTbInsnMap *find_slot(uintptr_t code)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;

    if (t == NULL) {
        return NULL;
    }
    for (int i = 0; i < LATR_TB_MAP_SLOTS; i++) {
        if (t->insn_maps[i].valid && t->insn_maps[i].code_base == code) {
            return &t->insn_maps[i];
        }
    }
    return NULL;
}

void latr_tb_insn_map_begin(void *code, uint32_t code_size)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    LatrTbInsnMap *map;

    if (t == NULL) {
        return;
    }
    map = find_slot((uintptr_t)code);
    if (map == NULL) {
        map = &t->insn_maps[0];
        for (int i = 0; i < LATR_TB_MAP_SLOTS; i++) {
            if (!t->insn_maps[i].valid) {
                map = &t->insn_maps[i];
                break;
            }
            if (t->insn_maps[i].generation < map->generation) {
                map = &t->insn_maps[i];
            }
        }
    }
    map->code_base = (uintptr_t)code;
    map->code_size = code_size;
    map->count = 0;
    map->bind_pool_used = 0;
    map->valid = true;
    map->generation = ++t->insn_map_generation;
    t->active_insn_map = map;
}

void latr_tb_insn_map_add(uint64_t guest_pc, uint32_t native_start,
                          uint32_t native_end, uint32_t raw_len,
                          uint8_t site, uint32_t retired_before,
                          const LatrRecoveryBinding *binds, uint8_t bind_cnt)
{
    LatrTbInsnMap *map = lsenv && lsenv->tr_data
        ? lsenv->tr_data->active_insn_map : NULL;

    if (map == NULL || map->count >= LATR_TB_MAP_SPANS ||
        native_end <= native_start) {
        return;
    }
    LatrTbInsnSpan *span = &map->spans[map->count++];
    span->guest_pc = guest_pc;
    span->native_start = native_start;
    span->native_end = native_end;
    span->raw_len = raw_len;
    span->site = site;
    span->retired_before = retired_before;
    span->bind_cnt = 0;
    span->bind_first = 0;
    if (binds != NULL && bind_cnt != 0 &&
        map->bind_pool_used + bind_cnt <= LATR_TB_RECOVERY_BINDS) {
        uint8_t n = bind_cnt > LATR_RECOVERY_MAX_BINDS_PER_SPAN
                        ? LATR_RECOVERY_MAX_BINDS_PER_SPAN : bind_cnt;
        span->bind_first = (uint16_t)map->bind_pool_used;
        for (uint8_t i = 0; i < n; i++) {
            map->binds[map->bind_pool_used++] = binds[i];
        }
        span->bind_cnt = n;
    }
}

bool latr_tb_insn_map_resolve(uintptr_t host_pc, uint64_t *guest_pc,
                              uint32_t *raw_len)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    const LatrTbInsnSpan *best = NULL;
    uint32_t best_generation = 0;

    if (t == NULL) {
        return false;
    }
    for (int i = 0; i < LATR_TB_MAP_SLOTS; i++) {
        const LatrTbInsnMap *map = &t->insn_maps[i];
        if (!map->valid || host_pc < map->code_base ||
            host_pc >= map->code_base + map->code_size) {
            continue;
        }
        uint32_t offset = (uint32_t)(host_pc - map->code_base);
        for (uint32_t j = 0; j < map->count; j++) {
            const LatrTbInsnSpan *span = &map->spans[j];
            if (offset < span->native_start || offset >= span->native_end) {
                continue;
            }
            if (best == NULL || map->generation >= best_generation) {
                best = span;
                best_generation = map->generation;
            }
        }
    }
    if (best == NULL) {
        return false;
    }
    if (guest_pc != NULL) {
        *guest_pc = best->guest_pc;
    }
    if (raw_len != NULL) {
        *raw_len = best->raw_len;
    }
    return true;
}

void latr_tb_insn_map_rebase(void *old_code, void *new_code)
{
    LatrTbInsnMap *map = find_slot((uintptr_t)old_code);

    if (map != NULL) {
        map->code_base = (uintptr_t)new_code;
    }
}

void latr_tb_insn_map_forget(void *code)
{
    LatrTbInsnMap *map = find_slot((uintptr_t)code);

    if (map != NULL) {
        map->valid = false;
        if (lsenv && lsenv->tr_data && lsenv->tr_data->active_insn_map == map) {
            lsenv->tr_data->active_insn_map = NULL;
        }
    }
}

void latr_tb_insn_map_forget_range(void *base, uint32_t size)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    uintptr_t start = (uintptr_t)base;
    uintptr_t end = start + size;

    if (t == NULL || size == 0) {
        return;
    }
    for (int i = 0; i < LATR_TB_MAP_SLOTS; i++) {
        LatrTbInsnMap *map = &t->insn_maps[i];

        if (!map->valid) {
            continue;
        }
        /* Empty ranges are not produced by the assembler, but tolerate one so
         * a zero-length TB cannot pin a map forever. */
        if (map->code_base == start ||
            (map->code_base < end &&
             start < map->code_base + map->code_size)) {
            map->valid = false;
            if (t->active_insn_map == map) {
                t->active_insn_map = NULL;
            }
        }
    }
}

/*
 * C1 precise recovery.
 *
 * A span already records where its guest insn lives in native code. For the
 * insns that can fault architecturally (loads/stores/atomics and explicit
 * guest traps) we additionally persist the regcache bindings that were live at
 * that point: which guest GPRs held a not-yet-written-back value, and which
 * host temp register carried it. On a host fault we can then replay those
 * bindings from the captured host register file to reconstruct the
 * architectural guest state that the translated code had deferred.
 */
const LatrTbInsnSpan *latr_tb_recovery_resolve(uintptr_t host_pc,
                                               const LatrTbInsnMap **map_out)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    const LatrTbInsnSpan *best = NULL;
    const LatrTbInsnMap *best_map = NULL;
    uint32_t best_generation = 0;

    if (t == NULL) {
        return NULL;
    }
    for (int i = 0; i < LATR_TB_MAP_SLOTS; i++) {
        const LatrTbInsnMap *map = &t->insn_maps[i];
        if (!map->valid || host_pc < map->code_base ||
            host_pc >= map->code_base + map->code_size) {
            continue;
        }
        uint32_t offset = (uint32_t)(host_pc - map->code_base);
        for (uint32_t j = 0; j < map->count; j++) {
            const LatrTbInsnSpan *span = &map->spans[j];
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
 * Replay the span's bindings into `env`. `host_gprs` is the fault-time host
 * register file in $r0..$r31 order (what a ucontext/sigframe provides). Each
 * binding's `temp` is the host phys reg number (la_t0..la_t5 == 12..17) that
 * held the uncommitted guest value, so env->gpr[guest_reg] = host_gprs[temp].
 * Returns false for NATIVE_INTERNAL rows: a fault there is a host bug and must
 * never be dressed up as a guest exception.
 */
bool latr_tb_recovery_apply(const LatrTbInsnSpan *span,
                            const LatrTbInsnMap *map,
                            const uint64_t host_gprs[32], CPUArchState *env)
{
    if (span == NULL || env == NULL ||
        span->site == LATR_RECOV_NATIVE_INTERNAL) {
        return false;
    }
    for (uint8_t i = 0; i < span->bind_cnt; i++) {
        const LatrRecoveryBinding *b = &map->binds[span->bind_first + i];
        int host_reg = b->temp;
        if (host_reg >= 0 && host_reg < 32 && b->guest_reg < 32) {
            env->gpr[b->guest_reg] = host_gprs[host_reg];
        }
    }
    env->pc = span->guest_pc;
    return true;
}
