#include <stdio.h>
#include "ir1.h"

static int checks;
static int failures;

#define CHECK(condition, description) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "LATR decode: FAIL %s\n", description); \
        failures++; \
    } \
} while (0)

int main(void)
{
    RiscvIR1 ir1;

    CHECK(latr_decode(0x1000, 0x00500513, &ir1), "decode ADDI");
    CHECK(ir1.opcode == LATR_RV_ADDI && ir1.raw_len == 4 &&
          ir1.arg.rd == 10 && ir1.arg.rs1 == 0 && ir1.arg.imm == 5,
          "ADDI normalized operands");

    CHECK(latr_decode(0x1004, 0x00c5853b, &ir1), "decode ADDW");
    CHECK(ir1.opcode == LATR_RV_ADDW && ir1.raw_len == 4 &&
          ir1.arg.rd == 10 && ir1.arg.rs1 == 11 && ir1.arg.rs2 == 12,
          "ADDW normalized operands");

    CHECK(latr_decode(0x2000, 0x03f11093, &ir1) &&
          ir1.opcode == LATR_RV_SLLI && ir1.arg.shamt == 63,
          "accept maximum RV64 immediate shift");
    CHECK(!latr_decode(0x2004, 0x06411093, &ir1) &&
          ir1.opcode == LATR_RV_ILLEGAL,
          "reject reserved RV64 immediate shift bit");

    CHECK(latr_decode(0x1008, 0x00000505, &ir1), "decode compressed ADDI");
    CHECK(ir1.opcode == LATR_RV_ADDI && ir1.raw_len == 2 &&
          ir1.arg.rd == 10 && ir1.arg.rs1 == 10 && ir1.arg.imm == 1,
          "compressed ADDI preserves raw length and operands");

    CHECK(latr_decode(0x100a, 0x00000073, &ir1) &&
          ir1.opcode == LATR_RV_ECALL,
          "decode ECALL");
    CHECK(latr_decode(0x100c, 0x0330000f, &ir1) &&
          ir1.opcode == LATR_RV_FENCE,
          "decode standard FENCE");
    CHECK(!latr_decode(0x1010, 0x00200073, &ir1) &&
          ir1.opcode == LATR_RV_ILLEGAL,
          "reject URET outside RV64GC");
    CHECK(!latr_decode(0x1018, 0x10400073, &ir1) &&
          ir1.opcode == LATR_RV_ILLEGAL,
          "reject SFENCE.VM outside RV64GC");

    CHECK(latr_decode(0x101c, 0x10200073, &ir1) &&
          ir1.opcode == LATR_RV_SRET,
          "decode SRET for runtime privilege checking");
    CHECK(latr_decode(0x1020, 0x30200073, &ir1) &&
          ir1.opcode == LATR_RV_MRET,
          "decode MRET for runtime privilege checking");
    CHECK(latr_decode(0x1024, 0x10500073, &ir1) &&
          ir1.opcode == LATR_RV_WFI,
          "decode WFI");
    CHECK(latr_decode(0x1028, 0x12000073, &ir1) &&
          ir1.opcode == LATR_RV_SFENCE_VMA,
          "decode SFENCE.VMA for runtime privilege checking");
    CHECK(latr_decode(0x102c, 0x0100000f, &ir1) &&
          ir1.opcode == LATR_RV_PAUSE,
          "decode Zihintpause");
    CHECK(latr_decode(0x1030, 0x0040200f, &ir1) &&
          ir1.opcode == LATR_RV_CBO_ZERO,
          "decode CBO.ZERO");


    CHECK(!latr_decode(0x100e, 0x00000000, &ir1) &&
          ir1.opcode == LATR_RV_ILLEGAL && ir1.raw_len == 2,
          "reject reserved compressed zero");
    CHECK(latr_decode(0x1010, 0x021101d7, &ir1) &&
          ir1.opcode == LATR_RV_VADD_VV && ir1.arg.rd == 3 &&
          ir1.arg.rs1 == 2 && ir1.arg.rs2 == 1 && ir1.arg.vm == 1,
          "decode vadd.vv with the unmasked form bit");
    CHECK(latr_decode(0x1014, 0x001101d7, &ir1) &&
          ir1.opcode == LATR_RV_VADD_VV && ir1.arg.vm == 0,
          "decode masked vadd.vv");
    CHECK(latr_decode(0x1018, 0x0203e607, &ir1) &&
          ir1.opcode == LATR_RV_VLE32_V && ir1.arg.rd == 12 &&
          ir1.arg.rs1 == 7 && ir1.arg.vm == 1 && ir1.arg.nf == 1,
          "decode vle32.v base register and nf");
    CHECK(latr_decode(0x101c, 0x5e0fb5d7, &ir1) &&
          ir1.opcode == LATR_RV_VMV_V_I && ir1.arg.rd == 11 &&
          ir1.arg.rs1 == 31,
          "decode vmv.v.i immediate field");
    CHECK(latr_decode(0x1020, 0x5c110557, &ir1) &&
          ir1.opcode == LATR_RV_VMERGE_VVM && ir1.arg.rd == 10 &&
          ir1.arg.rs1 == 2 && ir1.arg.rs2 == 1,
          "decode vmerge.vvm operand order");
    CHECK(!latr_decode(0x1014, 0xffffffff, &ir1) &&
          ir1.opcode == LATR_RV_ILLEGAL,
          "reject unknown 32-bit encoding");
    CHECK(!latr_decode(0, 0, NULL), "reject null output");

    if (failures != 0) {
        fprintf(stderr, "LATR decode: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("LATR decode: %d checks passed\n", checks);
    return 0;
}
