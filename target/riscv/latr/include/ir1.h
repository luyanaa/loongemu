#ifndef LATR_IR1_H
#define LATR_IR1_H

#include <stdbool.h>
#include <stdint.h>

typedef enum LatrRiscvOpcode {
    LATR_RV_ILLEGAL = 0,
#define LATR_RV_INSN(tag, name) LATR_RV_##tag,
#define LATR_RV_ALIAS(tag, name)
#include "../ir1/rv64gc-opcodes.def"
#undef LATR_RV_ALIAS
#undef LATR_RV_INSN
    LATR_RV_OPCODE_COUNT
} LatrRiscvOpcode;

typedef struct RiscvIR1 {
    uint64_t pc;
    uint32_t insn;
    uint8_t raw_len;
    LatrRiscvOpcode opcode;
    struct {
        int rd;
        int rs1;
        int rs2;
        int rs3;
        int32_t imm;
        int shamt;
        int csr;
        int zimm;
        int rm;
        int aq;
        int rl;
        int pred;
        int succ;
    } arg;
} RiscvIR1;

/* `raw` is the little-endian instruction bits, with the first 16 bits low. */
bool latr_decode(uint64_t pc, uint32_t raw, RiscvIR1 *out);

#endif
