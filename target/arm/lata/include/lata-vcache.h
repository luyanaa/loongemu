#ifndef LATA_VCACHE_H
#define LATA_VCACHE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Vector-state caching boundary.
 *
 * Invariant I0: at every TB boundary the only authoritative copy of the
 * architectural SVE/ASIMD state (Z0-Z31, P0-P15, FFR) is CPUARMState.sve.*.
 * A translation block may keep host-resident copies only as a borrow whose
 * lifetime is the TB itself, and every such copy must be written back before
 * the TB ends.
 *
 * Today no lowering keeps host-resident vector state, so the dirty mask must
 * always be zero when a TB ends.  The instrumentation below exists so that
 * (a) the invariant is checked instead of assumed, and (b) the vector env
 * traffic a future cache has to reduce is counted per TB.
 */

/* Written by the cache-aware env access helpers. */
void lata_vcache_begin_tb(void);
void lata_vcache_end_tb(void);
void lata_vcache_note_env_vector_read(void);
void lata_vcache_note_env_vector_write(void);
void lata_vcache_note_env_scalar_read(void);
void lata_vcache_note_env_scalar_write(void);

/* Borrow bookkeeping: a cached block marks its register dirty until flushed. */
void lata_vcache_mark_dirty(unsigned index);
void lata_vcache_clear_dirty(unsigned index);
uint64_t lata_vcache_dirty_mask(void);

/* Statistics for the current TB (reset by lata_vcache_begin_tb). */
uint32_t lata_vcache_vector_reads(void);
uint32_t lata_vcache_vector_writes(void);
uint32_t lata_vcache_scalar_reads(void);
uint32_t lata_vcache_scalar_writes(void);

/* True when no host-resident copy is outstanding (I0 holds right now). */
bool lata_vcache_clean(void);

/* Number of TB boundaries that observed a borrowed register (must stay 0). */
uint32_t lata_vcache_invariant_violations(void);

/*
 * Single 32-byte chunk cache.  The home register is LASX f15: guest V15 is
 * already env-backed (arm_la_fmap[15] == -1) and f15 is outside the f9-f14
 * temporary pool, so nothing else owns it.  Default off; a store always
 * invalidates, so no dirty state can survive a TB.
 */
bool lata_vcache_enabled(void);
void lata_vcache_set_enabled(bool enabled);
void lata_vcache_note_hit(void);
void lata_vcache_note_miss(void);
uint32_t lata_vcache_hits(void);
uint32_t lata_vcache_misses(void);
void lata_vcache_reset_chunk(void);
void lata_vcache_note_exit_hook(void);
/* Env vector writes that actually targeted a Z chunk (mask scratch excluded). */
void lata_vcache_note_z_write(void);
uint32_t lata_vcache_z_writes(void);
uint32_t lata_vcache_exit_hooks(void);

typedef struct LataVcacheSnapshot {
    bool in_tb;
    uint32_t exit_hooks;
    uint32_t z_writes;
    uint64_t dirty;
    uint32_t vector_reads;
    uint32_t vector_writes;
    uint32_t scalar_reads;
    uint32_t scalar_writes;
    uint32_t hits;
    uint32_t misses;
    uint32_t invariant_violations;
} LataVcacheSnapshot;

void lata_vcache_snapshot_take(LataVcacheSnapshot *snap);
void lata_vcache_snapshot_restore(const LataVcacheSnapshot *snap);

#endif
