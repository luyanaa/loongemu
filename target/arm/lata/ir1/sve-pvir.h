#ifndef LATA_SVE_PVIR_H
#define LATA_SVE_PVIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sve-decode.h"


enum {
    A64_SVE_PVIR_NUM_ZREGS = 32,
    A64_SVE_PVIR_NUM_PREGS = 16,
    A64_SVE_PVIR_Z_VALUE_BASE = 0,
    A64_SVE_PVIR_P_VALUE_BASE = 32,
    A64_SVE_PVIR_GPR_VALUE_BASE = 64,
    A64_SVE_PVIR_FIRST_TEMP_VALUE = 96,
    A64_SVE_PVIR_P_STATE_BASE = 0x1000,
    A64_SVE_PVIR_STATUS_STATE = 0x1010,
    A64_SVE_PVIR_FFR_VALUE_BASE = 48,
    A64_SVE_PVIR_FFR_STATE = 0x1020,
};
/* Max VL 2048 bits has 256 byte lanes: four packed 64-bit words. */
#define A64_SVE_PVIR_MAX_MASK_KEY_WORDS 4u

/* Entry-state IDs are Z0-31, P0-15, then X0-31; X31 denotes the zero value. */

typedef struct {
    bool valid;
    PvirPredicateSource source;
    PvirPredicateForm form;
    size_t active_lanes;
    size_t bitset_word_count;
    uint64_t bitset_words[A64_SVE_PVIR_MAX_MASK_KEY_WORDS];
    size_t first_lane;
    size_t lanes;
    uint16_t element_bits;
    uint32_t vector_bits;
} A64SVEPredicateMaskCacheKey;

typedef struct A64SVEPvirBuilder {
    bool initialized;
    PvirValueId next_value_id;
    PvirValueId z_values[A64_SVE_PVIR_NUM_ZREGS];
    PvirValueId p_values[A64_SVE_PVIR_NUM_PREGS];
    uint64_t p_versions[A64_SVE_PVIR_NUM_PREGS];
    uint64_t status_version;
    PvirValueId ffr_value;
    uint64_t ffr_version;

    A64SVEPredicateMaskCacheKey expanded_mask_key;
    size_t expanded_mask_chunks_materialized;
    size_t expanded_mask_chunks_reused;
} A64SVEPvirBuilder;

typedef enum {
    A64_SVE_IR2_FEATURE_ABSENT,
    A64_SVE_IR2_ACCESS_UNSUPPORTED,
    A64_SVE_IR2_LASX_UNAVAILABLE,
    A64_SVE_IR2_READY,
} A64SVEIR2Dispatch;

/* Initialize caller-owned SSA/version state; no storage is allocated. */
void a64_sve_pvir_builder_init(A64SVEPvirBuilder *builder);

/* Build one supported decoded instruction as a validated PVIR instruction. */
PvirStatus a64_sve_build_pvir(A64SVEPvirBuilder *builder,
                              const A64SVEIR1Instruction *instruction,
                              uint32_t vl_bits,
                              PvirInstruction *out);

/* Access controls are checked before this helper consults the shared LASX API. */
A64SVEIR2Dispatch a64_sve_ir2_dispatch(
    A64SVEAccessDisposition access_disposition);

#endif
