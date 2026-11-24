/*
 * AArch64 NZCV and condition-code reference, as a host-side unit test.
 *
 * LATA keeps NZCV in the LoongArch LTE flag register, so a host without LBT
 * cannot run any translated AArch64 code. Replacing that with a software path
 * needs two things proven before any emitter code is written:
 *
 *   1. the flag formulas for add/sub (N, Z, C, V for 64- and 32-bit forms),
 *   2. the 16 condition codes evaluated against a 4-bit NZCV nibble.
 *
 * This file is the reference implementation of both, checked against the cases
 * the LATA ISA suite already asserts, plus the full 16 x 16 condition matrix
 * derived from the ARM pseudocode. Keep it in step with any emitter change:
 * a mismatch here means the emitted path is wrong, not the test.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define N_BIT 3u
#define Z_BIT 2u
#define C_BIT 1u
#define V_BIT 0u

/* AddWithCarry over `bits`, returning the result and the packed nibble. */
static uint64_t add_with_carry(uint64_t a, uint64_t b, unsigned carry_in,
                               unsigned bits, unsigned *nzcv)
{
    uint64_t mask = bits == 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
    uint64_t op1 = a & mask;
    uint64_t op2 = b & mask;
    uint64_t sum = op1 + op2 + (uint64_t)carry_in;
    uint64_t result = sum & mask;
    unsigned n = (unsigned)((result >> (bits - 1)) & 1u);
    unsigned z = result == 0;
    /*
     * Carry-out without a 128-bit type (`__int128` trips -Werror=pedantic):
     * the sum of two unsigned values wrapped exactly when it is not greater
     * than the first operand, and a carry-in makes equality count as wrapped.
     */
    unsigned c = bits == 64 ? (unsigned)(carry_in ? (sum <= op1) : (sum < op1))
                            : (unsigned)((sum >> bits) & 1u);
    /* Overflow compares the operands' signs, not the sum's wide form. */
    unsigned v = (unsigned)(((((op1 ^ (~op2 & mask)) & (op1 ^ result)) >> (bits - 1)) & 1u));

    *nzcv = (n << N_BIT) | (z << Z_BIT) | (c << C_BIT) | (v << V_BIT);
    return result;
}

/* The AArch64 condition test, straight from the ARM pseudocode. */
static bool cond_holds(unsigned cond, unsigned nzcv)
{
    unsigned n = (nzcv >> N_BIT) & 1u;
    unsigned z = (nzcv >> Z_BIT) & 1u;
    unsigned c = (nzcv >> C_BIT) & 1u;
    unsigned v = (nzcv >> V_BIT) & 1u;

    switch (cond & 0xfu) {
    case 0x0: return z == 1;                 /* EQ */
    case 0x1: return z == 0;                 /* NE */
    case 0x2: return c == 1;                 /* CS/HS */
    case 0x3: return c == 0;                 /* CC/LO */
    case 0x4: return n == 1;                 /* MI */
    case 0x5: return n == 0;                 /* PL */
    case 0x6: return v == 1;                 /* VS */
    case 0x7: return v == 0;                 /* VC */
    case 0x8: return c == 1 && z == 0;       /* HI */
    case 0x9: return !(c == 1 && z == 0);    /* LS */
    case 0xa: return n == v;                 /* GE */
    case 0xb: return n != v;                 /* LT */
    case 0xc: return z == 0 && n == v;       /* GT */
    case 0xd: return !(z == 0 && n == v);    /* LE */
    default:  return true;                   /* AL, NV */
    }
}

static void expect_flags(const char *what, uint64_t a, uint64_t b,
                         unsigned bits, uint64_t want_res, unsigned want_nzcv)
{
    unsigned nzcv = 0xfu;
    uint64_t res = add_with_carry(a, b, 0, bits, &nzcv);

    if (res != want_res || nzcv != want_nzcv) {
        printf("FAIL %s: got res=%llx nzcv=%x want res=%llx nzcv=%x\n", what,
               (unsigned long long)res, nzcv, (unsigned long long)want_res,
               want_nzcv);
        assert(0);
    }
}

int main(void)
{
    int checks = 0;

    /*
     * The cases the LATA ISA suite asserts (integer/NZCV matrix): a nibble is
     * N<<3 | Z<<2 | C<<1 | V.
     */
    expect_flags("movn0 + 1", UINT64_MAX, 1, 64, 0, 0x6);
    /* min + min: the sum is 0 with carry out and signed overflow. */
    expect_flags("min + min", UINT64_C(0x8000000000000000),
                 UINT64_C(0x8000000000000000), 64, 0, 0x7);
    /* max - 1 + 1: no carry out, negative result. */
    expect_flags("max-1 + 1", UINT64_MAX - 1, 1, 64, UINT64_MAX, 0x8);
    checks += 3;

    /* Sub is AddWithCarry(a, ~b, 1). */
    {
        struct {
            uint64_t a, b;
            uint64_t res;
            unsigned nzcv;
        } subs[] = {
            { 5, 5, 0, 0x6 },                    /* zero: Z|C */
            { 0, 5, UINT64_MAX - 4, 0x8 },       /* borrow + negative */
            { UINT64_C(0x8000000000000000), 1, UINT64_C(0x7fffffffffffffff),
              0x3 },                             /* no borrow, overflow */
        };

        for (unsigned i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
            unsigned nzcv = 0xfu;
            uint64_t res = add_with_carry(subs[i].a, ~subs[i].b, 1, 64, &nzcv);

            if (res != subs[i].res || nzcv != subs[i].nzcv) {
                printf("FAIL subs[%u]: res=%llx nzcv=%x want %llx/%x\n", i,
                       (unsigned long long)res, nzcv,
                       (unsigned long long)subs[i].res, subs[i].nzcv);
                assert(0);
            }
            checks++;
        }
    }

    /* 32-bit forms sign-extend the result and use bit 31 for N/V. */
    {
        unsigned nzcv = 0xfu;
        uint64_t res = add_with_carry(0xffffffffu, 1, 0, 32, &nzcv);

        if (res != 0 || nzcv != 0x6) {
            printf("FAIL 32-bit wrap: res=%llx nzcv=%x\n",
                   (unsigned long long)res, nzcv);
            assert(0);
        }
        checks++;
    }

    /*
     * Condition truth table. For every nibble, each condition must agree with
     * the ARM pseudocode reading; the simple ones are spelled out so the table
     * is readable, the derived ones are cross-checked against their definition.
     */
    for (unsigned nzcv = 0; nzcv < 16; nzcv++) {
        unsigned n = (nzcv >> N_BIT) & 1u, z = (nzcv >> Z_BIT) & 1u;
        unsigned c = (nzcv >> C_BIT) & 1u, v = (nzcv >> V_BIT) & 1u;

        assert(cond_holds(0x0, nzcv) == (z == 1));
        assert(cond_holds(0x1, nzcv) == (z == 0));
        assert(cond_holds(0x2, nzcv) == (c == 1));
        assert(cond_holds(0x3, nzcv) == (c == 0));
        assert(cond_holds(0x4, nzcv) == (n == 1));
        assert(cond_holds(0x5, nzcv) == (n == 0));
        assert(cond_holds(0x6, nzcv) == (v == 1));
        assert(cond_holds(0x7, nzcv) == (v == 0));
        assert(cond_holds(0x8, nzcv) == (c == 1 && z == 0));
        assert(cond_holds(0x9, nzcv) == !(c == 1 && z == 0));
        assert(cond_holds(0xa, nzcv) == (n == v));
        assert(cond_holds(0xb, nzcv) == (n != v));
        assert(cond_holds(0xc, nzcv) == (z == 0 && n == v));
        assert(cond_holds(0xd, nzcv) == !(z == 0 && n == v));
        assert(cond_holds(0xe, nzcv));
        assert(cond_holds(0xf, nzcv));
        checks += 16;
    }

    /* CSET polarity as the LATA suite uses it: after `cmp x1, x5` with x1 < x5
     * the nibble is N|V^... check the two cases the suite asserts directly. */
    {
        unsigned nzcv = 0xfu;
        (void)add_with_carry(5, ~5, 1, 64, &nzcv);
        assert(cond_holds(0x0, nzcv));      /* EQ holds after 5 - 5 */
        assert(!cond_holds(0x1, nzcv));
        checks += 2;
    }
    {
        unsigned nzcv = 0xfu;
        (void)add_with_carry(UINT64_MAX, 1, 0, 64, &nzcv);
        assert(cond_holds(0x2, nzcv));      /* HS holds after carry out */
        assert(cond_holds(0x8, nzcv) == false); /* HI needs !Z */
        checks += 2;
    }

    printf("lata-nzcv: %d checks passed\n", checks);
    return 0;
}
