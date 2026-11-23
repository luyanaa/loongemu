/*
 * IR2 rollback snapshot, kept out of lata.h so a bare-metal harness can
 * include it without pulling the translator's CPU headers.
 */
#ifndef LATA_SNAPSHOT_H
#define LATA_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#include "lata-vcache.h"

/* Temp-pool slots tracked by the snapshot (one per LSX/LASX float temp). */
#define LATA_FTEMP_POOL_ENTRIES 32

typedef struct LataIr2Snapshot {
    int ir2_inst_num_current;
    int real_ir2_inst_num;
    int first_ir2_id;
    int last_ir2_id;
    int last_ir2_next;
    int label_num;
    uint32_t all_temp_status;
    uint32_t vreg_resident_touched;
    uint32_t vreg_resident_dirty;
    uint32_t vreg_v_written_mask;
    uint32_t vreg_tb_resident;
    uint32_t w_write_flag;
    bool ftemp_alloc_failed;
    struct {
        bool valid;
        bool dirty;
        int offset;
    } zcache_slots[2];
    unsigned zcache_victim;
    LataVcacheSnapshot vcache_snap;
    /* Temp pool may have grown by spilling resident vectors; rollback must
     * undo that growth or later allocations inherit stale phy<->slot maps. */
    int ftemp_pool_entries;
    int ftemp_pool_base;
    int ftemp_pool_map[LATA_FTEMP_POOL_ENTRIES];
    int ftemp_pool_reverse_map[LATA_FTEMP_POOL_ENTRIES];
} LataIr2Snapshot;

struct LataContext;
void lata_ir2_snapshot_take(LataIr2Snapshot *snap);
void lata_ir2_snapshot_rollback(const LataIr2Snapshot *snap);
void lata_ftemp_pool_snapshot(struct LataContext *ctx, LataIr2Snapshot *snap);
void lata_ftemp_pool_rollback(struct LataContext *ctx,
                              const LataIr2Snapshot *snap);
bool lata_ir2_ftemp_pool_state_matches(const LataIr2Snapshot *snap);

#endif
