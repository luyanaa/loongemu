#ifndef LATR_CHAIN_H
#define LATR_CHAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "loongemu/soft_ras.h"

/*
 * Per-hart chain control (design: docs/design/chain-control-ras.md).
 *
 * A chain is a sequence of translated blocks entered without going back through
 * the full dispatcher. The only thing that makes a chain safe is that every
 * duty the dispatcher performs for a block is either (a) still performed, or
 * (b) provably not needed because nothing that duty would change has happened.
 * `chain_epoch` is the word that answers that: it is bumped by exactly the
 * events in `LatrChainBreak`, and by nothing else. A new event needs a new enum
 * value here, a row in the design table, and a test.
 *
 * P0 (this file, currently) is counters only: the machine bumps `chain_epoch`
 * and the per-reason buckets at the six real break points, records
 * `tb_generation` on translation-cache changes, and never links (links stays
 * 0). That makes P1's Phase A a change to the decision, not to the event
 * wiring, and it measures how often each break happens before anything depends
 * on it.
 */
typedef enum LatrChainBreak {
    LATR_CHAIN_BREAK_INTERRUPT = 0, /* an interrupt became pending/delivered */
    LATR_CHAIN_BREAK_SFENCE = 1,    /* sfence_pending: cross-core broadcast */
    LATR_CHAIN_BREAK_FENCE_I = 2,   /* fence_i_pending: cross-core broadcast */
    LATR_CHAIN_BREAK_WFI = 3,       /* wfi_pending: C-side idle handling */
    LATR_CHAIN_BREAK_EXCEPTION = 4, /* guest trap latched: C-side diagnostics */
    LATR_CHAIN_BREAK_RECLAIM = 5,   /* arena reclaimed: code may move */
    LATR_CHAIN_BREAK_MISS = 6,      /* lookup miss: must translate */
    LATR_CHAIN_BREAK_BUDGET = 7,    /* chain length policy exhausted */
    LATR_CHAIN_BREAK_COUNT = 8
} LatrChainBreak;

typedef struct LatrChainControl {
    /* Bumped by every event that invalidates "nothing changed since entry". */
    uint64_t chain_epoch;
    /* Bumped when this hart's TB cache gains or loses an entry. */
    uint64_t tb_generation;
    /* Chained dispatches taken; P0 must keep this at 0. */
    uint64_t links;
    /* Chain length histogram, 8 buckets (<2, <4, <8, <16, <32, <64, <128, +). */
    uint64_t length_histogram[8];
    uint64_t breaks[LATR_CHAIN_BREAK_COUNT];
    uint32_t max_chain; /* policy cap, 0 = chaining disabled */
    SoftRas ras;
} LatrChainControl;

void latr_chain_init(LatrChainControl *chain, uint32_t max_chain);
/* One break event: bumps the epoch and the reason bucket. */
void latr_chain_break(LatrChainControl *chain, LatrChainBreak reason);
/* The hart's TB cache changed (insert or invalidation). */
void latr_chain_tb_changed(LatrChainControl *chain);
/* Record a completed chain of `length` blocks (P1+; unused while links == 0). */
void latr_chain_record_length(LatrChainControl *chain, uint32_t length);

uint64_t latr_chain_epoch(const LatrChainControl *chain);
uint64_t latr_chain_tb_generation(const LatrChainControl *chain);
uint64_t latr_chain_links(const LatrChainControl *chain);
uint64_t latr_chain_breaks(const LatrChainControl *chain, LatrChainBreak reason);
uint64_t latr_chain_total_breaks(const LatrChainControl *chain);
const char *latr_chain_break_name(LatrChainBreak reason);

#endif
