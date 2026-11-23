#ifndef PVIR_PLAN_H
#define PVIR_PLAN_H

/*
 * Shared PVIR lowering plan: the chunk/alias/cost schedule that both LATR
 * (RVV -> LSX/LASX) and LATA (SVE -> LASX) currently re-implement. This is a
 * *lowering* plan, not IR semantics — it lives beside la-pvir.h but never
 * inside the operand/predicate model.
 *
 * The planner is a pure function over (span_bytes, has_lasx, a per-port
 * legality predicate). It emits a step schedule the caller replays to emit
 * la_v* (16B) and la_xv* (32B) chunks. It owns no memory and emits no code.
 */
#include "la-pvir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One vector step in the schedule. `size` is 16 (LSX) or 32 (LASX); `offset`
 * is the byte offset into the logical vector operation. */
typedef struct PvirChunkStep {
    uint16_t offset;
    uint16_t size;
} PvirChunkStep;

/* Canonical identity of a materialized predicate mask. Two operations may
 * share the already-loaded expanded mask only when their keys are equal.
 * Moved out of LATA's A64SVEPredicateMaskCacheKey into shared planning; it is
 * a planner input, not a second cache. bitset_words is fixed-size because the
 * mask is materialized to at most PVIR_PLAN_MAX_MASK_KEY_WORDS words. */
#define PVIR_PLAN_MAX_MASK_KEY_WORDS 4u
typedef struct PvirPredicateMaskKey {
    bool valid;
    PvirPredicateSource source;
    PvirPredicateForm form;
    size_t active_lanes;
    size_t bitset_word_count;
    uint64_t bitset_words[PVIR_PLAN_MAX_MASK_KEY_WORDS];
    size_t first_lane;
    size_t lanes;
    uint16_t element_bits;
    uint32_t vector_bits;
} PvirPredicateMaskKey;

typedef struct PvirPlan {
    uint32_t span_bytes;      /* bytes this op spans after tail policy */
    uint8_t  chunk_count;     /* number of valid entries in steps[] */
    uint8_t  lasx_chunks;     /* count of size==32 steps (stat) */
    uint8_t  lsx_chunks;      /* count of size==16 steps (stat) */
    bool     mask_reusable;   /* mask_key matches the live materialized mask */
    PvirPredicateMaskKey mask_key;
    PvirChunkStep steps[16];  /* <= VLMAX*8/16 for RVV, <= SVE_MAX for LATA */
} PvirPlan;

/* Build the canonical key for an operation's predicate. Returns false when the
 * predicate form can't be keyed (e.g. an oversized bitset). */
bool pvir_plan_mask_key_from_operation(const PvirVectorOperation *operation,
                                       PvirPredicateMaskKey *key);

/* Exact structural equality; invalid keys never match. */
bool pvir_plan_mask_keys_equal(const PvirPredicateMaskKey *left,
                               const PvirPredicateMaskKey *right);

/* Per-port legality: may a `width`-byte vector operation be issued at byte
 * `offset`? Closes over the operand registers and op family:
 *  - RVV: enforces the LMUL even-pair rule (vd/a/b + reg_idx must be even).
 *  - SVE: no pairing; effectively has_lasx && 32B-aligned.
 * Return false to force the planner to a smaller (16B) step. */
typedef bool (*PvirChunkLegalFn)(void *ctx, unsigned offset, unsigned width);

/* Greedy schedule: at each offset prefer a 32B LASX step when `has_lasx` and
 * legal(offset,32) and the offset is 32B-aligned and >=32B remain; otherwise a
 * 16B LSX step. Returns INSUFFICIENT_CAPACITY if the schedule would overflow
 * steps[]. `span_bytes` must already be rounded to the 16B granularity and
 * non-zero. */
PvirStatus pvir_plan_chunks(PvirPlan *plan, uint32_t span_bytes, bool has_lasx,
                            PvirChunkLegalFn legal, void *ctx);

#endif /* PVIR_PLAN_H */
