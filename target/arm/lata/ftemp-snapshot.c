/*
 * Temp-pool snapshot for IR2 rollback.
 *
 * The allocator grows the temporary pool whenever it spills a resident guest
 * vector, and that growth is permanent for the TB.  A lowering that spills and
 * then rolls back its IR2 must therefore undo the growth as well, otherwise the
 * phy<->slot maps outlive the instructions that justified them.  Kept in its
 * own TU so the standalone context test can exercise it without linking the
 * whole translator.
 */
#include "lata.h"
#include <stdbool.h>

void lata_ftemp_pool_snapshot(LataContext *ctx, LataIr2Snapshot *snap)
{
    if (ctx == NULL || snap == NULL) {
        return;
    }
    snap->ftemp_pool_entries = ctx->ftemp_pool_entries;
    snap->ftemp_pool_base = ctx->ftemp_base;
    for (int i = 0; i < LATA_FTEMP_POOL_ENTRIES; ++i) {
        snap->ftemp_pool_map[i] = ctx->reg_ftemp_map[i];
        snap->ftemp_pool_reverse_map[i] = ctx->reg_ftemp_reverse_map[i];
    }
}

void lata_ftemp_pool_rollback(LataContext *ctx, const LataIr2Snapshot *snap)
{
    if (ctx == NULL || snap == NULL) {
        return;
    }
    ctx->ftemp_pool_entries = snap->ftemp_pool_entries;
    ctx->ftemp_base = snap->ftemp_pool_base;
    for (int i = 0; i < LATA_FTEMP_POOL_ENTRIES; ++i) {
        ctx->reg_ftemp_map[i] = snap->ftemp_pool_map[i];
        ctx->reg_ftemp_reverse_map[i] = snap->ftemp_pool_reverse_map[i];
    }
}

/* Test/debug helper: is the live pool exactly what the snapshot captured? */
bool lata_ir2_ftemp_pool_state_matches(const LataIr2Snapshot *snap)
{
    LataContext *ctx = lata_context_current();

    if (ctx == NULL || snap == NULL) {
        return false;
    }
    if (ctx->ftemp_pool_entries != snap->ftemp_pool_entries ||
        ctx->ftemp_base != snap->ftemp_pool_base) {
        return false;
    }
    for (int i = 0; i < LATA_FTEMP_POOL_ENTRIES; ++i) {
        if (ctx->reg_ftemp_map[i] != snap->ftemp_pool_map[i] ||
            ctx->reg_ftemp_reverse_map[i] != snap->ftemp_pool_reverse_map[i]) {
            return false;
        }
    }
    return true;
}
