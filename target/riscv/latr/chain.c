/*
 * Chain control state machine. See latr-chain.h and
 * docs/design/chain-control-ras.md.
 *
 * Deliberately free of machine and translator headers: the invariants are
 * "epoch moves on exactly these events" and "generation moves when the cache
 * changes", both of which a host test can drive directly.
 */
#include <string.h>
#include "latr-chain.h"

void latr_chain_init(LatrChainControl *chain, uint32_t max_chain)
{
    if (chain == 0) {
        return;
    }
    memset(chain, 0, sizeof(*chain));
    chain->max_chain = max_chain;
    soft_ras_init(&chain->ras);
}

void latr_chain_break(LatrChainControl *chain, LatrChainBreak reason)
{
    if (chain == 0 || (unsigned)reason >= LATR_CHAIN_BREAK_COUNT) {
        return;
    }
    chain->chain_epoch++;
    chain->breaks[reason]++;
}

void latr_chain_tb_changed(LatrChainControl *chain)
{
    if (chain != 0) {
        chain->tb_generation++;
    }
}

void latr_chain_record_length(LatrChainControl *chain, uint32_t length)
{
    unsigned bucket = 0;

    if (chain == 0) {
        return;
    }
    while (bucket < 7 && length >= (2u << bucket)) {
        bucket++;
    }
    chain->length_histogram[bucket]++;
}

uint64_t latr_chain_epoch(const LatrChainControl *chain)
{
    return chain == 0 ? 0 : chain->chain_epoch;
}

uint64_t latr_chain_tb_generation(const LatrChainControl *chain)
{
    return chain == 0 ? 0 : chain->tb_generation;
}

uint64_t latr_chain_links(const LatrChainControl *chain)
{
    return chain == 0 ? 0 : chain->links;
}

uint64_t latr_chain_breaks(const LatrChainControl *chain, LatrChainBreak reason)
{
    if (chain == 0 || (unsigned)reason >= LATR_CHAIN_BREAK_COUNT) {
        return 0;
    }
    return chain->breaks[reason];
}

uint64_t latr_chain_total_breaks(const LatrChainControl *chain)
{
    uint64_t total = 0;

    if (chain == 0) {
        return 0;
    }
    for (unsigned i = 0; i < LATR_CHAIN_BREAK_COUNT; i++) {
        total += chain->breaks[i];
    }
    return total;
}

const char *latr_chain_break_name(LatrChainBreak reason)
{
    switch (reason) {
    case LATR_CHAIN_BREAK_INTERRUPT: return "interrupt";
    case LATR_CHAIN_BREAK_SFENCE: return "sfence";
    case LATR_CHAIN_BREAK_FENCE_I: return "fence-i";
    case LATR_CHAIN_BREAK_WFI: return "wfi";
    case LATR_CHAIN_BREAK_EXCEPTION: return "exception";
    case LATR_CHAIN_BREAK_RECLAIM: return "reclaim";
    case LATR_CHAIN_BREAK_MISS: return "miss";
    case LATR_CHAIN_BREAK_BUDGET: return "budget";
    default: return "?";
    }
}
