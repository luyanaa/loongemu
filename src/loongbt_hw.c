#include "loongemu/loongbt.h"

/* Hardware back end: the real LBT instructions.  Built for LoongArch with the
 * assembler extension enabled (-mattr=+lbt); on every other target the whole
 * body is compiled out and the dispatcher falls back to the integer model.
 *
 * Architectural split: LBT arithmetic instructions update EFLAGS only — the
 * result value comes from the base-ISA operation, which the translator emits
 * separately.  This function therefore derives the value with the portable
 * model and takes the flags from hardware, so both back ends expose the same
 * contract.  MTFLAG/MFFLAG and the FTOP moves are pure data movement and are
 * implemented with the documented bit layout on both back ends. */

#if defined(__loongarch__) && LOONGBT_ENABLE_HW

#define LBT_MTFLAG(v) __asm__ volatile("x86mtflag %0, 63" :: "r"((uint64_t)(v)))
#define LBT_MFFLAG(v) __asm__ volatile("x86mfflag %0, 63" : "=r"(v))

#define LBT_ALU2(mb, mh, mw, md)                                   \
    do {                                                           \
        switch (width) {                                           \
        case LOONGBT_B: __asm__ volatile(mb " %0, %1" :: "r"(a), "r"(b)); break; \
        case LOONGBT_H: __asm__ volatile(mh " %0, %1" :: "r"(a), "r"(b)); break; \
        case LOONGBT_W: __asm__ volatile(mw " %0, %1" :: "r"(a), "r"(b)); break; \
        default:        __asm__ volatile(md " %0, %1" :: "r"(a), "r"(b)); break; \
        }                                                          \
    } while (0)

#define LBT_ALU1(mb, mh, mw, md)                                   \
    do {                                                           \
        switch (width) {                                           \
        case LOONGBT_B: __asm__ volatile(mb " %0" :: "r"(a)); break; \
        case LOONGBT_H: __asm__ volatile(mh " %0" :: "r"(a)); break; \
        case LOONGBT_W: __asm__ volatile(mw " %0" :: "r"(a)); break; \
        default:        __asm__ volatile(md " %0" :: "r"(a)); break; \
        }                                                          \
    } while (0)

uint64_t loongbt_exec_soft(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                           uint64_t a, uint64_t b);

static uint64_t hw_setx86j(unsigned cond)
{
    uint64_t r = 0;

    switch (cond & 0xf) {
    case 0:  __asm__ volatile("setx86j %0, 0"  : "=r"(r) :: "memory"); break;
    case 1:  __asm__ volatile("setx86j %0, 1"  : "=r"(r) :: "memory"); break;
    case 2:  __asm__ volatile("setx86j %0, 2"  : "=r"(r) :: "memory"); break;
    case 3:  __asm__ volatile("setx86j %0, 3"  : "=r"(r) :: "memory"); break;
    case 4:  __asm__ volatile("setx86j %0, 4"  : "=r"(r) :: "memory"); break;
    case 5:  __asm__ volatile("setx86j %0, 5"  : "=r"(r) :: "memory"); break;
    case 6:  __asm__ volatile("setx86j %0, 6"  : "=r"(r) :: "memory"); break;
    case 7:  __asm__ volatile("setx86j %0, 7"  : "=r"(r) :: "memory"); break;
    case 8:  __asm__ volatile("setx86j %0, 8"  : "=r"(r) :: "memory"); break;
    case 9:  __asm__ volatile("setx86j %0, 9"  : "=r"(r) :: "memory"); break;
    case 10: __asm__ volatile("setx86j %0, 10" : "=r"(r) :: "memory"); break;
    case 11: __asm__ volatile("setx86j %0, 11" : "=r"(r) :: "memory"); break;
    case 12: __asm__ volatile("setx86j %0, 12" : "=r"(r) :: "memory"); break;
    case 13: __asm__ volatile("setx86j %0, 13" : "=r"(r) :: "memory"); break;
    case 14: __asm__ volatile("setx86j %0, 14" : "=r"(r) :: "memory"); break;
    default: __asm__ volatile("setx86j %0, 15" : "=r"(r) :: "memory"); break;
    }
    return r;
}

static void hw_mttop(unsigned top)
{
    switch (top & 7) {
    case 0: __asm__ volatile("x86mttop 0" ::: "memory"); break;
    case 1: __asm__ volatile("x86mttop 1" ::: "memory"); break;
    case 2: __asm__ volatile("x86mttop 2" ::: "memory"); break;
    case 3: __asm__ volatile("x86mttop 3" ::: "memory"); break;
    case 4: __asm__ volatile("x86mttop 4" ::: "memory"); break;
    case 5: __asm__ volatile("x86mttop 5" ::: "memory"); break;
    case 6: __asm__ volatile("x86mttop 6" ::: "memory"); break;
    default: __asm__ volatile("x86mttop 7" ::: "memory"); break;
    }
}

bool loongbt_hw_probe(void)
{
    uint64_t cfg = 0;
    uint64_t two = 2;

    __asm__ volatile("cpucfg %0, %1" : "=r"(cfg) : "r"(two));
    /* CPUCFG.2[18] = X86BT (LoongArch manual; same bit as Linux
     * LOONGARCH_CPUCFG2_X86BT). */
    return (cfg & (UINT64_C(1) << 18)) != 0;
}

uint64_t loongbt_hw_exec(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                         uint64_t a, uint64_t b)
{
    LoongbtState scratch;
    uint64_t value;
    uint64_t flags;

    switch (op) {
    case LOONGBT_MTFLAG:
        loongbt_unpack_flags(&st->eflags, a, (uint8_t)b);
        return 0;
    case LOONGBT_MFFLAG:
        return loongbt_pack_flags(st->eflags, (uint8_t)b);
    case LOONGBT_MTTOP:
        st->top = (uint32_t)(b & 7);
        hw_mttop(st->top);
        return 0;
    case LOONGBT_MFTOP:
        __asm__ volatile("x86mftop %0" : "=r"(value) :: "memory");
        st->top = (uint32_t)(value & 7);
        return st->top;
    case LOONGBT_INCTOP:
        __asm__ volatile("x86inctop" ::: "memory");
        st->top = (st->top + 1) & 7;
        return 0;
    case LOONGBT_DECTOP:
        __asm__ volatile("x86dectop" ::: "memory");
        st->top = (st->top - 1) & 7;
        return 0;
    case LOONGBT_SETTM:
        __asm__ volatile("x86settm" ::: "memory");
        st->tm = true;
        return 0;
    case LOONGBT_CLRTM:
        __asm__ volatile("x86clrtm" ::: "memory");
        st->tm = false;
        return 0;
    case LOONGBT_SETX86J:
        return hw_setx86j((unsigned)b);
    default:
        break;
    }

    scratch = *st;
    value = loongbt_exec_soft(&scratch, op, width, a, b);

    LBT_MTFLAG(st->eflags);
    switch (op) {
    case LOONGBT_ADD:   LBT_ALU2("x86add.b", "x86add.h", "x86add.w", "x86add.d"); break;
    case LOONGBT_ADC:   LBT_ALU2("x86adc.b", "x86adc.h", "x86adc.w", "x86adc.d"); break;
    case LOONGBT_SUB:   LBT_ALU2("x86sub.b", "x86sub.h", "x86sub.w", "x86sub.d"); break;
    case LOONGBT_ADDU:  LBT_ALU2("x86add.wu", "x86add.wu", "x86add.wu", "x86add.du"); break;
    case LOONGBT_SUBU:  LBT_ALU2("x86sub.wu", "x86sub.wu", "x86sub.wu", "x86sub.du"); break;
    case LOONGBT_SBC:   LBT_ALU2("x86sbc.b", "x86sbc.h", "x86sbc.w", "x86sbc.d"); break;
    case LOONGBT_AND:   LBT_ALU2("x86and.b", "x86and.h", "x86and.w", "x86and.d"); break;
    case LOONGBT_OR:    LBT_ALU2("x86or.b",  "x86or.h",  "x86or.w",  "x86or.d");  break;
    case LOONGBT_XOR:   LBT_ALU2("x86xor.b", "x86xor.h", "x86xor.w", "x86xor.d"); break;
    case LOONGBT_MUL:   LBT_ALU2("x86mul.b", "x86mul.h", "x86mul.w", "x86mul.d"); break;
    case LOONGBT_MULU:  LBT_ALU2("x86mul.bu", "x86mul.hu", "x86mul.wu", "x86mul.du"); break;
    case LOONGBT_ROTL:
    case LOONGBT_ROTLI: LBT_ALU2("x86rotl.b", "x86rotl.h", "x86rotl.w", "x86rotl.d"); break;
    case LOONGBT_ROTR:
    case LOONGBT_ROTRI: LBT_ALU2("x86rotr.b", "x86rotr.h", "x86rotr.w", "x86rotr.d"); break;
    case LOONGBT_RCL:
    case LOONGBT_RCLI:  LBT_ALU2("x86rcl.b", "x86rcl.h", "x86rcl.w", "x86rcl.d"); break;
    case LOONGBT_RCR:
    case LOONGBT_RCRI:  LBT_ALU2("x86rcr.b", "x86rcr.h", "x86rcr.w", "x86rcr.d"); break;
    case LOONGBT_SLL:
    case LOONGBT_SLLI:  LBT_ALU2("x86sll.b",  "x86sll.h",  "x86sll.w",  "x86sll.d");  break;
    case LOONGBT_SRL:
    case LOONGBT_SRLI:  LBT_ALU2("x86srl.b",  "x86srl.h",  "x86srl.w",  "x86srl.d");  break;
    case LOONGBT_SRA:
    case LOONGBT_SRAI:  LBT_ALU2("x86sra.b",  "x86sra.h",  "x86sra.w",  "x86sra.d");  break;
    case LOONGBT_INC:   LBT_ALU1("x86inc.b",  "x86inc.h",  "x86inc.w",  "x86inc.d");  break;
    case LOONGBT_DEC:   LBT_ALU1("x86dec.b",  "x86dec.h",  "x86dec.w",  "x86dec.d");  break;
    case LOONGBT_LOOPE:
        __asm__ volatile("setx86loope %0, %1" : "=r"(value) : "r"(a) : "memory");
        break;
    case LOONGBT_LOOPNE:
        __asm__ volatile("setx86loopne %0, %1" : "=r"(value) : "r"(a) : "memory");
        break;
    default:
        return value;
    }
    LBT_MFFLAG(flags);
    st->eflags = (uint32_t)flags & LOONGBT_EF_ALL;
    return value;
}

#else /* !(__loongarch__ && LOONGBT_ENABLE_HW) */

bool loongbt_hw_probe(void)
{
    return false;
}

uint64_t loongbt_exec_soft(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                           uint64_t a, uint64_t b);

uint64_t loongbt_hw_exec(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                         uint64_t a, uint64_t b)
{
    if (loongbt_hw_probe()) {
        return 0;
    }
    return loongbt_exec_soft(st, op, width, a, b);
}

#endif
