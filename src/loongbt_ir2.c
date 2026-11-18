/* IR2 opcode integration: the generated LISA_* enumeration is the contract
 * between the LATX emitter and this library.  Mapping is mechanical: the
 * back end chosen at run time (LBT hardware vs integer fallback) is invisible
 * here, which is what lets the same IR2 stream run on 2K1000LA/2K2000.
 *
 * LISA_X86_INST is a marker and LISA_X86SETTAG needs the x87 tag-word state,
 * which the portable core does not model yet; both report unsupported. */
#include "loongemu/loongbt.h"
#include "loongemu/loongbt_ir2_opcodes.h"

bool loongbt_op_from_ir2(uint32_t lisa_opcode, LoongbtOp *op, LoongbtWidth *width)
{
    switch (lisa_opcode) {
    case LISA_X86MTTOP: *op = LOONGBT_MTTOP; *width = LOONGBT_W; return true;
    case LISA_X86MFTOP: *op = LOONGBT_MFTOP; *width = LOONGBT_W; return true;
    case LISA_X86LOOPE: *op = LOONGBT_LOOPE; *width = LOONGBT_W; return true;
    case LISA_X86LOOPNE: *op = LOONGBT_LOOPNE; *width = LOONGBT_W; return true;
    case LISA_X86INC_B: *op = LOONGBT_INC; *width = LOONGBT_B; return true;
    case LISA_X86INC_H: *op = LOONGBT_INC; *width = LOONGBT_H; return true;
    case LISA_X86INC_W: *op = LOONGBT_INC; *width = LOONGBT_W; return true;
    case LISA_X86INC_D: *op = LOONGBT_INC; *width = LOONGBT_D; return true;
    case LISA_X86DEC_B: *op = LOONGBT_DEC; *width = LOONGBT_B; return true;
    case LISA_X86DEC_H: *op = LOONGBT_DEC; *width = LOONGBT_H; return true;
    case LISA_X86DEC_W: *op = LOONGBT_DEC; *width = LOONGBT_W; return true;
    case LISA_X86DEC_D: *op = LOONGBT_DEC; *width = LOONGBT_D; return true;
    case LISA_X86SETTM: *op = LOONGBT_SETTM; *width = LOONGBT_W; return true;
    case LISA_X86CLRTM: *op = LOONGBT_CLRTM; *width = LOONGBT_W; return true;
    case LISA_X86INCTOP: *op = LOONGBT_INCTOP; *width = LOONGBT_W; return true;
    case LISA_X86DECTOP: *op = LOONGBT_DECTOP; *width = LOONGBT_W; return true;
    case LISA_SETX86J: *op = LOONGBT_SETX86J; *width = LOONGBT_W; return true;
    case LISA_X86MUL_B: *op = LOONGBT_MUL; *width = LOONGBT_B; return true;
    case LISA_X86MUL_H: *op = LOONGBT_MUL; *width = LOONGBT_H; return true;
    case LISA_X86MUL_W: *op = LOONGBT_MUL; *width = LOONGBT_W; return true;
    case LISA_X86MUL_D: *op = LOONGBT_MUL; *width = LOONGBT_D; return true;
    case LISA_X86MUL_BU: *op = LOONGBT_MULU; *width = LOONGBT_B; return true;
    case LISA_X86MUL_HU: *op = LOONGBT_MULU; *width = LOONGBT_H; return true;
    case LISA_X86MUL_WU: *op = LOONGBT_MULU; *width = LOONGBT_W; return true;
    case LISA_X86MUL_DU: *op = LOONGBT_MULU; *width = LOONGBT_D; return true;
    case LISA_X86ADD_WU: *op = LOONGBT_ADDU; *width = LOONGBT_W; return true;
    case LISA_X86ADD_DU: *op = LOONGBT_ADDU; *width = LOONGBT_D; return true;
    case LISA_X86SUB_WU: *op = LOONGBT_SUBU; *width = LOONGBT_W; return true;
    case LISA_X86SUB_DU: *op = LOONGBT_SUBU; *width = LOONGBT_D; return true;
    case LISA_X86ADD_B: *op = LOONGBT_ADD; *width = LOONGBT_B; return true;
    case LISA_X86ADD_H: *op = LOONGBT_ADD; *width = LOONGBT_H; return true;
    case LISA_X86ADD_W: *op = LOONGBT_ADD; *width = LOONGBT_W; return true;
    case LISA_X86ADD_D: *op = LOONGBT_ADD; *width = LOONGBT_D; return true;
    case LISA_X86SUB_B: *op = LOONGBT_SUB; *width = LOONGBT_B; return true;
    case LISA_X86SUB_H: *op = LOONGBT_SUB; *width = LOONGBT_H; return true;
    case LISA_X86SUB_W: *op = LOONGBT_SUB; *width = LOONGBT_W; return true;
    case LISA_X86SUB_D: *op = LOONGBT_SUB; *width = LOONGBT_D; return true;
    case LISA_X86ADC_B: *op = LOONGBT_ADC; *width = LOONGBT_B; return true;
    case LISA_X86ADC_H: *op = LOONGBT_ADC; *width = LOONGBT_H; return true;
    case LISA_X86ADC_W: *op = LOONGBT_ADC; *width = LOONGBT_W; return true;
    case LISA_X86ADC_D: *op = LOONGBT_ADC; *width = LOONGBT_D; return true;
    case LISA_X86SBC_B: *op = LOONGBT_SBC; *width = LOONGBT_B; return true;
    case LISA_X86SBC_H: *op = LOONGBT_SBC; *width = LOONGBT_H; return true;
    case LISA_X86SBC_W: *op = LOONGBT_SBC; *width = LOONGBT_W; return true;
    case LISA_X86SBC_D: *op = LOONGBT_SBC; *width = LOONGBT_D; return true;
    case LISA_X86SLL_B: *op = LOONGBT_SLL; *width = LOONGBT_B; return true;
    case LISA_X86SLL_H: *op = LOONGBT_SLL; *width = LOONGBT_H; return true;
    case LISA_X86SLL_W: *op = LOONGBT_SLL; *width = LOONGBT_W; return true;
    case LISA_X86SLL_D: *op = LOONGBT_SLL; *width = LOONGBT_D; return true;
    case LISA_X86SRL_B: *op = LOONGBT_SRL; *width = LOONGBT_B; return true;
    case LISA_X86SRL_H: *op = LOONGBT_SRL; *width = LOONGBT_H; return true;
    case LISA_X86SRL_W: *op = LOONGBT_SRL; *width = LOONGBT_W; return true;
    case LISA_X86SRL_D: *op = LOONGBT_SRL; *width = LOONGBT_D; return true;
    case LISA_X86SRA_B: *op = LOONGBT_SRA; *width = LOONGBT_B; return true;
    case LISA_X86SRA_H: *op = LOONGBT_SRA; *width = LOONGBT_H; return true;
    case LISA_X86SRA_W: *op = LOONGBT_SRA; *width = LOONGBT_W; return true;
    case LISA_X86SRA_D: *op = LOONGBT_SRA; *width = LOONGBT_D; return true;
    case LISA_X86ROTR_B: *op = LOONGBT_ROTR; *width = LOONGBT_B; return true;
    case LISA_X86ROTR_H: *op = LOONGBT_ROTR; *width = LOONGBT_H; return true;
    case LISA_X86ROTR_D: *op = LOONGBT_ROTR; *width = LOONGBT_D; return true;
    case LISA_X86ROTR_W: *op = LOONGBT_ROTR; *width = LOONGBT_W; return true;
    case LISA_X86ROTL_B: *op = LOONGBT_ROTL; *width = LOONGBT_B; return true;
    case LISA_X86ROTL_H: *op = LOONGBT_ROTL; *width = LOONGBT_H; return true;
    case LISA_X86ROTL_W: *op = LOONGBT_ROTL; *width = LOONGBT_W; return true;
    case LISA_X86ROTL_D: *op = LOONGBT_ROTL; *width = LOONGBT_D; return true;
    case LISA_X86RCR_B: *op = LOONGBT_RCR; *width = LOONGBT_B; return true;
    case LISA_X86RCR_H: *op = LOONGBT_RCR; *width = LOONGBT_H; return true;
    case LISA_X86RCR_W: *op = LOONGBT_RCR; *width = LOONGBT_W; return true;
    case LISA_X86RCR_D: *op = LOONGBT_RCR; *width = LOONGBT_D; return true;
    case LISA_X86RCL_B: *op = LOONGBT_RCL; *width = LOONGBT_B; return true;
    case LISA_X86RCL_H: *op = LOONGBT_RCL; *width = LOONGBT_H; return true;
    case LISA_X86RCL_W: *op = LOONGBT_RCL; *width = LOONGBT_W; return true;
    case LISA_X86RCL_D: *op = LOONGBT_RCL; *width = LOONGBT_D; return true;
    case LISA_X86AND_B: *op = LOONGBT_AND; *width = LOONGBT_B; return true;
    case LISA_X86AND_H: *op = LOONGBT_AND; *width = LOONGBT_H; return true;
    case LISA_X86AND_W: *op = LOONGBT_AND; *width = LOONGBT_W; return true;
    case LISA_X86AND_D: *op = LOONGBT_AND; *width = LOONGBT_D; return true;
    case LISA_X86OR_B: *op = LOONGBT_OR; *width = LOONGBT_B; return true;
    case LISA_X86OR_H: *op = LOONGBT_OR; *width = LOONGBT_H; return true;
    case LISA_X86OR_W: *op = LOONGBT_OR; *width = LOONGBT_W; return true;
    case LISA_X86OR_D: *op = LOONGBT_OR; *width = LOONGBT_D; return true;
    case LISA_X86XOR_B: *op = LOONGBT_XOR; *width = LOONGBT_B; return true;
    case LISA_X86XOR_H: *op = LOONGBT_XOR; *width = LOONGBT_H; return true;
    case LISA_X86XOR_W: *op = LOONGBT_XOR; *width = LOONGBT_W; return true;
    case LISA_X86XOR_D: *op = LOONGBT_XOR; *width = LOONGBT_D; return true;
    case LISA_X86SLLI_B: *op = LOONGBT_SLLI; *width = LOONGBT_B; return true;
    case LISA_X86SLLI_H: *op = LOONGBT_SLLI; *width = LOONGBT_H; return true;
    case LISA_X86SLLI_W: *op = LOONGBT_SLLI; *width = LOONGBT_W; return true;
    case LISA_X86SLLI_D: *op = LOONGBT_SLLI; *width = LOONGBT_D; return true;
    case LISA_X86SRLI_B: *op = LOONGBT_SRLI; *width = LOONGBT_B; return true;
    case LISA_X86SRLI_H: *op = LOONGBT_SRLI; *width = LOONGBT_H; return true;
    case LISA_X86SRLI_W: *op = LOONGBT_SRLI; *width = LOONGBT_W; return true;
    case LISA_X86SRLI_D: *op = LOONGBT_SRLI; *width = LOONGBT_D; return true;
    case LISA_X86SRAI_B: *op = LOONGBT_SRAI; *width = LOONGBT_B; return true;
    case LISA_X86SRAI_H: *op = LOONGBT_SRAI; *width = LOONGBT_H; return true;
    case LISA_X86SRAI_W: *op = LOONGBT_SRAI; *width = LOONGBT_W; return true;
    case LISA_X86SRAI_D: *op = LOONGBT_SRAI; *width = LOONGBT_D; return true;
    case LISA_X86ROTRI_B: *op = LOONGBT_ROTRI; *width = LOONGBT_B; return true;
    case LISA_X86ROTRI_H: *op = LOONGBT_ROTRI; *width = LOONGBT_H; return true;
    case LISA_X86ROTRI_W: *op = LOONGBT_ROTRI; *width = LOONGBT_W; return true;
    case LISA_X86ROTRI_D: *op = LOONGBT_ROTRI; *width = LOONGBT_D; return true;
    case LISA_X86RCRI_B: *op = LOONGBT_RCRI; *width = LOONGBT_B; return true;
    case LISA_X86RCRI_H: *op = LOONGBT_RCRI; *width = LOONGBT_H; return true;
    case LISA_X86RCRI_W: *op = LOONGBT_RCRI; *width = LOONGBT_W; return true;
    case LISA_X86RCRI_D: *op = LOONGBT_RCRI; *width = LOONGBT_D; return true;
    case LISA_X86ROTLI_B: *op = LOONGBT_ROTLI; *width = LOONGBT_B; return true;
    case LISA_X86ROTLI_H: *op = LOONGBT_ROTLI; *width = LOONGBT_H; return true;
    case LISA_X86ROTLI_W: *op = LOONGBT_ROTLI; *width = LOONGBT_W; return true;
    case LISA_X86ROTLI_D: *op = LOONGBT_ROTLI; *width = LOONGBT_D; return true;
    case LISA_X86RCLI_B: *op = LOONGBT_RCLI; *width = LOONGBT_B; return true;
    case LISA_X86RCLI_H: *op = LOONGBT_RCLI; *width = LOONGBT_H; return true;
    case LISA_X86RCLI_W: *op = LOONGBT_RCLI; *width = LOONGBT_W; return true;
    case LISA_X86RCLI_D: *op = LOONGBT_RCLI; *width = LOONGBT_D; return true;
    case LISA_X86MFFLAG: *op = LOONGBT_MFFLAG; *width = LOONGBT_W; return true;
    case LISA_X86MTFLAG: *op = LOONGBT_MTFLAG; *width = LOONGBT_W; return true;
    default:
        return false;
    }
}

uint64_t loongbt_exec_ir2(LoongbtState *st, uint32_t lisa_opcode,
                          uint64_t a, uint64_t b, bool *supported)
{
    LoongbtOp op;
    LoongbtWidth width;

    if (!loongbt_op_from_ir2(lisa_opcode, &op, &width)) {
        if (supported != 0) {
            *supported = false;
        }
        return 0;
    }
    if (supported != 0) {
        *supported = true;
    }
    return loongbt_exec(st, op, width, a, b);
}
