/*
 * "Can the scalar helper run this?" -- answered without touching guest state.
 *
 * This lives in its own file so it can be linked into a host test: it depends
 * only on the PVIR and IR1 headers, not on CPUARMState, the translator or the
 * register allocator. The executor in sve-helper.c and the fallback decision in
 * sve-lasx-lower.c both consult it, and the coverage test walks every decodable
 * SVE encoding through it.
 *
 * Keep this in sync with sve-helper.c. The executor guards against drift: its
 * final default reports LATA_SVE_HELPER_UNSUPPORTED, and it checks this query
 * first, so a shape claimed here but not implemented there fails loudly at the
 * first execution instead of writing a stale register and returning success.
 */
#include "sve-lasx-lower.h"

#include <stddef.h>

/* Insn types with a dedicated implementation in the executor's first switch.
 * Everything else is expected to be described completely by PVIR. */
static bool helper_supports_source_insn(const A64SVEIR1Instruction *source)
{
    switch (source->insn_type) {
    /* Contiguous and fault-tolerant loads/stores. */
    case AARCH64_A64_SVE_LD1B:
    case AARCH64_A64_SVE_LD1H:
    case AARCH64_A64_SVE_LD1W:
    case AARCH64_A64_SVE_LD1D:
    case AARCH64_A64_SVE_ST1B:
    case AARCH64_A64_SVE_ST1H:
    case AARCH64_A64_SVE_ST1W:
    case AARCH64_A64_SVE_ST1D:
    case AARCH64_A64_SVE_LD1SB:
    case AARCH64_A64_SVE_LD1SH:
    case AARCH64_A64_SVE_LD1SW:
    case AARCH64_A64_SVE_LDFF1B:
    case AARCH64_A64_SVE_LDFF1H:
    case AARCH64_A64_SVE_LDFF1W:
    case AARCH64_A64_SVE_LDFF1D:
    case AARCH64_A64_SVE_LDFF1SB:
    case AARCH64_A64_SVE_LDFF1SH:
    case AARCH64_A64_SVE_LDFF1SW:
    case AARCH64_A64_SVE_LDNF1B:
    case AARCH64_A64_SVE_LDNF1H:
    case AARCH64_A64_SVE_LDNF1W:
    case AARCH64_A64_SVE_LDNF1D:
    case AARCH64_A64_SVE_LDNF1SB:
    case AARCH64_A64_SVE_LDNF1SH:
    case AARCH64_A64_SVE_LDNF1SW:
    case AARCH64_A64_SVE_LD1_SCALAR:
    case AARCH64_A64_SVE_ST1_SCALAR:
    case AARCH64_A64_SVE_LDN:
    case AARCH64_A64_SVE_STN:
    case AARCH64_A64_SVE_LD1R:
    case AARCH64_A64_SVE_LD1RQ:
    case AARCH64_A64_SVE_GATHER_LD:
    case AARCH64_A64_SVE_SCATTER_ST:
    case AARCH64_A64_SVE_LDNT1:
    case AARCH64_A64_SVE_STNT1:
    /* FFR and predicate bookkeeping. */
    case AARCH64_A64_SVE_SETFFR:
    case AARCH64_A64_SVE_WRFFR:
    case AARCH64_A64_SVE_RDFFR:
    case AARCH64_A64_SVE_PTEST:
    case AARCH64_A64_SVE_BRK:
    case AARCH64_A64_SVE_PFIRST:
    case AARCH64_A64_SVE_PNEXT:
    case AARCH64_A64_SVE_PRED_LOGICAL:
    case AARCH64_A64_SVE_CNTP:
    case AARCH64_A64_SVE_CNT_SCALAR:
    case AARCH64_A64_SVE_LAST:
    /* Integer forms whose operand mapping only the source knows. */
    case AARCH64_A64_SVE_DUP:
    case AARCH64_A64_SVE_FABS:
    case AARCH64_A64_SVE_FNEG:
    case AARCH64_A64_SVE_SHIFT_IMM:
    case AARCH64_A64_SVE_INT_UNARY:
    case AARCH64_A64_SVE_WMUL:
    case AARCH64_A64_SVE_IMM_BIN:
        return true;
    default:
        return false;
    }
}

static bool helper_supports_permute_kind(PvirPermuteKind kind)
{
    switch (kind) {
    case PVIR_PERMUTE_TBL:
    case PVIR_PERMUTE_TBX:
    case PVIR_PERMUTE_ZIP1:
    case PVIR_PERMUTE_ZIP2:
    case PVIR_PERMUTE_UZP1:
    case PVIR_PERMUTE_UZP2:
    case PVIR_PERMUTE_TRN1:
    case PVIR_PERMUTE_TRN2:
    case PVIR_PERMUTE_SPLICE:
    case PVIR_PERMUTE_EXT:
    case PVIR_PERMUTE_REV:
    case PVIR_PERMUTE_REVB:
    case PVIR_PERMUTE_REVH:
    case PVIR_PERMUTE_REVW:
    case PVIR_PERMUTE_COMPACT:
        return true;
    default:
        /* The executor's permute chain has no default branch: an unknown kind
         * would fall through and store a zeroed vector. */
        return false;
    }
}

LataSVEHelperSupport lata_sve_helper_support(
    const PvirInstruction *instruction, const A64SVEIR1Instruction *source)
{
    if (instruction == NULL) {
        return LATA_SVE_HELPER_INVALID_PVIR;
    }
    if (pvir_instruction_validate(instruction) != PVIR_STATUS_OK) {
        return LATA_SVE_HELPER_INVALID_PVIR;
    }
    if (source != NULL && helper_supports_source_insn(source)) {
        return LATA_SVE_HELPER_SUPPORTED;
    }

    switch (instruction->opcode) {
    case PVIR_OP_LOAD:
    case PVIR_OP_STORE:
        /* The memory path reads addresses, predicate register numbers and the
         * element size out of the source instruction; without it the helper
         * cannot know which address to touch. */
        return source != NULL ? LATA_SVE_HELPER_SUPPORTED
                              : LATA_SVE_HELPER_UNSUPPORTED_SHAPE;
    case PVIR_OP_PREDICATE_GENERATE: {
        const PvirPredicateGeneratorOperation *g =
            &instruction->operation.predicate_generator;

        /* whileXX compares lanes against two GPRs, which only the source
         * instruction describes. */
        if (g->kind == PVIR_PREDICATE_GENERATOR_LANE_COMPARE && source == NULL) {
            return LATA_SVE_HELPER_UNSUPPORTED_SHAPE;
        }
        switch (g->kind) {
        case PVIR_PREDICATE_GENERATOR_CONSTANT:
        case PVIR_PREDICATE_GENERATOR_LANE_COMPARE:
            return LATA_SVE_HELPER_SUPPORTED;
        default:
            /* Anything else copies FFR into a predicate register, which is
             * what the executor does for the remaining kinds. */
            return LATA_SVE_HELPER_SUPPORTED;
        }
    }
    case PVIR_OP_PERMUTE:
        return helper_supports_permute_kind(instruction->operation.permute.kind)
                   ? LATA_SVE_HELPER_SUPPORTED
                   : LATA_SVE_HELPER_UNSUPPORTED_SHAPE;
    case PVIR_OP_ARCH_STATE:
    case PVIR_OP_NOT:
    case PVIR_OP_BROADCAST:
    case PVIR_OP_COMPARE:
    case PVIR_OP_SELECT:
    case PVIR_OP_FMA:
    case PVIR_OP_FP_ARITH:
    case PVIR_OP_ADD:
    case PVIR_OP_SUB:
    case PVIR_OP_MUL:
    case PVIR_OP_DIV:
    case PVIR_OP_REM:
    case PVIR_OP_AND:
    case PVIR_OP_OR:
    case PVIR_OP_XOR:
    case PVIR_OP_SHL:
    case PVIR_OP_SHR:
    case PVIR_OP_SAR:
    case PVIR_OP_MIN:
    case PVIR_OP_MAX:
    case PVIR_OP_CONVERT:
    case PVIR_OP_FIXED:
    case PVIR_OP_REDUCE:
    case PVIR_OP_LANE_INDEX:
    case PVIR_OP_INSERT:
    case PVIR_OP_EXTRACT:
    case PVIR_OP_EXTEND:
    case PVIR_OP_MOVE:
    case PVIR_OP_TRUNCATE:
        return LATA_SVE_HELPER_SUPPORTED;
    case PVIR_OP_INVALID:
    default:
        return LATA_SVE_HELPER_UNSUPPORTED_OPCODE;
    }
}
