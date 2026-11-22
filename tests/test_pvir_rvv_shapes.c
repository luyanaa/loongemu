/*
 * Shape validation for the planned RVV->PVIR descriptions. This does not run
 * vectors; it proves that the mapping shapes chosen in RVV_PVIR.md pass
 * pvir_instruction_validate(), so the plan cannot silently assume an
 * unsupported combination.
 */
#include <stdio.h>
#include "la-pvir.h"

/* Existing opcode numbers must never move; new RVV ops are appended after
 * PVIR_OP_BROADCAST so SVE descriptions keep their meaning. */
_Static_assert(PVIR_OP_ADD == 1, "PVIR opcode numbering is frozen");
_Static_assert(PVIR_OP_BROADCAST == 19, "new opcodes must be appended");
_Static_assert(PVIR_OP_TRUNCATE == 20 && PVIR_OP_EXTEND == 21 &&
               PVIR_OP_REM == 22 && PVIR_OP_MIN == 23 &&
               PVIR_OP_MAX == 24 && PVIR_OP_MOVE == 25 &&
               PVIR_OP_EXTRACT == 26 && PVIR_OP_INSERT == 27 &&
               PVIR_OP_CONVERT == 28 && PVIR_OP_FIXED == 29 &&
               PVIR_OP_FP_ARITH == 30 && PVIR_OP_LANE_INDEX == 31,
               "RVV opcodes keep their assigned numbers");
_Static_assert(PVIR_ROUND_RNU == 7 && PVIR_ROUND_ROD == 8,
               "fixed-point rounding modes keep their numbers");

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "PVIR-RVV: FAIL %s\n", name);                   \
        }                                                                   \
    } while (0)

static PvirPredicateSource mask_source = {
    PVIR_PREDICATE_SOURCE_VECTOR, 1, 7
};

/* SEW=32, LMUL=2 -> vector_bits=256, lanes=8 (VLEN=128). */
static PvirVectorType int_type(uint16_t bits, size_t vector_bits,
                               PvirSignedness sign)
{
    PvirVectorType type = { PVIR_ELEMENT_INTEGER, sign, bits, vector_bits };
    return type;
}

/* Zeroed instruction with the mandatory fault sentinel. */
static PvirInstruction plain_instruction(void)
{
    PvirInstruction in = {0};

    in.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    return in;
}

/* Starts from a masked vadd.vv and lets each case reshape it. */
static PvirInstruction masked_vadd(unsigned lanes)
{
    PvirInstruction in = {0};

    /* A zeroed struct is *not* a valid instruction: fault sentinels must be
     * PVIR_NO_VALUE (pvir.c valid_fault_effect). */
    in.effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    in.opcode = PVIR_OP_ADD;
    in.operation.vector.type = int_type(32, 256, PVIR_SIGNED);
    in.operation.vector.predicate = pvir_predicate_vector_mask(
        mask_source, lanes);
    in.operation.vector.inactive_policy = PVIR_INACTIVE_MERGE;
    in.operation.vector.merge_value = 10;
    in.operation.vector.destination = 9;
    in.operation.vector.operand_count = 2;
    in.operation.vector.operand_kinds[0] = PVIR_OPERAND_VECTOR;
    in.operation.vector.operand_kinds[1] = PVIR_OPERAND_VECTOR;
    in.operation.vector.operands[0] = 9;
    in.operation.vector.operands[1] = 2;
    return in;
}

int main(void)
{
    PvirInstruction in;

    in = masked_vadd(8);
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "masked vadd.vv with MERGE validates");
    CHECK(in.operation.vector.predicate.form == PVIR_PREDICATE_VECTOR_MASK,
          "RVV mask uses a runtime vector-mask predicate");

    in = masked_vadd(8);
    in.operation.vector.operand_kinds[1] = PVIR_OPERAND_SCALAR;
    in.operation.vector.operands[1] = 3;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vadd.vx broadcast-scalar operand validates");

    in = masked_vadd(8);
    in.operation.vector.operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
    in.operation.vector.operands[1] = PVIR_NO_VALUE;
    in.operation.vector.operand_immediates[1] = 5;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vadd.vi immediate operand validates");

    in = masked_vadd(8);
    in.opcode = PVIR_OP_COMPARE;
    {
        PvirVectorType mask_type = { PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 8 };
        PvirVectorType data_type = int_type(32, 256, PVIR_SIGNED);
        in.operation.vector.type = mask_type;
        in.operation.vector.operand_types[0] = data_type;
        in.operation.vector.operand_types[1] = data_type;
    }
    in.operation.vector.compare_condition = PVIR_COMPARE_EQ;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vmseq.vv packed-mask result validates");

    /* Widening: result 32-bit, operands 16-bit, same lane count. */
    in = masked_vadd(8);
    {
        PvirVectorType op16 = int_type(16, 128, PVIR_UNSIGNED);
        in.operation.vector.type = int_type(32, 256, PVIR_UNSIGNED);
        in.operation.vector.operand_types[0] = op16;
        in.operation.vector.operand_types[1] = op16;
    }
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vwadd.vv widening operand types validate");

    /* Narrowing shape: result 16-bit, source 32-bit, same lanes. Structure
     * only; the mixed-width semantics are an open lowering contract. */
    in = masked_vadd(8);
    {
        PvirVectorType op32 = int_type(32, 256, PVIR_UNSIGNED);
        in.opcode = PVIR_OP_SHR;
        in.operation.vector.type = int_type(16, 128, PVIR_UNSIGNED);
        in.operation.vector.operand_types[0] = op32;
        in.operation.vector.operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
        in.operation.vector.operands[1] = PVIR_NO_VALUE;
        in.operation.vector.operand_immediates[1] = 4;
    }
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vnsrl.wv narrowing shape validates");

    /* FMA shape for vmacc.vv: no operand_types override. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_FMA;
    in.operation.vector.operand_count = 3;
    in.operation.vector.operand_kinds[2] = PVIR_OPERAND_VECTOR;
    in.operation.vector.operands[2] = 4;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vmacc.vv three-vector FMA shape validates");

    /* vl=0 cannot be represented: the frontend must branch around it. */
    in = masked_vadd(0);
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "0-lane vector instruction is rejected");

    /* FP shape needs the active-lane exception policy. */
    in = masked_vadd(8);
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
    };
    in.operation.vector.operand_kinds[1] = PVIR_OPERAND_VECTOR;
    in.operation.vector.operands[1] = 2;
    in.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vfadd.vv FP shape validates with active-lane exceptions");

    /* ---- patterns that need no PVIR extension ---- */

    /* vmerge.vvm: OR(copy vs2, 0), predicate = vl&v0, merge = vs1. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_OR;
    in.operation.vector.operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
    in.operation.vector.operands[0] = 2;
    in.operation.vector.operands[1] = PVIR_NO_VALUE;
    in.operation.vector.operand_immediates[1] = 0;
    in.operation.vector.merge_value = 3; /* vs1 */
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vmerge copy+paced-merge idiom validates");

    /* Tail fixup: OR(old vd, 0), predicate = lane>=vl, merge = new vd. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_OR;
    in.operation.vector.operand_kinds[1] = PVIR_OPERAND_IMMEDIATE;
    in.operation.vector.operands[0] = 10; /* old vd */
    in.operation.vector.operands[1] = PVIR_NO_VALUE;
    in.operation.vector.operand_immediates[1] = 0;
    in.operation.vector.merge_value = 9; /* newly computed vd */
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vmerge tail-fix idiom validates");

    /* Mask destination keeps inactive lanes undisturbed. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_COMPARE;
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 8
    };
    in.operation.vector.operand_types[0] = int_type(32, 256, PVIR_SIGNED);
    in.operation.vector.operand_types[1] = int_type(32, 256, PVIR_SIGNED);
    in.operation.vector.compare_condition = PVIR_COMPARE_LT;
    in.operation.vector.merge_value = 6; /* old v0 */
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "mask destination with MERGE validates");

    /* Whole-register load (vl1re): BITS vector of 128 bits, bytes in memory. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_LOAD;
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 8, 128
    };
    in.operation.vector.predicate = pvir_predicate_all_true(16);
    in.operation.vector.inactive_policy = PVIR_INACTIVE_ZERO;
    in.operation.vector.merge_value = PVIR_NO_VALUE;
    in.operation.vector.destination = 7;
    in.operation.vector.operand_count = 0;
    in.operation.vector.memory_address.present = true;
    in.operation.vector.memory_address.base_kind = PVIR_MEMORY_BASE_VALUE;
    in.operation.vector.memory_address.base_value = 1;
    in.operation.vector.memory_element_bits = 8;
    in.effects.memory.access = PVIR_MEMORY_READ;
    in.effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    in.effects.memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "whole-register load (BITS) validates");

    /* vleff: NONFAULTING requires FIRST_FAULT + cumulative success. */
    in.effects.fault.behavior = PVIR_FAULT_NONFAULTING;
    in.effects.fault.fault_result = PVIR_FAULT_RESULT_UNKNOWN;
    in.effects.fault.unknown_from_lane = 3;
    in.effects.arch_state.present = true;
    in.effects.arch_state.domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    in.effects.arch_state.action = PVIR_ARCH_STATE_UPDATE;
    in.effects.arch_state.update_kind =
        PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS;
    in.effects.arch_state.state_id = 2;
    in.operation.vector.arch_state.input_value = 3;
    in.operation.vector.arch_state.output_value = 4;
    in.operation.vector.arch_state.clear_from_lane = 5;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "vleff maps onto NONFAULTING + FIRST_FAULT state");

    in.effects.arch_state.domain = PVIR_ARCH_STATE_DOMAIN_RESTART;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "NONFAULTING rejects the RESTART domain (documented constraint)");

    /* G5: mask load with byte-wise memory is not expressible yet. */
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 8
    };
    in.operation.vector.predicate = pvir_predicate_all_true(8);
    in.effects.fault.behavior = PVIR_FAULT_NONE;
    in.effects.arch_state.present = false;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "mask byte load is currently rejected (known gap G5)");

    /* ---- Batch B: integer-only additive ops ---- */

    /* TRUNCATE: result narrower than the operand, same lanes. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_TRUNCATE;
    in.operation.vector.type = int_type(16, 128, PVIR_UNSIGNED);
    in.operation.vector.operand_types[0] = int_type(32, 256, PVIR_UNSIGNED);
    in.operation.vector.operand_count = 1;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "TRUNCATE 32->16 validates");

    in = masked_vadd(8);
    in.opcode = PVIR_OP_TRUNCATE;
    in.operation.vector.type = int_type(32, 256, PVIR_UNSIGNED);
    in.operation.vector.operand_types[0] = int_type(16, 128, PVIR_UNSIGNED);
    in.operation.vector.operand_count = 1;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "TRUNCATE rejects a narrower operand");

    /* EXTEND: result wider than the operand; signedness comes from operand. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_EXTEND;
    in.operation.vector.type = int_type(32, 256, PVIR_SIGNED);
    in.operation.vector.operand_types[0] = int_type(16, 128, PVIR_SIGNED);
    in.operation.vector.operand_count = 1;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "EXTEND 16->32 validates");
    in.operation.vector.operand_types[0] = int_type(32, 256, PVIR_SIGNED);
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "EXTEND rejects equal widths");

    /* REM / MIN / MAX. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_REM;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK, "REM validates");
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
    };
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "REM rejects float elements");

    in = masked_vadd(8);
    in.opcode = PVIR_OP_MIN;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "MIN validates on integers");
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 32, 256
    };
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "MIN rejects bit elements");
    in = masked_vadd(8);
    in.opcode = PVIR_OP_MAX;
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
    };
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "MAX rejects floats (NaN rules need a dedicated op)");

    /* MOVE: identity copy, same vector type. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_MOVE;
    in.operation.vector.operand_count = 1;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK, "MOVE validates");
    in.operation.vector.operand_types[0] = int_type(16, 128, PVIR_SIGNED);
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "MOVE rejects a different element type");

    /* ---- Batch C: lane operations and conversions ---- */

    in = masked_vadd(8);
    in.opcode = PVIR_OP_EXTRACT;
    in.operation.lane.source = 9;
    in.operation.lane.destination = 11;
    in.operation.lane.merge_value = PVIR_NO_VALUE;
    in.operation.lane.vector_type = int_type(32, 256, PVIR_SIGNED);
    in.operation.lane.lane = 2;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "EXTRACT lane validates");
    in.operation.lane.lane = 8;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "EXTRACT rejects an out-of-range lane");
    in.operation.lane.lane = 2;
    in.operation.lane.merge_value = 12;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "EXTRACT rejects a merge value");

    in = masked_vadd(8);
    in.opcode = PVIR_OP_INSERT;
    in.operation.lane.source = 11;      /* scalar */
    in.operation.lane.destination = 9;  /* vector */
    in.operation.lane.merge_value = 10; /* preserved lanes */
    in.operation.lane.vector_type = int_type(32, 256, PVIR_SIGNED);
    in.operation.lane.lane = 3;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "INSERT lane validates");
    in.operation.lane.merge_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "INSERT requires the preserved-lane value");

    /* CONVERT: int->fp, fp->int, fp->fp; integer-only is rejected. */
    in = masked_vadd(8);
    in.opcode = PVIR_OP_CONVERT;
    in.operation.convert.source = 9;
    in.operation.convert.destination = 10;
    in.operation.convert.source_type = int_type(32, 256, PVIR_SIGNED);
    in.operation.convert.destination_type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 64, 512
    };
    in.operation.convert.rounding = PVIR_ROUND_NE;
    in.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "CONVERT int32->fp64 validates");
    in.operation.convert.source_type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
    };
    in.operation.convert.destination_type = int_type(16, 128, PVIR_SIGNED);
    in.operation.convert.rounding = PVIR_ROUND_RTZ;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "CONVERT fp32->int16 validates");
    in.operation.convert.source_type = int_type(32, 256, PVIR_SIGNED);
    in.operation.convert.destination_type = int_type(16, 128, PVIR_SIGNED);
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "CONVERT rejects integer width changes");
    in.operation.convert.source_type = int_type(32, 256, PVIR_SIGNED);
    in.operation.convert.destination_type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 64, 512
    };
    in.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_NONE;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "CONVERT requires the active-lane FP exception policy");

    /* ---- Batch D: packed-mask byte accesses (vlm.v/vsm.v) ---- */

    in = masked_vadd(8);
    in.opcode = PVIR_OP_LOAD;
    in.operation.vector.type = (PvirVectorType){
        PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 8
    };
    in.operation.vector.predicate = pvir_predicate_all_true(8);
    in.operation.vector.inactive_policy = PVIR_INACTIVE_ZERO;
    in.operation.vector.merge_value = PVIR_NO_VALUE;
    in.operation.vector.destination = 7;
    in.operation.vector.operand_count = 0;
    in.operation.vector.memory_address.present = true;
    in.operation.vector.memory_address.base_kind = PVIR_MEMORY_BASE_VALUE;
    in.operation.vector.memory_address.base_value = 1;
    in.operation.vector.memory_element_bits = 8;
    in.effects.memory.access = PVIR_MEMORY_READ;
    in.effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    in.effects.memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "mask byte load (vlm.v) validates");
    in.operation.vector.memory_element_bits = 16;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "mask load rejects non-byte elements");

    in.operation.vector.memory_element_bits = 8;
    in.opcode = PVIR_OP_STORE;
    in.operation.vector.inactive_policy = PVIR_INACTIVE_NOT_APPLICABLE;
    in.operation.vector.merge_value = PVIR_NO_VALUE;
    in.operation.vector.destination = PVIR_NO_VALUE;
    in.operation.vector.operand_count = 1;
    in.operation.vector.operand_kinds[0] = PVIR_OPERAND_VECTOR;
    in.operation.vector.operands[0] = 6;
    in.effects.memory.access = PVIR_MEMORY_WRITE;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "mask byte store (vsm.v) validates");

    /* ---- Batch E: fixed-point/saturating, FP rounding, lane index ---- */

    in = plain_instruction();
    {
        PvirFixedPointOperation *fp = &in.operation.fixed;
        in.opcode = PVIR_OP_FIXED;
        fp->type = int_type(32, 256, PVIR_UNSIGNED);
        fp->predicate = pvir_predicate_vector_mask(mask_source, 8);
        fp->inactive_policy = PVIR_INACTIVE_MERGE;
        fp->merge_value = 10;
        fp->destination = 9;
        fp->operands[0] = 9;
        fp->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        fp->operands[1] = 2;
        fp->operand_kinds[1] = PVIR_OPERAND_VECTOR;
        fp->kind = PVIR_FIXED_AVG_ADD;
        fp->rounding = PVIR_ROUND_RNU;
    }
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "fixed-point average add validates");
    in.operation.fixed.rounding = PVIR_ROUND_DEFAULT;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "fixed-point average requires a rounding mode");
    in.operation.fixed.rounding = PVIR_ROUND_RNU;
    in.operation.fixed.type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
    };
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "fixed-point rejects float elements");
    in.operation.fixed.type = int_type(32, 256, PVIR_UNSIGNED);
    in.operation.fixed.kind = PVIR_FIXED_SAT_ADD;
    in.operation.fixed.rounding = PVIR_ROUND_DEFAULT;
    in.operation.fixed.saturating = true;
    in.operation.fixed.sat_state.output_value = 12;
    in.effects.arch_state.present = true;
    in.effects.arch_state.domain = PVIR_ARCH_STATE_DOMAIN_OTHER;
    in.effects.arch_state.action = PVIR_ARCH_STATE_UPDATE;
    in.effects.arch_state.state_id = 3;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "saturating add with vxsat state validates");
    in.effects.arch_state.present = false;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "saturating add requires the vxsat state effect");

    /* FP arithmetic with explicit rounding (RVV frm), including SQRT. */
    in = plain_instruction();
    {
        PvirFPOpOperation *fp = &in.operation.fp;
        in.opcode = PVIR_OP_FP_ARITH;
        fp->type = (PvirVectorType){
            PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
        };
        fp->predicate = pvir_predicate_vector_mask(mask_source, 8);
        fp->inactive_policy = PVIR_INACTIVE_MERGE;
        fp->merge_value = 10;
        fp->destination = 9;
        fp->operands[0] = 9;
        fp->operand_kinds[0] = PVIR_OPERAND_VECTOR;
        fp->operands[1] = 2;
        fp->operand_kinds[1] = PVIR_OPERAND_SCALAR;
        fp->kind = PVIR_FP_ADD;
        fp->rounding = PVIR_ROUND_RMM;
    }
    in.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "FP add with explicit rounding validates");
    in.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_NONE;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "FP arithmetic requires active-lane exceptions");
    in.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    in.operation.fp.kind = PVIR_FP_SQRT;
    in.operation.fp.operands[1] = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "FP sqrt shape validates");
    in.operation.fp.operands[1] = 2;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "FP sqrt rejects a second operand");
    in.operation.fp.kind = PVIR_FP_ADD;
    in.operation.fp.type = int_type(32, 256, PVIR_SIGNED);
    in.operation.fp.operands[1] = 2;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "FP arithmetic rejects integer elements");

    /* Lane index (vid.v): start scalar, optional step scalar. */
    in = plain_instruction();
    {
        PvirIndexOperation *ix = &in.operation.index;
        in.opcode = PVIR_OP_LANE_INDEX;
        ix->type = int_type(32, 256, PVIR_UNSIGNED);
        ix->predicate = pvir_predicate_vector_mask(mask_source, 8);
        ix->inactive_policy = PVIR_INACTIVE_MERGE;
        ix->merge_value = 10;
        ix->destination = 9;
        ix->start_value = 3;
        ix->step_value = PVIR_NO_VALUE; /* constant 1 */
    }
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "lane index with constant step validates");
    in.operation.index.step_value = 4;
    CHECK(pvir_instruction_validate(&in) == PVIR_STATUS_OK,
          "lane index with a scalar step validates");
    in.operation.index.start_value = PVIR_NO_VALUE;
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "lane index requires a start value");
    in.operation.index.start_value = 3;
    in.operation.index.type = (PvirVectorType){
        PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256
    };
    CHECK(pvir_instruction_validate(&in) != PVIR_STATUS_OK,
          "lane index rejects float elements");

    printf("PVIR-RVV shapes: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
