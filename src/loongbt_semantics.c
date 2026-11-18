#include "loongemu/loongbt.h"

/* Portable (no-LBT) back end: the architectural semantics of the LBT x86
 * subset.  Every operation is width-parametric so one implementation serves
 * .b/.h/.w/.d. */
static uint64_t zmask(unsigned w)
{
    return w == 64 ? UINT64_MAX : ((UINT64_C(1) << w) - 1);
}

static int64_t sext(uint64_t v, unsigned w)
{
    uint64_t sign;

    if (w == 64) {
        return (int64_t)v;
    }
    sign = UINT64_C(1) << (w - 1);
    v &= zmask(w);
    return (int64_t)((v ^ sign) - sign);
}

static uint32_t parity_flag(uint64_t r)
{
    /* PF is set when the low byte has an even number of one bits. */
    return __builtin_parity((unsigned)(r & 0xff)) ? 0 : LOONGBT_EF_PF;
}

static void flags_result(LoongbtState *st, unsigned w, uint64_t r, uint32_t extra)
{
    uint32_t f = extra & (LOONGBT_EF_CF | LOONGBT_EF_AF | LOONGBT_EF_OF);

    if (r == 0) {
        f |= LOONGBT_EF_ZF;
    }
    if (r & (UINT64_C(1) << (w - 1))) {
        f |= LOONGBT_EF_SF;
    }
    st->eflags = f | parity_flag(r);
}

static uint32_t carry_in(const LoongbtState *st)
{
    return (st->eflags & LOONGBT_EF_CF) ? 1u : 0u;
}

static uint64_t op_add_adc(LoongbtState *st, unsigned w, uint64_t a, uint64_t b,
                           int with_carry)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    uint64_t c = with_carry ? carry_in(st) : 0;
    unsigned __int128 wide = (unsigned __int128)x + y + c;
    uint64_t r = (uint64_t)wide & m;
    uint32_t f = 0;

    if (wide > m) {
        f |= LOONGBT_EF_CF;
    }
    if ((x ^ y ^ r) & 0x10) {
        f |= LOONGBT_EF_AF;
    }
    if (~(x ^ y) & (x ^ r) & (UINT64_C(1) << (w - 1))) {
        f |= LOONGBT_EF_OF;
    }
    flags_result(st, w, r, f);
    return r;
}

static uint64_t op_add_u(LoongbtState *st, unsigned w, uint64_t a, uint64_t b)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    unsigned __int128 wide = (unsigned __int128)x + y;
    uint64_t r = (uint64_t)wide & m;
    uint32_t f = 0;

    if (wide > m) {
        f |= LOONGBT_EF_CF;
    }
    if ((x ^ y ^ r) & 0x10) {
        f |= LOONGBT_EF_AF;
    }
    flags_result(st, w, r, f); /* OF stays 0 for the unsigned form */
    return r;
}

static uint64_t op_sub_u(LoongbtState *st, unsigned w, uint64_t a, uint64_t b)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    uint64_t r = (x - y) & m;
    uint32_t f = 0;

    if (x < y) {
        f |= LOONGBT_EF_CF;
    }
    if ((x ^ y ^ r) & 0x10) {
        f |= LOONGBT_EF_AF;
    }
    flags_result(st, w, r, f); /* OF stays 0 */
    return r;
}

static uint64_t op_sub_sbc(LoongbtState *st, unsigned w, uint64_t a, uint64_t b,
                           int with_borrow)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    uint64_t c = with_borrow ? carry_in(st) : 0;
    uint64_t wide_rhs = y + c;
    uint64_t r = (x - wide_rhs) & m;
    uint32_t f = 0;

    if ((unsigned __int128)x < (unsigned __int128)y + c) {
        f |= LOONGBT_EF_CF;
    }
    if ((x ^ y ^ r) & 0x10) {
        f |= LOONGBT_EF_AF;
    }
    if ((x ^ y) & (x ^ r) & (UINT64_C(1) << (w - 1))) {
        f |= LOONGBT_EF_OF;
    }
    flags_result(st, w, r, f);
    return r;
}

static uint64_t op_logic(LoongbtState *st, unsigned w, uint64_t a, uint64_t b, int which)
{
    uint64_t m = zmask(w), x = a & m, y = b & m, r;

    r = which == 0 ? (x & y) : which == 1 ? (x | y) : (x ^ y);
    flags_result(st, w, r, 0); /* CF, OF, AF cleared */
    return r;
}

static uint64_t op_incdec(LoongbtState *st, unsigned w, uint64_t a, int dec)
{
    uint64_t m = zmask(w), x = a & m;
    uint64_t one = 1;
    uint64_t r = (dec ? x - 1 : x + 1) & m;
    uint32_t saved_cf = st->eflags & LOONGBT_EF_CF;
    uint32_t f = 0;

    if ((x ^ one ^ r) & 0x10) {
        f |= LOONGBT_EF_AF;
    }
    if (dec) {
        if (x == (UINT64_C(1) << (w - 1))) {
            f |= LOONGBT_EF_OF;
        }
    } else if (x == (m >> 1)) {
        f |= LOONGBT_EF_OF;
    }
    flags_result(st, w, r, f);
    st->eflags |= saved_cf; /* INC/DEC preserve CF */
    return r;
}

static uint64_t op_mul(LoongbtState *st, unsigned w, uint64_t a, uint64_t b, int is_signed)
{
    uint64_t m = zmask(w), x = a & m, y = b & m, r;
    uint32_t overflow;
    unsigned __int128 up;
    __int128 sp;

    if (is_signed) {
        int64_t xs = sext(x, w), ys = sext(y, w);
        sp = (__int128)xs * ys;
        r = (uint64_t)sp & m;
        overflow = ((__int128)sext(r, w) != sp);
    } else {
        up = (unsigned __int128)x * y;
        r = (uint64_t)up & m;
        overflow = ((up >> w) != 0);
    }
    st->eflags = overflow ? (LOONGBT_EF_CF | LOONGBT_EF_OF) : 0;
    return r;
}

static uint64_t op_rot(LoongbtState *st, unsigned w, uint64_t a, uint64_t b,
                       int left, int through_carry)
{
    uint64_t m = zmask(w), x = a & m;
    unsigned ring = through_carry ? w + 1 : w;
    unsigned c = (unsigned)(b & (w == 64 ? 0x3f : 0x1f));
    unsigned n, bit;
    uint64_t r;

    if (through_carry) {
        unsigned __int128 wide;
        unsigned cf = carry_in(st);

        if (c == 0) {
            return x;
        }
        n = c % ring;
        if (n == 0) {
            return x;
        }
        wide = ((unsigned __int128)cf << w) | x;
        if (left) {
            wide = ((wide << n) | (wide >> (ring - n))) & ((((unsigned __int128)1) << ring) - 1);
            bit = (unsigned)((wide >> w) & 1);
        } else {
            wide = ((wide >> n) | (wide << (ring - n))) & ((((unsigned __int128)1) << ring) - 1);
            bit = (unsigned)((wide >> w) & 1);
        }
        r = (uint64_t)wide & m;
        st->eflags &= ~(LOONGBT_EF_CF | LOONGBT_EF_OF);
        if (bit) {
            st->eflags |= LOONGBT_EF_CF;
        }
        if (c == 1) {
            if ((x ^ r) & (UINT64_C(1) << (w - 1))) {
                st->eflags |= LOONGBT_EF_OF;
            }
        }
        return r;
    }

    if (c == 0) {
        return x;
    }
    n = c % w;
    if (n == 0) {
        return x;
    }
    if (left) {
        r = ((x << n) | (x >> (w - n))) & m;
        bit = (unsigned)((x >> (w - n)) & 1);
    } else {
        r = ((x >> n) | (x << (w - n))) & m;
        bit = (unsigned)((x >> (n - 1)) & 1);
    }
    st->eflags &= ~(LOONGBT_EF_CF | LOONGBT_EF_OF);
    if (bit) {
        st->eflags |= LOONGBT_EF_CF;
    }
    if (c == 1 && ((x ^ r) & (UINT64_C(1) << (w - 1)))) {
        st->eflags |= LOONGBT_EF_OF;
    }
    return r;
}

static uint64_t op_shift(LoongbtState *st, unsigned w, uint64_t a, uint64_t b, int kind)
{
    /* kind: 0 = sll, 1 = srl, 2 = sra */
    uint64_t m = zmask(w), x = a & m;
    unsigned c = (unsigned)(b & (w == 64 ? 0x3f : 0x1f));
    uint64_t r;
    uint32_t cf;

    if (c == 0) {
        return x; /* flags unchanged */
    }
    if (kind == 0) {
        cf = (c <= w) ? (uint32_t)((x >> (w - c)) & 1) : 0;
        r = (c >= w) ? 0 : ((x << c) & m);
    } else if (kind == 1) {
        cf = (c <= w) ? (uint32_t)((x >> (c - 1)) & 1) : 0;
        r = (c >= w) ? 0 : (x >> c);
    } else {
        int64_t s = sext(x, w);
        unsigned sh = c >= w ? w - 1 : c;
        cf = (c > w) ? (uint32_t)((s < 0) ? 1 : 0)
                     : (uint32_t)((x >> (c - 1)) & 1);
        r = (uint64_t)(s >> sh) & m;
    }
    flags_result(st, w, r, cf ? LOONGBT_EF_CF : 0);
    if (c == 1 && kind != 2 && ((x ^ r) & (UINT64_C(1) << (w - 1)))) {
        st->eflags |= LOONGBT_EF_OF;
    }
    return r;
}

uint64_t loongbt_exec_soft(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                           uint64_t a, uint64_t b)
{
    unsigned w = (unsigned)width;

    switch (op) {
    case LOONGBT_ADD:  return op_add_adc(st, w, a, b, 0);
    case LOONGBT_ADDU: return op_add_u(st, w, a, b);
    case LOONGBT_ADC:  return op_add_adc(st, w, a, b, 1);
    case LOONGBT_SUB:  return op_sub_sbc(st, w, a, b, 0);
    case LOONGBT_SUBU: return op_sub_u(st, w, a, b);
    case LOONGBT_SBC:  return op_sub_sbc(st, w, a, b, 1);
    case LOONGBT_AND:  return op_logic(st, w, a, b, 0);
    case LOONGBT_OR:   return op_logic(st, w, a, b, 1);
    case LOONGBT_XOR:  return op_logic(st, w, a, b, 2);
    case LOONGBT_INC:  return op_incdec(st, w, a, 0);
    case LOONGBT_DEC:  return op_incdec(st, w, a, 1);
    case LOONGBT_MUL:  return op_mul(st, w, a, b, 1);
    case LOONGBT_MULU: return op_mul(st, w, a, b, 0);
    case LOONGBT_ROTL:
    case LOONGBT_ROTLI: return op_rot(st, w, a, b, 1, 0);
    case LOONGBT_ROTR:
    case LOONGBT_ROTRI: return op_rot(st, w, a, b, 0, 0);
    case LOONGBT_RCL:
    case LOONGBT_RCLI: return op_rot(st, w, a, b, 1, 1);
    case LOONGBT_RCR:
    case LOONGBT_RCRI: return op_rot(st, w, a, b, 0, 1);
    case LOONGBT_SLL:
    case LOONGBT_SLLI: return op_shift(st, w, a, b, 0);
    case LOONGBT_SRL:
    case LOONGBT_SRLI: return op_shift(st, w, a, b, 1);
    case LOONGBT_SRA:
    case LOONGBT_SRAI: return op_shift(st, w, a, b, 2);
    case LOONGBT_MTFLAG:
        loongbt_unpack_flags(&st->eflags, a, (uint8_t)b);
        return 0;
    case LOONGBT_MFFLAG:
        return loongbt_pack_flags(st->eflags, (uint8_t)b);
    case LOONGBT_MTTOP:
        st->top = (uint32_t)(b & 7);
        return 0;
    case LOONGBT_MFTOP:
        return st->top;
    case LOONGBT_INCTOP:
        st->top = (st->top + 1) & 7;
        return 0;
    case LOONGBT_DECTOP:
        st->top = (st->top - 1) & 7;
        return 0;
    case LOONGBT_SETTM:
        st->tm = true;
        return 0;
    case LOONGBT_CLRTM:
        st->tm = false;
        return 0;
    case LOONGBT_SETX86J: {
        uint32_t f = st->eflags;
        unsigned cond = (unsigned)(b & 0xf);
        int cf = !!(f & LOONGBT_EF_CF), pf = !!(f & LOONGBT_EF_PF);
        int zf = !!(f & LOONGBT_EF_ZF), sf = !!(f & LOONGBT_EF_SF);
        int of = !!(f & LOONGBT_EF_OF);
        int r;

        switch (cond) {
        case 0:  r = !cf && !zf; break;              /* A  */
        case 1:  r = !cf; break;                     /* AE */
        case 2:  r = cf; break;                      /* B  */
        case 3:  r = cf || zf; break;                /* BE */
        case 4:  r = zf; break;                      /* E  */
        case 5:  r = !zf; break;                     /* NE */
        case 6:  r = !zf && (sf == of); break;       /* G  */
        case 7:  r = (sf == of); break;              /* GE */
        case 8:  r = (sf != of); break;              /* L  */
        case 9:  r = zf || (sf != of); break;        /* LE */
        case 10: r = sf; break;                      /* S  */
        case 11: r = !sf; break;                     /* NS */
        case 12: r = of; break;                      /* O  */
        case 13: r = !of; break;                     /* NO */
        case 14: r = pf; break;                      /* P  */
        default: r = !pf; break;                     /* NP */
        }
        return (uint64_t)r;
    }
    case LOONGBT_LOOPE:
        return (a != 0 && (st->eflags & LOONGBT_EF_ZF)) ? 1 : 0;
    case LOONGBT_LOOPNE:
        return (a != 0 && !(st->eflags & LOONGBT_EF_ZF)) ? 1 : 0;
    default:
        return 0;
    }
}
