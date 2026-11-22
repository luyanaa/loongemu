#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lata-vcache.h"

#if defined(LATA_ENABLE_TLS)
#define LATA_VCACHE_TLS _Thread_local
#else
#define LATA_VCACHE_TLS
#endif

/*
 * Per-vCPU bookkeeping bound via lata_vcache_bind_state() from LataContext.
 * Standalone unit tests (test_vcache) run with default_vcache when unbound.
 */
static LataVcacheState default_vcache = { .enabled = true };
static LATA_VCACHE_TLS LataVcacheState *bound_vcache;

static inline LataVcacheState *vcache_state(void)
{
    return bound_vcache != NULL ? bound_vcache : &default_vcache;
}

void lata_vcache_state_init(LataVcacheState *state, bool enabled)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->enabled = enabled;
}

void lata_vcache_bind_state(LataVcacheState *state)
{
    bound_vcache = state;
}

LataVcacheState *lata_vcache_current_state(void)
{
    return vcache_state();
}

void lata_vcache_begin_tb(void)
{
    LataVcacheState *v = vcache_state();

    /*
     * A previous TB must not have left a borrowed register behind.  Count the
     * violation instead of asserting so a release build keeps translating and
     * the test suite reports the exact TB that broke I0.
     */
    if (v->dirty != 0) {
        v->invariant_violations++;
    }
    v->in_tb = true;
    v->dirty = 0;
    v->vector_reads = 0;
    v->vector_writes = 0;
    v->scalar_reads = 0;
    v->scalar_writes = 0;
    v->tb_count++;
}

void lata_vcache_end_tb(void)
{
    LataVcacheState *v = vcache_state();

    if (v->dirty != 0) {
        v->invariant_violations++;
        v->dirty = 0;
    }
    v->in_tb = false;
}

void lata_vcache_note_env_vector_read(void)
{
    vcache_state()->vector_reads++;
}

void lata_vcache_note_env_vector_write(void)
{
    vcache_state()->vector_writes++;
}

void lata_vcache_note_env_scalar_read(void)
{
    vcache_state()->scalar_reads++;
}

void lata_vcache_note_env_scalar_write(void)
{
    vcache_state()->scalar_writes++;
}

void lata_vcache_mark_dirty(unsigned index)
{
    if (index < 64) {
        vcache_state()->dirty |= UINT64_C(1) << index;
    }
}

void lata_vcache_clear_dirty(unsigned index)
{
    if (index < 64) {
        vcache_state()->dirty &= ~(UINT64_C(1) << index);
    }
}

uint64_t lata_vcache_dirty_mask(void)
{
    return vcache_state()->dirty;
}

uint32_t lata_vcache_vector_reads(void)
{
    return vcache_state()->vector_reads;
}

uint32_t lata_vcache_vector_writes(void)
{
    return vcache_state()->vector_writes;
}

uint32_t lata_vcache_scalar_reads(void)
{
    return vcache_state()->scalar_reads;
}

uint32_t lata_vcache_scalar_writes(void)
{
    return vcache_state()->scalar_writes;
}

bool lata_vcache_enabled(void)
{
    return vcache_state()->enabled;
}

void lata_vcache_set_enabled(bool enabled)
{
    vcache_state()->enabled = enabled;
}

void lata_vcache_note_hit(void)
{
    vcache_state()->hits++;
}

void lata_vcache_note_miss(void)
{
    vcache_state()->misses++;
}

uint32_t lata_vcache_hits(void)
{
    return vcache_state()->hits;
}

uint32_t lata_vcache_misses(void)
{
    return vcache_state()->misses;
}

void lata_vcache_note_z_write(void)
{
    vcache_state()->z_writes++;
}

uint32_t lata_vcache_z_writes(void)
{
    return vcache_state()->z_writes;
}

void lata_vcache_note_exit_hook(void)
{
    vcache_state()->exit_hooks++;
}

uint32_t lata_vcache_exit_hooks(void)
{
    return vcache_state()->exit_hooks;
}

/* Chunk bookkeeping is owned by lata.c; nothing to flush here. */
void lata_vcache_reset_chunk(void)
{
}

bool lata_vcache_clean(void)
{
    return vcache_state()->dirty == 0;
}

uint32_t lata_vcache_invariant_violations(void)
{
    return vcache_state()->invariant_violations;
}

void lata_vcache_snapshot_take(LataVcacheSnapshot *snap)
{
    const LataVcacheState *v = vcache_state();

    if (!snap) {
        return;
    }
    snap->in_tb = v->in_tb;
    snap->exit_hooks = v->exit_hooks;
    snap->z_writes = v->z_writes;
    snap->dirty = v->dirty;
    snap->vector_reads = v->vector_reads;
    snap->vector_writes = v->vector_writes;
    snap->scalar_reads = v->scalar_reads;
    snap->scalar_writes = v->scalar_writes;
    snap->hits = v->hits;
    snap->misses = v->misses;
    snap->invariant_violations = v->invariant_violations;
}

void lata_vcache_snapshot_restore(const LataVcacheSnapshot *snap)
{
    LataVcacheState *v = vcache_state();

    if (!snap) {
        return;
    }
    v->in_tb = snap->in_tb;
    v->exit_hooks = snap->exit_hooks;
    v->z_writes = snap->z_writes;
    v->dirty = snap->dirty;
    v->vector_reads = snap->vector_reads;
    v->vector_writes = snap->vector_writes;
    v->scalar_reads = snap->scalar_reads;
    v->scalar_writes = snap->scalar_writes;
    v->hits = snap->hits;
    v->misses = snap->misses;
    v->invariant_violations = snap->invariant_violations;
}
