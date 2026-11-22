/*
 * Coverage test: no decodable SVE instruction may become a silent no-op.
 *
 * The translator has three tiers -- LSX lowering, LASX lowering, scalar helper
 * -- and each may decline an instruction. Declining is fine as long as the next
 * tier handles it or translation fails outright, because the one outcome that
 * cannot be allowed is generated code that runs and leaves the destination
 * register untouched. The helper used to do exactly that: its final `default`
 * returned success for an instruction it never executed.
 *
 * So the invariant this file pins down is: for every encoding the decoder
 * accepts, the helper either implements it or says so. Walking the decoder's
 * own pattern table (via a64_sve_decode_corpus) means new decodable forms are
 * covered automatically -- adding a pattern without teaching the helper about
 * it fails here rather than in a guest.
 *
 * The same query is what the LASX and LSX fallbacks consult before emitting a
 * helper call, so a pass here means both SIMD tiers have a safe fallback.
 */
#include "sve-decode.h"
#include "sve-lasx-lower.h"
#include "sve-pvir.h"

#include <stdio.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static const char *support_name(LataSVEHelperSupport support)
{
    switch (support) {
    case LATA_SVE_HELPER_SUPPORTED:
        return "supported";
    case LATA_SVE_HELPER_UNSUPPORTED_OPCODE:
        return "unsupported-opcode";
    case LATA_SVE_HELPER_UNSUPPORTED_SHAPE:
        return "unsupported-shape";
    case LATA_SVE_HELPER_INVALID_PVIR:
        return "invalid-pvir";
    }
    return "?";
}

static void test_corpus_is_representative(void)
{
    static uint32_t corpus[512];
    size_t count = a64_sve_decode_corpus(corpus, sizeof(corpus) / sizeof(corpus[0]));

    /* A corpus of zero would make the rest of this file vacuous. The decoder
     * currently exposes ~90 distinct types out of ~218 patterns; require a
     * large fraction of that so a broken corpus is caught, without pinning the
     * exact number as the table grows. */
    CHECK(count >= 80);
    for (size_t i = 0; i < count; i++) {
        A64SVEIR1Instruction decoded;

        CHECK(a64_sve_decode(corpus[i], &decoded) == A64_SVE_DECODE_MATCH);
    }
    printf("sve-helper: corpus has %zu representative encodings\n", count);
}

/*
 * Every decodable type, decoded and lowered to PVIR, must be executable by the
 * helper. Types that produce no PVIR at all are not part of this tier (they are
 * translated directly), and are counted separately so the numbers add up.
 */
static void test_every_decodable_type_is_helper_supported(void)
{
    static uint32_t corpus[512];
    size_t count = a64_sve_decode_corpus(corpus, sizeof(corpus) / sizeof(corpus[0]));
    size_t lowered = 0;
    size_t skipped = 0;

    for (size_t i = 0; i < count; i++) {
        A64SVEIR1Instruction source;
        A64SVEPvirBuilder builder;
        PvirInstruction instruction;
        LataSVEHelperSupport support;

        CHECK(a64_sve_decode(corpus[i], &source) == A64_SVE_DECODE_MATCH);
        a64_sve_pvir_builder_init(&builder);
        if (a64_sve_build_pvir(&builder, &source, 256, &instruction) !=
            PVIR_STATUS_OK) {
            ++skipped;
            printf("sve-helper: insn_type %d has no PVIR form\n",
                   (int)source.insn_type);
            continue;
        }
        ++lowered;
        support = lata_sve_helper_support(&instruction, &source);
        if (support != LATA_SVE_HELPER_SUPPORTED) {
            fprintf(stderr,
                    "FAIL: %s (insn_type=%d, opcode=%d, word=0x%08x) is not "
                    "helper-supported: %s\n",
                    "decodable SVE type", (int)source.insn_type,
                    (int)instruction.opcode, corpus[i], support_name(support));
            ++failures;
        }
        ++checks;
    }
    CHECK(lowered >= 80);
    printf("sve-helper: %zu lowered, %zu produced no PVIR\n", lowered, skipped);
}

/*
 * The query must actually discriminate. If it returned SUPPORTED
 * unconditionally the coverage test above would pass while proving nothing.
 */
static void test_query_rejects_unknown_shapes(void)
{
    static uint32_t corpus[512];
    size_t count = a64_sve_decode_corpus(corpus, sizeof(corpus) / sizeof(corpus[0]));
    A64SVEIR1Instruction source = {0};
    PvirInstruction instruction;
    PvirInstruction permute = {0};
    bool have_permute = false;

    for (size_t i = 0; i < count && !have_permute; i++) {
        A64SVEPvirBuilder trial;

        CHECK(a64_sve_decode(corpus[i], &source) == A64_SVE_DECODE_MATCH);
        a64_sve_pvir_builder_init(&trial);
        if (a64_sve_build_pvir(&trial, &source, 256, &instruction) ==
                PVIR_STATUS_OK &&
            instruction.opcode == PVIR_OP_PERMUTE) {
            permute = instruction;
            have_permute = true;
        }
    }
    CHECK(have_permute);

    /* Force a permute kind the executor's if/else chain does not cover; without
     * the shape check the executor would store a zeroed vector and report OK. */
    permute.operation.permute.kind = (PvirPermuteKind)999;
    CHECK(lata_sve_helper_support(&permute, &source) !=
          LATA_SVE_HELPER_SUPPORTED);

    instruction.opcode = PVIR_OP_INVALID;
    CHECK(lata_sve_helper_support(&instruction, NULL) ==
          LATA_SVE_HELPER_INVALID_PVIR);

    /* A load needs its source instruction: only that describes the address. */
    instruction = (PvirInstruction){0};
    instruction.opcode = PVIR_OP_LOAD;
    CHECK(lata_sve_helper_support(&instruction, NULL) !=
          LATA_SVE_HELPER_SUPPORTED);
}

int main(void)
{
    test_corpus_is_representative();
    test_every_decodable_type_is_helper_supported();
    test_query_rejects_unknown_shapes();

    if (failures != 0) {
        fprintf(stderr, "sve-helper: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("sve-helper: %u checks passed\n", checks);
    return 0;
}
