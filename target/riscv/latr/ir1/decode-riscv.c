#include <string.h>
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "ir1.h"

typedef struct DisasContext {
    RiscvIR1 *ir1;
} DisasContext;

static int ex_plus_1(DisasContext *ctx, int value)
{
    (void)ctx;
    return value + 1;
}

#define LATR_EX_SHIFT(amount) \
    static int ex_shift_##amount(DisasContext *ctx, int value) \
    { \
        (void)ctx; \
        return (int)((uint32_t)value << amount); \
    }
LATR_EX_SHIFT(1)
LATR_EX_SHIFT(2)
LATR_EX_SHIFT(3)
LATR_EX_SHIFT(4)
LATR_EX_SHIFT(12)
#undef LATR_EX_SHIFT

static int ex_rvc_register(DisasContext *ctx, int reg)
{
    (void)ctx;
    return 8 + reg;
}

static int ex_sreg_register(DisasContext *ctx, int reg)
{
    (void)ctx;
    return reg < 2 ? reg + 8 : reg + 16;
}

static int ex_rvc_shiftli(DisasContext *ctx, int imm)
{
    (void)ctx;
    return imm;
}

static int ex_rvc_shiftri(DisasContext *ctx, int imm)
{
    (void)ctx;
    return imm;
}


#include "decode-insn32.c.inc"
#include "decode-insn16.c.inc"
#include "decode-trans-stubs.inc"

bool latr_decode(uint64_t pc, uint32_t raw, RiscvIR1 *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->pc = pc;
    out->insn = raw;
    out->raw_len = (raw & 3u) == 3u ? 4 : 2;

    DisasContext ctx = { .ir1 = out };
    bool decoded = out->raw_len == 2
        ? decode_insn16(&ctx, (uint16_t)raw)
        : decode_insn32(&ctx, raw);
    if (decoded) {
        switch (out->opcode) {
        case LATR_RV_SLLI:
        case LATR_RV_SRLI:
        case LATR_RV_SRAI:
        case LATR_RV_SLLI_UW:
        case LATR_RV_RORI:
        case LATR_RV_BSETI:
        case LATR_RV_BCLRI:
        case LATR_RV_BINVI:
        case LATR_RV_BEXTI:
            decoded = out->arg.shamt >= 0 && out->arg.shamt < 64;
            break;
        case LATR_RV_RORIW:
            decoded = out->arg.shamt >= 0 && out->arg.shamt < 32;
            break;
        case LATR_RV_SLLIW:
        case LATR_RV_SRLIW:
        case LATR_RV_SRAIW:
            decoded = out->arg.shamt >= 0 && out->arg.shamt < 32;
            break;
        default:
            break;
        }
    }
    if (!decoded || out->opcode == LATR_RV_ILLEGAL) {
        out->opcode = LATR_RV_ILLEGAL;
        return false;
    }
    return true;
}
