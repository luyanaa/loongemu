#include "pvir-plan.h"

PvirStatus pvir_plan_chunks(PvirPlan *plan, uint32_t span_bytes, bool has_lasx,
                            PvirChunkLegalFn legal, void *ctx)
{
    uint32_t byte;
    unsigned count = 0;
    unsigned lasx = 0;
    unsigned lsx = 0;

    if (plan == NULL || span_bytes == 0 || (span_bytes & 15u) != 0) {
        return PVIR_STATUS_INVALID_ARGUMENT;
    }

    plan->span_bytes = span_bytes;
    plan->chunk_count = 0;
    plan->lasx_chunks = 0;
    plan->lsx_chunks = 0;
    /* mask fields are caller-owned; pvir_plan_chunks only schedules chunks. */

    for (byte = 0; byte < span_bytes;) {
        unsigned width = 16u;
        bool use_lasx = has_lasx && (span_bytes - byte) >= 32u &&
                        (byte & 31u) == 0 &&
                        (legal == NULL || legal(ctx, byte, 32u));
        if (use_lasx) {
            width = 32u;
        } else if (legal != NULL && !legal(ctx, byte, 16u)) {
            /* A step the port forbids at every width cannot be lowered. */
            return PVIR_STATUS_UNSUPPORTED_OPERATION;
        }

        if (count >= sizeof(plan->steps) / sizeof(plan->steps[0])) {
            return PVIR_STATUS_INSUFFICIENT_CAPACITY;
        }
        plan->steps[count].offset = (uint16_t)byte;
        plan->steps[count].size = (uint16_t)width;
        count++;
        if (use_lasx) {
            lasx++;
        } else {
            lsx++;
        }
        byte += width;
    }
    plan->chunk_count = (uint8_t)count;
    plan->lasx_chunks = (uint8_t)lasx;
    plan->lsx_chunks = (uint8_t)lsx;
    return PVIR_STATUS_OK;
}
bool pvir_plan_mask_key_from_operation(const PvirVectorOperation *operation,
                                       PvirPredicateMaskKey *key)
{
    const PvirPredicate *predicate;

    if (operation == NULL || key == NULL) {
        return false;
    }
    predicate = &operation->predicate;

    *key = (PvirPredicateMaskKey){0};
    key->source = predicate->source;
    key->form = predicate->form;
    key->first_lane = predicate->first_lane;
    key->lanes = predicate->lanes;
    key->element_bits = operation->type.element_bits;
    key->vector_bits = operation->type.vector_bits;

    switch (predicate->form) {
    case PVIR_PREDICATE_PREFIX:
    case PVIR_PREDICATE_SUFFIX:
        key->active_lanes = predicate->value.active_lanes;
        break;
    case PVIR_PREDICATE_BITSET:
        if (predicate->value.bitset.word_count >
                PVIR_PLAN_MAX_MASK_KEY_WORDS ||
            (predicate->value.bitset.word_count != 0 &&
             predicate->value.bitset.words == NULL)) {
            return false;
        }
        key->bitset_word_count = predicate->value.bitset.word_count;
        for (size_t i = 0; i < key->bitset_word_count; ++i) {
            key->bitset_words[i] = predicate->value.bitset.words[i];
        }
        break;
    case PVIR_PREDICATE_ALL_TRUE:
    case PVIR_PREDICATE_ALL_FALSE:
    case PVIR_PREDICATE_VECTOR_MASK:
        break;
    default:
        return false;
    }
    key->valid = true;
    return true;
}

bool pvir_plan_mask_keys_equal(const PvirPredicateMaskKey *left,
                               const PvirPredicateMaskKey *right)
{
    if (left == NULL || right == NULL || !left->valid || !right->valid ||
        left->source.kind != right->source.kind ||
        left->source.id != right->source.id ||
        left->source.version != right->source.version ||
        left->form != right->form ||
        left->active_lanes != right->active_lanes ||
        left->bitset_word_count != right->bitset_word_count ||
        left->first_lane != right->first_lane ||
        left->lanes != right->lanes ||
        left->element_bits != right->element_bits ||
        left->vector_bits != right->vector_bits) {
        return false;
    }
    for (size_t i = 0; i < PVIR_PLAN_MAX_MASK_KEY_WORDS; ++i) {
        if (left->bitset_words[i] != right->bitset_words[i]) {
            return false;
        }
    }
    return true;
}

