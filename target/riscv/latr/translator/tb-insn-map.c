/*
 * Per-TB native-offset -> guest-PC map for precise LATR exception
 * attribution. A host fault (for example an address error raised by a
 * misaligned LoongArch access) can be blamed on the exact guest instruction
 * that produced it.
 * Storage is a per-translation-data list of heap-allocated maps owned by the
 * TB code range they describe. A map is created on latr_tb_insn_map_begin()
 * (once per assembled block) and removed only when that block's code is
 * forgotten or rebased away, so an old still-executing TB always resolves --
 * no fixed LRU cap can drop a map whose machine code is still live.
 */
#include "latr.h"
#include "mem.h"

static LatrTbInsnMap *find_map(uintptr_t code)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;

    if (t == NULL) {
        return NULL;
    }
    for (LatrTbInsnMap *map = t->insn_map_list; map != NULL;
         map = map->next) {
        if (map->valid && map->code_base == code) {
            return map;
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
    map = find_map((uintptr_t)code);
    if (map == NULL) {
        map = mm_calloc(1, sizeof(*map));
        if (map == NULL) {
            t->active_insn_map = NULL;
            return;
        }
        map->next = t->insn_map_list;
        t->insn_map_list = map;
        t->insn_map_count++;
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
                          uint8_t site, uint8_t phase, uint32_t retired_before,
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
    latr_span_set_raw_len(span, raw_len);
    span->site = site;
    span->phase = phase;
    span->effect_id = 0;
    span->retired_before = retired_before;
    span->bind_cnt = 0;
    span->bind_first = 0;
    span->bind_flags = 0;
    if (binds != NULL && bind_cnt != 0) {
        uint8_t n = bind_cnt > LATR_RECOVERY_MAX_BINDS_PER_SPAN
                        ? LATR_RECOVERY_MAX_BINDS_PER_SPAN : bind_cnt;
        if (map->bind_pool_used + n > LATR_TB_RECOVERY_BINDS) {
            n = 0;
        }
        if (n < bind_cnt) {
            /* Truncating uncommitted state must never look like a clean
             * snapshot: flag the span so precise-recovery-dependent code
             * paths are denied rather than silently applied. */
            span->bind_flags |= LE_BIND_OVERFLOW;
        }
        if (n != 0) {
            span->bind_first = (uint16_t)map->bind_pool_used;
            for (uint8_t i = 0; i < n; i++) {
                map->binds[map->bind_pool_used++] = binds[i];
            }
            span->bind_cnt = n;
        }
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
    for (LatrTbInsnMap *map = t->insn_map_list; map != NULL; map = map->next) {
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
        *raw_len = latr_span_raw_len(best);
    }
    return true;
}

void latr_tb_insn_map_rebase(void *old_code, void *new_code)
{
    LatrTbInsnMap *map = find_map((uintptr_t)old_code);

    if (map != NULL) {
        map->code_base = (uintptr_t)new_code;
    }
}

/* Remove a map from the list and free it -- the TB it described no longer
 * owns that code range, so keeping it would attribute faults to dead code. */
static void drop_map(TRANSLATION_DATA *t, LatrTbInsnMap *map)
{
    if (t->active_insn_map == map) {
        t->active_insn_map = NULL;
    }
    if (t->insn_map_list == map) {
        t->insn_map_list = map->next;
    } else {
        for (LatrTbInsnMap *prev = t->insn_map_list;
             prev != NULL && prev->next != NULL; prev = prev->next) {
            if (prev->next == map) {
                prev->next = map->next;
                break;
            }
        }
    }
    t->insn_map_count--;
    mm_free(map);
}

void latr_tb_insn_map_forget(void *code)
{
    TRANSLATION_DATA *t = lsenv ? lsenv->tr_data : NULL;
    LatrTbInsnMap *map = t != NULL ? find_map((uintptr_t)code) : NULL;

    if (map != NULL) {
        drop_map(t, map);
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
    LatrTbInsnMap *map = t->insn_map_list;
    while (map != NULL) {
        LatrTbInsnMap *next = map->next;
        if (map->valid &&
            (map->code_base == start ||
             (map->code_base < end &&
              start < map->code_base + map->code_size))) {
            drop_map(t, map);
        }
        map = next;
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
    for (const LatrTbInsnMap *map = t->insn_map_list; map != NULL;
         map = map->next) {
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
 *
 * Returns false for NATIVE_INTERNAL rows: a fault there is a host bug and must
 * never be dressed up as a guest exception.
 *
 * Two rules make the writeback atomic:
 *   - the snapshot must cover what the span needs. A GPR binding with no
 *     `host_gprs` (or an FPR binding with no `host_fprs`) means the state is
 *     unknown, and applying only the reachable half would hand the guest a
 *     register file that never existed;
 *   - every binding is validated before the first store, so a bad row leaves
 *     env untouched instead of half-patched.
 */
bool latr_tb_recovery_apply(const LatrTbInsnSpan *span,
                            const LatrTbInsnMap *map,
                            const uint64_t host_gprs[32],
                            const uint64_t host_fprs[32],
                            CPUArchState *env)
{
    uint64_t gprs[32];
    uint64_t fprs[32];
    uint64_t gpr_mask = 0;
    uint64_t fpr_mask = 0;

    if (span == NULL || env == NULL || map == NULL ||
        span->site == LATR_RECOV_NATIVE_INTERNAL) {
        return false;
    }
    for (uint8_t i = 0; i < span->bind_cnt; i++) {
        const LatrRecoveryBinding *b = &map->binds[span->bind_first + i];

        if (b->guest_reg >= 32 || b->temp < 0 || b->temp >= 32) {
            return false;
        }
        if (b->is_fpr) {
            if (host_fprs == NULL) {
                return false;
            }
            fprs[b->guest_reg] = host_fprs[b->temp];
            fpr_mask |= UINT64_C(1) << b->guest_reg;
        } else {
            if (host_gprs == NULL) {
                return false;
            }
            gprs[b->guest_reg] = host_gprs[b->temp];
            gpr_mask |= UINT64_C(1) << b->guest_reg;
        }
    }
    for (unsigned reg = 0; reg < 32; reg++) {
        if (gpr_mask & (UINT64_C(1) << reg)) {
            env->gpr[reg] = gprs[reg];
        }
        if (fpr_mask & (UINT64_C(1) << reg)) {
            env->fpr[reg] = fprs[reg];
        }
    }
    env->pc = span->guest_pc;
    return true;
}
