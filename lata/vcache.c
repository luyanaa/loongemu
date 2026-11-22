#include <stdbool.h>
#include <stdint.h>

#include "lata-vcache.h"

/*
 * Single-threaded per-vCPU bookkeeping.  The translator runs on the vCPU that
 * owns the TB, so no locking is needed; the structure is static because LATA
 * keeps one translation context per core (lsenv is already per-core).
 */
static struct {
    /* Window on by default; tests may disable it. */
    bool enabled;
    uint32_t hits;
    uint32_t misses;
    bool in_tb;
    uint32_t exit_hooks;
    uint32_t z_writes;
    uint64_t dirty;
    uint32_t vector_reads;
    uint32_t vector_writes;
    uint32_t scalar_reads;
    uint32_t scalar_writes;
    uint32_t tb_count;
    uint32_t invariant_violations;
} vcache = { .enabled = true };

void lata_vcache_begin_tb(void)
{
    /*
     * A previous TB must not have left a borrowed register behind.  Count the
     * violation instead of asserting so a release build keeps translating and
     * the test suite reports the exact TB that broke I0.
     */
    if (vcache.dirty != 0) {
        vcache.invariant_violations++;
    }
    vcache.in_tb = true;
    vcache.dirty = 0;
    vcache.vector_reads = 0;
    vcache.vector_writes = 0;
    vcache.scalar_reads = 0;
    vcache.scalar_writes = 0;
    vcache.tb_count++;
}

void lata_vcache_end_tb(void)
{
    /*
     * With caching disabled nothing can be dirty: env was written directly by
     * the lowering.  When a cache is introduced, its write-back hook runs here
     * before the mask is cleared.
     */
    if (vcache.dirty != 0) {
        vcache.invariant_violations++;
        vcache.dirty = 0;
    }
    vcache.in_tb = false;
}

void lata_vcache_note_env_vector_read(void)
{
    vcache.vector_reads++;
}

void lata_vcache_note_env_vector_write(void)
{
    vcache.vector_writes++;
}

void lata_vcache_note_env_scalar_read(void)
{
    vcache.scalar_reads++;
}

void lata_vcache_note_env_scalar_write(void)
{
    vcache.scalar_writes++;
}

void lata_vcache_mark_dirty(unsigned index)
{
    if (index < 64) {
        vcache.dirty |= UINT64_C(1) << index;
    }
}

void lata_vcache_clear_dirty(unsigned index)
{
    if (index < 64) {
        vcache.dirty &= ~(UINT64_C(1) << index);
    }
}

uint64_t lata_vcache_dirty_mask(void)
{
    return vcache.dirty;
}

uint32_t lata_vcache_vector_reads(void)
{
    return vcache.vector_reads;
}

uint32_t lata_vcache_vector_writes(void)
{
    return vcache.vector_writes;
}

uint32_t lata_vcache_scalar_reads(void)
{
    return vcache.scalar_reads;
}

uint32_t lata_vcache_scalar_writes(void)
{
    return vcache.scalar_writes;
}

bool lata_vcache_enabled(void)
{
    return vcache.enabled;
}

void lata_vcache_set_enabled(bool enabled)
{
    vcache.enabled = enabled;
}

void lata_vcache_note_hit(void)
{
    vcache.hits++;
}

void lata_vcache_note_miss(void)
{
    vcache.misses++;
}

uint32_t lata_vcache_hits(void)
{
    return vcache.hits;
}

uint32_t lata_vcache_misses(void)
{
    return vcache.misses;
}

void lata_vcache_note_z_write(void)
{
    vcache.z_writes++;
}

uint32_t lata_vcache_z_writes(void)
{
    return vcache.z_writes;
}

void lata_vcache_note_exit_hook(void)
{
    vcache.exit_hooks++;
}

uint32_t lata_vcache_exit_hooks(void)
{
    return vcache.exit_hooks;
}

/* Chunk bookkeeping is owned by lata.c; nothing to flush when read-only. */
void lata_vcache_reset_chunk(void)
{
}

bool lata_vcache_clean(void)
{
    return vcache.dirty == 0;
}

uint32_t lata_vcache_invariant_violations(void)
{
    return vcache.invariant_violations;
}

void lata_vcache_snapshot_take(LataVcacheSnapshot *snap)
{
    if (!snap) {
        return;
    }
    snap->in_tb = vcache.in_tb;
    snap->exit_hooks = vcache.exit_hooks;
    snap->z_writes = vcache.z_writes;
    snap->dirty = vcache.dirty;
    snap->vector_reads = vcache.vector_reads;
    snap->vector_writes = vcache.vector_writes;
    snap->scalar_reads = vcache.scalar_reads;
    snap->scalar_writes = vcache.scalar_writes;
    snap->hits = vcache.hits;
    snap->misses = vcache.misses;
    snap->invariant_violations = vcache.invariant_violations;
}

void lata_vcache_snapshot_restore(const LataVcacheSnapshot *snap)
{
    if (!snap) {
        return;
    }
    vcache.in_tb = snap->in_tb;
    vcache.exit_hooks = snap->exit_hooks;
    vcache.z_writes = snap->z_writes;
    vcache.dirty = snap->dirty;
    vcache.vector_reads = snap->vector_reads;
    vcache.vector_writes = snap->vector_writes;
    vcache.scalar_reads = snap->scalar_reads;
    vcache.scalar_writes = snap->scalar_writes;
    vcache.hits = snap->hits;
    vcache.misses = snap->misses;
    vcache.invariant_violations = snap->invariant_violations;
}
