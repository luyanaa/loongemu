/* Host-side MIPS64r2 IR1 decode checks.  No IR2 or LoongArch code involved. */
#include <stdio.h>
#include <string.h>
#include "ir1.h"

static int checks, fails;

static void expect(uint32_t insn, MIPS_OP op, int subop, int rs, int rt,
                   int rd, int16_t simm, const char *what)
{
    MipsIR1 ir1;

    checks++;
    mips_decode_one(&ir1, insn, 0x1000);
    if (ir1.op != op || (subop >= 0 && (int)ir1.subop != subop) ||
        ir1.rs != rs || ir1.rt != rt || ir1.rd != rd ||
        (simm != -32768 && ir1.simm != simm)) {
        fails++;
        printf("FAIL %s insn=%08x op=%d sub=%d rs=%u rt=%u rd=%u simm=%d\n",
               what, insn, ir1.op, ir1.subop, ir1.rs, ir1.rt, ir1.rd,
               ir1.simm);
    }
}

int main(void)
{
    expect(0x00000000, MIPS_OP_NOP, -1, 0, 0, 0, 0, "nop");
    expect(0x64020005, MIPS_OP_ALU_IMM, MIPS_IMM_DADDIU, 0, 2, 0, 5,
           "daddiu");
    expect(0x0043202d, MIPS_OP_ALU, MIPS_ALU_DADDU, 2, 3, 4, -32768,
           "daddu");
    expect(0x0043001d, MIPS_OP_MULDIV, MIPS_MD_DMULTU, 2, 3, 0, -32768,
           "dmultu");
    expect(0x00002012, MIPS_OP_HILO, MIPS_HILO_MFLO, 0, 0, 4, -32768,
           "mflo");
    expect(0x00263a02, MIPS_OP_SHIFT_V, MIPS_SHV_ROTR, 1, 6, 7, -32768,
           "rotr");
    expect(0x0c00000b, MIPS_OP_JUMP, MIPS_JP_JAL, 0, 0, 0, -32768,
           "jal");
    expect(0x00400008, MIPS_OP_JUMP, MIPS_JP_JR, 2, 0, 0, -32768, "jr");
    expect(0x10430002, MIPS_OP_BRANCH, MIPS_BR_BEQ, 2, 3, 0, 2, "beq");
    expect(0x50420002, MIPS_OP_BRANCH, MIPS_BR_BEQL, 2, 2, 0, 2, "beql");
    expect(0x88440000, MIPS_OP_LOAD, MIPS_MEM_LWL, 2, 4, 0, 0, "lwl");
    expect(0xb4450010, MIPS_OP_STORE, MIPS_MEM_SDR, 2, 5, 0, 16, "sdr");
    expect(0x3c031122, MIPS_OP_ALU_IMM, MIPS_IMM_LUI, 0, 3, 2, 0x1122,
           "lui");
    expect(0x34633344, MIPS_OP_ALU_IMM, MIPS_IMM_ORI, 3, 3, 6, 0x3344,
           "ori");
    expect(0xac430000, MIPS_OP_STORE, MIPS_MEM_SW, 2, 3, 0, 0, "sw");
    expect(0xfc450008, MIPS_OP_STORE, MIPS_MEM_SD, 2, 5, 0, 8, "sd");
    expect(0xdc470010, MIPS_OP_LOAD, MIPS_MEM_LD, 2, 7, 0, 16, "ld");
    expect(0x0000000c, MIPS_OP_SYSCALL, -1, 0, 0, 0, -32768, "syscall");
    expect(0x0000000d, MIPS_OP_BREAK, -1, 0, 0, 0, -32768, "break");
    expect(0x42000018, MIPS_OP_CP0_OP, MIPS_CP0OP_ERET, 16, 0, 0, -32768,
           "eret");
    expect(0x40026000, MIPS_OP_CP0_MOVE, MIPS_CP0_MFC0, 0, 2, 12, -32768,
           "mfc0");

    printf("latm-decode: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
