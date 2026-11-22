#ifndef LA_PVIR_H
#define LA_PVIR_H

/*
 * ISA-independent Predicated Vector IR semantic core.
 *
 * PVIR owns no memory. Predicate bitsets and expanded-mask cache storage are
 * caller-owned and must outlive every use or cache hit. The word-count based
 * representation has no 64-lane limit; storage scales as ceil(lanes / 64).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PVIR_STATUS_OK = 0,
    PVIR_STATUS_INVALID_ARGUMENT,
    PVIR_STATUS_INVALID_PREDICATE,
    PVIR_STATUS_INSUFFICIENT_CAPACITY,
    PVIR_STATUS_NEEDS_VECTOR_MASK,
    PVIR_STATUS_UNSUPPORTED_OPERATION,
} PvirStatus;

typedef enum {
    PVIR_ELEMENT_INTEGER,
    PVIR_ELEMENT_FLOAT,
    PVIR_ELEMENT_BITS,
    /* Packed comparison result: one logical bit per lane. */
    PVIR_ELEMENT_MASK,
} PvirElementKind;

typedef enum {
    PVIR_SIGNLESS,
    PVIR_SIGNED,
    PVIR_UNSIGNED,
} PvirSignedness;

typedef struct {
    PvirElementKind kind;
    PvirSignedness signedness;
    uint16_t element_bits;
    size_t vector_bits;
} PvirVectorType;
/* MASK uses one packed bit per lane, so vector_bits is its lane count. */

/* Return the lane count after validating the element type and vector shape. */
PvirStatus pvir_vector_type_lanes(PvirVectorType type, size_t *lanes);

typedef enum {
    PVIR_PREDICATE_SOURCE_CONSTANT,
    PVIR_PREDICATE_SOURCE_PREDICATE,
    PVIR_PREDICATE_SOURCE_VECTOR,
} PvirPredicateSourceKind;

typedef struct {
    PvirPredicateSourceKind kind;
    /* PREDICATE/VECTOR identify a value; version distinguishes its writes. */
    uint32_t id;
    uint64_t version;
} PvirPredicateSource;

typedef enum {
    PVIR_PREDICATE_ALL_TRUE,
    PVIR_PREDICATE_ALL_FALSE,
    PVIR_PREDICATE_PREFIX,
    PVIR_PREDICATE_SUFFIX,
    PVIR_PREDICATE_BITSET,
    PVIR_PREDICATE_VECTOR_MASK,
} PvirPredicateForm;

typedef struct {
    const uint64_t *words;
    size_t word_count;
} PvirPredicateBitset;

/* first_lane selects a source slice; constructors default it to zero. */
typedef struct {
    PvirPredicateForm form;
    size_t first_lane;
    size_t lanes;
    PvirPredicateSource source;
    union {
        /* Number of active lanes, for PREFIX or SUFFIX. */
        size_t active_lanes;
        PvirPredicateBitset bitset;
    } value;
} PvirPredicate;

PvirPredicate pvir_predicate_all_true(size_t lanes);
PvirPredicate pvir_predicate_all_false(size_t lanes);
PvirPredicate pvir_predicate_prefix(PvirPredicateSource source,
                                    size_t lanes, size_t active_lanes);
PvirPredicate pvir_predicate_suffix(PvirPredicateSource source,
                                    size_t lanes, size_t active_lanes);
PvirPredicate pvir_predicate_bitset(PvirPredicateSource source, size_t lanes,
                                    const uint64_t *words, size_t word_count);
PvirPredicate pvir_predicate_vector_mask(PvirPredicateSource source,
                                         size_t lanes);

PvirStatus pvir_predicate_validate(const PvirPredicate *predicate);
/* Canonicalize bitsets and degenerate prefix/suffix forms in place. */
PvirStatus pvir_predicate_canonicalize(PvirPredicate *predicate);

/*
 * Expand one predicate slice to one all-zero or all-one uint64_t per lane.
 * first_lane is the source-vector offset; BITSET/PREFIX/SUFFIX values are
 * relative to the slice. VECTOR_MASK accepts packed words for a runtime
 * VECTOR or PREDICATE value; the input must cover ceil((first_lane + lanes) /
 * 64) words, while output capacity need only cover lanes. Input and output
 * storage must not overlap. No storage is allocated by PVIR.
 */
PvirStatus pvir_predicate_expand(const PvirPredicate *predicate,
                                 uint16_t element_bits,
                                 const uint64_t *source_mask_words,
                                 size_t source_mask_word_count,
                                 uint64_t *expanded,
                                 size_t expanded_capacity);

typedef enum {
    PVIR_OP_INVALID = 0,
    PVIR_OP_ADD,
    PVIR_OP_SUB,
    PVIR_OP_MUL,
    PVIR_OP_DIV,
    PVIR_OP_FMA,
    PVIR_OP_AND,
    PVIR_OP_OR,
    PVIR_OP_XOR,
    PVIR_OP_NOT,
    PVIR_OP_SHL,
    PVIR_OP_SHR,
    PVIR_OP_SAR,
    PVIR_OP_COMPARE,
    PVIR_OP_SELECT,
    PVIR_OP_LOAD,
    PVIR_OP_STORE,
    PVIR_OP_ARCH_STATE,
    PVIR_OP_PREDICATE_GENERATE,
    /* Scalar broadcast: every lane receives the SCALAR operand. */
    PVIR_OP_BROADCAST,
    /*
     * Additions driven by RVV. Appended after the SVE-era opcodes so the
     * existing numbering and semantics are frozen. Generic operations; no ISA
     * mode flag is implied.
     */
    /* Narrow an integer vector: operand width > result width, same lanes. */
    PVIR_OP_TRUNCATE,
    /* Widen an integer vector: operand width < result width, same lanes. */
    PVIR_OP_EXTEND,
    PVIR_OP_REM,
    PVIR_OP_MIN,
    PVIR_OP_MAX,
    /* Copy a vector value (identity element conversion, same type). */
    PVIR_OP_MOVE,
    /* Move one element between a vector and a scalar (PvirLaneOperation). */
    PVIR_OP_EXTRACT,
    PVIR_OP_INSERT,
    /* Integer/FP or FP/FP conversion (PvirConvertOperation). */
    PVIR_OP_CONVERT,
    /* Fixed-point/saturating integer arithmetic (PvirFixedPointOperation). */
    PVIR_OP_FIXED,
    /* FP arithmetic with explicit rounding (PvirFPOpOperation). */
    PVIR_OP_FP_ARITH,
    /* Generate a lane-index vector (PvirIndexOperation). */
    PVIR_OP_LANE_INDEX,
} PvirOpcode;

typedef enum {
    PVIR_INACTIVE_NOT_APPLICABLE,
    PVIR_INACTIVE_MERGE,
    PVIR_INACTIVE_ZERO,
    /* Inactive destination lanes are unspecified/don't-care. */
    PVIR_INACTIVE_UNKNOWN,
} PvirInactivePolicy;

#define PVIR_MAX_OPERANDS 4u
#define PVIR_NO_VALUE UINT32_MAX
typedef uint32_t PvirValueId;

typedef enum {
    PVIR_OPERAND_VECTOR,
    PVIR_OPERAND_SCALAR,
    PVIR_OPERAND_IMMEDIATE,
} PvirOperandKind;

/*
 * EQ/NE are type-independent. LT/LE/GT/GE are signed integer or IEEE
 * floating comparisons; the U variants are unsigned integer comparisons.
 * PVIR_ELEMENT_BITS supports EQ/NE only.
 */
typedef enum {
    PVIR_COMPARE_NONE,
    PVIR_COMPARE_EQ,
    PVIR_COMPARE_NE,
    PVIR_COMPARE_LT,
    PVIR_COMPARE_LE,
    PVIR_COMPARE_GT,
    PVIR_COMPARE_GE,
    PVIR_COMPARE_LTU,
    PVIR_COMPARE_LEU,
    PVIR_COMPARE_GTU,
    PVIR_COMPARE_GEU,
} PvirCompareCondition;

typedef struct {
    /* READ: output only; WRITE: input only; UPDATE: both; CLEAR: neither. */
    PvirValueId input_value;
    PvirValueId output_value;
    /* First lane cleared to known zero in cumulative-success output state. */
    PvirValueId clear_from_lane;
} PvirArchStateOperation;

typedef enum {
    PVIR_MEMORY_BASE_VALUE,
    PVIR_MEMORY_BASE_STACK_POINTER,
} PvirMemoryBaseKind;

typedef struct {
    bool present;
    PvirMemoryBaseKind base_kind;
    /* A 64-bit address value; PVIR_NO_VALUE when base_kind is STACK_POINTER. */
    PvirValueId base_value;
    int64_t displacement;
} PvirMemoryAddress;

/* COMPARE yields a packed same-lane PVIR_ELEMENT_MASK; FP compares are quiet. */
typedef struct {
    PvirVectorType type;
    PvirPredicate predicate;
    PvirInactivePolicy inactive_policy;
    PvirCompareCondition compare_condition;
    PvirValueId destination;
    /* VECTOR/SCALAR use value IDs; IMMEDIATE requires PVIR_NO_VALUE. */
    PvirValueId operands[PVIR_MAX_OPERANDS];
    /*
     * VECTOR: element_bits==0 inherits type and ignores the remaining fields;
     * explicit types must be valid and have VL lanes. Other kinds require
     * an all-zero type.
     */
    PvirVectorType operand_types[PVIR_MAX_OPERANDS];
    /* Scalar width is inferred from the operation/source element type. */
    PvirOperandKind operand_kinds[PVIR_MAX_OPERANDS];
    /* Raw immediate bits; checked against source width, zero otherwise. */
    uint64_t operand_immediates[PVIR_MAX_OPERANDS];
    uint8_t operand_count;
    /* Values consumed/produced by an optional architectural-state effect. */
    PvirArchStateOperation arch_state;
    /* Required for MERGE; otherwise must be PVIR_NO_VALUE. */
    PvirValueId merge_value;
    /* Memory address and access element size, used only by LOAD/STORE. */
    PvirMemoryAddress memory_address;
    uint16_t memory_element_bits;
} PvirVectorOperation;

typedef enum {
    PVIR_MEMORY_NONE,
    PVIR_MEMORY_READ,
    PVIR_MEMORY_WRITE,
    PVIR_MEMORY_READ_WRITE,
} PvirMemoryAccess;

typedef enum {
    PVIR_MEMORY_ORDER_UNSPECIFIED,
    PVIR_MEMORY_ORDER_RELAXED,
    PVIR_MEMORY_ORDER_ACQUIRE,
    PVIR_MEMORY_ORDER_RELEASE,
    PVIR_MEMORY_ORDER_ACQUIRE_RELEASE,
    PVIR_MEMORY_ORDER_SEQUENTIAL,
} PvirMemoryOrdering;

typedef enum {
    PVIR_INACTIVE_MEMORY_NOT_APPLICABLE,
    /*
     * Inactive lanes have no guest-visible architectural access/fault effect.
     * This does not promise that a speculative physical transaction is absent.
     */
    PVIR_INACTIVE_MEMORY_SUPPRESSED,
    /* A lowerer must not issue a physical transaction for inactive lanes. */
    PVIR_INACTIVE_MEMORY_NO_TRANSACTION,
} PvirInactiveMemoryEffect;

typedef struct {
    PvirMemoryAccess access;
    PvirMemoryOrdering ordering;
    PvirInactiveMemoryEffect inactive_lanes;
} PvirMemoryEffect;

typedef enum {
    /* No guest-visible fault; distinct from a fault suppressed by the op. */
    PVIR_FAULT_NONE,
    /* A fault is reported; lane_order may be unspecified or explicit. */
    PVIR_FAULT_PRECISE,
    /* First active-lane fault is precise; later faults after success are suppressed. */
    PVIR_FAULT_FIRST_FAULT,
    /* Faults are suppressed; result and optional state update are explicit. */
    PVIR_FAULT_NONFAULTING,
} PvirFaultBehavior;

typedef enum {
    /* No ordering guarantee. Precise faults may use this or lane order. */
    PVIR_FAULT_ORDER_UNSPECIFIED,
    PVIR_FAULT_ORDER_LANE_ASCENDING,
} PvirFaultOrder;

typedef enum {
    PVIR_FAULT_RESULT_UNSPECIFIED,
    PVIR_FAULT_RESULT_UNKNOWN,
    PVIR_FAULT_RESULT_ZERO,
    PVIR_FAULT_RESULT_MERGE,
    PVIR_FAULT_RESULT_VALUE,
} PvirFaultResultKind;

typedef struct {
    PvirFaultBehavior behavior;
    PvirFaultOrder lane_order;
    PvirFaultResultKind fault_result;
    /* VALUE uses fault_value; otherwise this field is ignored. */
    PvirValueId fault_value;
    /*
     * For FIRST_FAULT/NONFAULTING, this value identifies the first potentially
     * faulting active lane from which the loaded-vector result is unknown.
     * Results are also unknown for lanes whose input FFR bit was clear. This
     * boundary is separate from arch_state.clear_from_lane.
     */
    PvirValueId unknown_from_lane;
} PvirFaultEffect;

typedef uint32_t PvirSideEffectFlags;
enum {
    PVIR_SIDE_EFFECT_IO = 1u << 0,
    PVIR_SIDE_EFFECT_VOLATILE = 1u << 1,
    PVIR_SIDE_EFFECT_ATOMIC = 1u << 2,
    PVIR_SIDE_EFFECT_ORDERED = 1u << 3,
    PVIR_SIDE_EFFECT_BARRIER = 1u << 4,
};

typedef enum {
    /* The operation does not alter guest-visible FP status or raise FP traps. */
    PVIR_FP_EXCEPTIONS_NONE,
    /* Active lanes may affect guest FP status/traps; inactive lanes cannot. */
    PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY,
} PvirFPExceptionEffect;

typedef enum {
    PVIR_ARCH_STATE_DOMAIN_NONE,
    /* Generic first-fault state, suitable for SVE FFR-like state. */
    PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT,
    /* Generic restart/fault-progress state; no ISA-specific behavior implied. */
    PVIR_ARCH_STATE_DOMAIN_RESTART,
    PVIR_ARCH_STATE_DOMAIN_OTHER,
} PvirArchStateDomain;

typedef enum {
    PVIR_ARCH_STATE_ACTION_NONE,
    PVIR_ARCH_STATE_READ,
    PVIR_ARCH_STATE_WRITE,
    PVIR_ARCH_STATE_UPDATE,
    PVIR_ARCH_STATE_CLEAR,
} PvirArchStateAction;

typedef enum {
    PVIR_ARCH_STATE_UPDATE_NONE,
    /* Cumulative successes; state is cleared to known zero from the boundary. */
    PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS,
} PvirArchStateUpdateKind;

typedef struct {
    bool present;
    PvirArchStateDomain domain;
    PvirArchStateAction action;
    PvirArchStateUpdateKind update_kind;
    /* Opaque frontend-owned state slot and versions; PVIR assigns no ISA IDs. */
    uint32_t state_id;
    uint64_t version_before;
    uint64_t version_after;
} PvirArchStateEffect;

typedef enum {
    PVIR_PREDICATE_GENERATOR_CONSTANT,
    /* result[lane] = compare((start + lane) mod 2^scalar_bits, limit) */
    PVIR_PREDICATE_GENERATOR_LANE_COMPARE,
    /* Read a predicate-like architecture-state value into a PREDICATE. */
    PVIR_PREDICATE_GENERATOR_STATE_READ,
} PvirPredicateGeneratorKind;

/*
 * Generic predicate producer. CONSTANT represents all-true, all-false, or a
 * prefix; a FIRST_FAULT-domain constant write initializes an all-true state.
 * LANE_COMPARE produces a runtime mask from scalar bounds. STATE_READ consumes
 * source_state/source_state_value. destination is the produced value;
 * effects.arch_state records its architectural write, and status_state carries
 * any second architectural-state write, such as flags.
 */
typedef struct {
    PvirPredicateGeneratorKind kind;
    PvirValueId destination;
    PvirVectorType lane_type;
    uint32_t state_id;
    PvirPredicateForm form;
    size_t active_lanes;
    PvirCompareCondition compare_condition;
    uint16_t scalar_bits;
    PvirValueId scalar_start;
    PvirValueId scalar_limit;
    PvirArchStateEffect status_state;
    /* Optional state input consumed while producing the predicate value. */
    PvirArchStateEffect source_state;
    PvirValueId source_state_value;
} PvirPredicateGeneratorOperation;

typedef struct {
    PvirMemoryEffect memory;
    PvirFaultEffect fault;
    PvirSideEffectFlags side_effects;
    PvirFPExceptionEffect fp_exceptions;
    PvirArchStateEffect arch_state;
} PvirEffectMetadata;

typedef enum {
    /* No rounding control: the implementation's default; SVE-era ops use it. */
    PVIR_ROUND_DEFAULT,
    PVIR_ROUND_NE,
    PVIR_ROUND_RTZ,
    PVIR_ROUND_RDN,
    PVIR_ROUND_RUP,
    PVIR_ROUND_RMM,
    PVIR_ROUND_RNV,
    /* Fixed-point (RVV vxrm) rounding modes. */
    PVIR_ROUND_RNU, /* round to nearest, ties away from zero */
    PVIR_ROUND_ROD, /* round to odd */
} PvirRounding;

/*
 * Single-element transfer. EXTRACT reads lane `lane` of the vector value into
 * a scalar value; INSERT writes a scalar value into one lane and preserves the
 * remaining lanes from `merge_value` (an explicit old-vector value id).
 */
typedef struct {
    PvirValueId source;      /* vector for EXTRACT, scalar for INSERT */
    PvirValueId destination; /* scalar for EXTRACT, vector for INSERT */
    PvirValueId merge_value; /* INSERT only: lanes other than `lane` */
    PvirVectorType vector_type;
    uint16_t lane;           /* immediate element index, < lanes */
} PvirLaneOperation;

/*
 * Conversion between element representations; at least one side is FLOAT so
 * integer width changes stay in TRUNCATE/EXTEND. `rounding` applies when an FP
 * value is converted (FP to integer or FP to narrower/broader FP).
 */
typedef struct {
    PvirValueId source;
    PvirValueId destination;
    PvirVectorType source_type;
    PvirVectorType destination_type;
    PvirRounding rounding;
} PvirConvertOperation;

/*
 * Fixed-point and saturating integer operations (RVV vaadd/vssrl/vnclip/
 * vsadd/vsmul families). `rounding` applies to AVG_* and SCALE_SHIFT/CLIP;
 * `saturating` requires `sat_state.output_value` to carry the vxsat update.
 */
typedef enum {
    PVIR_FIXED_AVG_ADD,
    PVIR_FIXED_AVG_SUB,
    PVIR_FIXED_SAT_ADD,
    PVIR_FIXED_SAT_SUB,
    PVIR_FIXED_SAT_MUL,
    PVIR_FIXED_SCALE_SHIFT,
    PVIR_FIXED_CLIP,
} PvirFixedPointKind;

typedef struct {
    PvirVectorType type;
    PvirPredicate predicate;
    PvirInactivePolicy inactive_policy;
    PvirValueId destination;
    PvirValueId merge_value;
    PvirValueId operands[2];
    PvirOperandKind operand_kinds[2];
    uint64_t operand_immediates[2];
    PvirFixedPointKind kind;
    PvirRounding rounding;
    bool saturating;
    PvirArchStateOperation sat_state;
} PvirFixedPointOperation;

/* FP arithmetic with an explicit rounding mode; SQRT takes one operand. */
typedef enum {
    PVIR_FP_ADD,
    PVIR_FP_SUB,
    PVIR_FP_MUL,
    PVIR_FP_DIV,
    PVIR_FP_SQRT,
} PvirFPKind;

typedef struct {
    PvirVectorType type;
    PvirPredicate predicate;
    PvirInactivePolicy inactive_policy;
    PvirValueId destination;
    PvirValueId merge_value;
    PvirValueId operands[2];
    PvirOperandKind operand_kinds[2];
    PvirFPKind kind;
    PvirRounding rounding;
} PvirFPOpOperation;

/*
 * Lane-index vector: element i = start + i * step, where start is a scalar
 * value and step is a scalar value or PVIR_NO_VALUE for a constant 1.
 */
typedef struct {
    PvirVectorType type;
    PvirPredicate predicate;
    PvirInactivePolicy inactive_policy;
    PvirValueId destination;
    PvirValueId merge_value;
    PvirValueId start_value;
    PvirValueId step_value;
} PvirIndexOperation;

typedef struct {
    PvirOpcode opcode;
    PvirEffectMetadata effects;
    union {
        PvirVectorOperation vector;
        PvirArchStateOperation arch_state;
        PvirPredicateGeneratorOperation predicate_generator;
        PvirLaneOperation lane;
        PvirConvertOperation convert;
        PvirFixedPointOperation fixed;
        PvirFPOpOperation fp;
        PvirIndexOperation index;
    } operation;
} PvirInstruction;

/* Reject malformed shapes and inconsistent or lossy effect descriptions. */
PvirStatus pvir_instruction_validate(const PvirInstruction *instruction);

typedef struct {
    bool valid;
    PvirPredicateSource source;
    PvirPredicateForm predicate_form;
    /* Value payload for PREFIX/SUFFIX; zero for all other forms. */
    size_t active_lanes;
    uint16_t element_bits;
    size_t lanes;
    size_t first_lane;
    uint64_t *expanded_lanes;
    size_t capacity;
} PvirExpandedMaskCacheEntry;

typedef struct {
    PvirExpandedMaskCacheEntry *entries;
    size_t count;
    size_t next_victim;
} PvirExpandedMaskCache;

/* Caller-owned buffers must be disjoint; init rejects overlap and end overflow. */
PvirStatus pvir_expanded_mask_cache_init(PvirExpandedMaskCache *cache,
                                         PvirExpandedMaskCacheEntry *entries,
                                         size_t entry_count);
/* Invalidates every version/SEW cached for this source identity. */
void pvir_expanded_mask_cache_invalidate_source(PvirExpandedMaskCache *cache,
                                                PvirPredicateSourceKind kind,
                                                uint32_t id);
void pvir_expanded_mask_cache_invalidate_all(PvirExpandedMaskCache *cache);
/*
 * Cache key: source kind/id/version, predicate form/active count/first lane,
 * element width, and slice lane count. BITSET/VECTOR_MASK contents are
 * source-versioned. On a miss, source_mask_words supplies packed lane truths
 * for a VECTOR_MASK PREDICATE or VECTOR value.
 */
PvirStatus pvir_expanded_mask_cache_get(PvirExpandedMaskCache *cache,
                                        const PvirPredicate *predicate,
                                        uint16_t element_bits,
                                        const uint64_t *source_mask_words,
                                        size_t source_mask_word_count,
                                        const uint64_t **expanded_lanes);

#endif
