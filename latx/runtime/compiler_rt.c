/*
 * Compiler-runtime pieces clang emits for the LoongArch freestanding build:
 * 128-bit unsigned division and the binary128 (TFmode) conversions that the
 * x87 helpers trigger through long-double arithmetic.
 *
 * Implemented here (rather than linking compiler-rt) so the archive stays
 * self-contained and the semantics are visible in-tree.
 */
#include <stdint.h>

typedef union F128Bits {
    long double f;
    unsigned __int128 u;
} F128Bits;

/* --- __udivti3: unsigned 128/128 division by shift-subtract --- */
unsigned __int128 __udivti3(unsigned __int128 n, unsigned __int128 d)
{
    unsigned __int128 q = 0;
    unsigned __int128 r = 0;
    int i;

    if (d == 0) {
        return 0; /* the architectural path traps elsewhere */
    }
    for (i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (unsigned __int128)1u << i;
        }
    }
    return q;
}

/* --- double -> binary128 (exact) --- */
long double __extenddftf2(double x)
{
    F128Bits in, out;
    uint64_t bits;
    uint64_t sign, exp, frac;

    in.f = 0;
    /* Bit-copy without relying on a double -> int128 conversion builtin. */
    {
        union { double d; uint64_t u; } tmp;

        tmp.d = x;
        bits = tmp.u;
    }
    sign = (bits >> 63) & 1u;
    exp = (bits >> 52) & 0x7ffu;
    frac = bits & 0x000fffffffffffffull;

    out.u = 0;
    if (exp == 0x7ffu) {
        /* inf / nan */
        out.u = ((unsigned __int128)sign << 127)
              | ((unsigned __int128)0x7fffu << 112)
              | ((unsigned __int128)frac << 60);
        return out.f;
    }
    if (exp == 0) {
        /* zero or subnormal double -> normal/zero binary128 */
        if (frac == 0) {
            out.u = (unsigned __int128)sign << 127;
            return out.f;
        }
        {
            int shift = 0;
            unsigned __int128 m = frac;

            while ((m & (UINT64_C(1) << 52)) == 0) {
                m <<= 1;
                shift++;
            }
            m &= ~(UINT64_C(1) << 52);
            out.u = ((unsigned __int128)sign << 127)
                  | ((unsigned __int128)(16383 - 1022 - shift) << 112)
                  | (m << 60);
        }
        return out.f;
    }
    out.u = ((unsigned __int128)sign << 127)
          | ((unsigned __int128)((int)exp - 1023 + 16383) << 112)
          | ((unsigned __int128)frac << 60);
    return out.f;
}

/* --- binary128 -> double (round to nearest even) --- */
double __trunctfdf2(long double x)
{
    F128Bits in;
    uint64_t sign, exp, frac64;
    unsigned __int128 frac;

    in.f = x;
    sign = (uint64_t)(in.u >> 127) & 1u;
    exp = (uint64_t)(in.u >> 112) & 0x7fffu;
    frac = in.u & (((unsigned __int128)1u << 112) - 1u);

    if (exp == 0x7fffu) {
        uint64_t out = (sign << 63) | (UINT64_C(0x7ff) << 52);

        out |= (uint64_t)(frac >> 60);
        {
            union { double d; uint64_t u; } tmp;

            tmp.u = out;
            return tmp.d;
        }
    }

    if (exp == 0 && frac == 0) {
        union { double d; uint64_t u; } tmp;

        tmp.u = sign << 63;
        return tmp.d;
    }

    {
        int e = (int)exp - 16383 + 1023;
        unsigned __int128 m;

        if (exp == 0) {
            /* binary128 subnormal: normalize first */
            int shift = 0;

            m = frac;
            while ((m & ((unsigned __int128)1u << 111)) == 0) {
                m <<= 1;
                shift++;
            }
            frac = (m << 1) & (((unsigned __int128)1u << 112) - 1u);
            e = 1 - 16383 + 1023 - shift;
        }

        if (e >= 0x7ff) {
            union { double d; uint64_t u; } tmp;

            tmp.u = (sign << 63) | (UINT64_C(0x7ff) << 52);
            return tmp.d;
        }
        if (e <= 0) {
            /* subnormal double: shift with rounding */
            int drop = 1 - e;
            unsigned __int128 full = (((unsigned __int128)1u << 112) | frac);
            unsigned __int128 half = (unsigned __int128)1u << (drop - 1);
            unsigned __int128 low = full & ((((unsigned __int128)1u) << drop) - 1u);
            unsigned __int128 q = full >> drop;
            uint64_t out;

            if (low > half || (low == half && (q & 1u))) {
                q++;
            }
            out = (sign << 63) | (uint64_t)(q & ((UINT64_C(1) << 52) - 1u));
            {
                union { double d; uint64_t u; } tmp;

                tmp.u = out;
                return tmp.d;
            }
        }

        frac64 = (uint64_t)(frac >> 60);
        {
            unsigned __int128 dropped = frac & (((unsigned __int128)1u << 60) - 1u);
            unsigned __int128 half = (unsigned __int128)1u << 59;
            uint64_t out;

            if (dropped > half || (dropped == half && (frac64 & 1u))) {
                frac64++;
                if (frac64 == (UINT64_C(1) << 52)) {
                    frac64 = 0;
                    e++;
                    if (e >= 0x7ff) {
                        union { double d; uint64_t u; } tmp;

                        tmp.u = (sign << 63) | (UINT64_C(0x7ff) << 52);
                        return tmp.d;
                    }
                }
            }
            out = (sign << 63) | ((uint64_t)e << 52) | frac64;
            {
                union { double d; uint64_t u; } tmp;

                tmp.u = out;
                return tmp.d;
            }
        }
    }
}

/* --- binary128 comparison (NaN returns false) --- */

/* --- binary128 comparisons in the compiler-rt ABI --- */
static int tf_compare(long double a, long double b)
{
    union { long double f; unsigned __int128 u; } x, y;
    uint64_t xs, ys;
    unsigned __int128 xm, ym;

    x.f = a;
    y.f = b;
    xs = (uint64_t)(x.u >> 127);
    ys = (uint64_t)(y.u >> 127);
    if (x.u == y.u) {
        return 0;
    }
    if (xs != ys) {
        return xs == 0 ? 1 : -1;
    }
    xm = x.u & ((((unsigned __int128)1u) << 127) - 1u);
    ym = y.u & ((((unsigned __int128)1u) << 127) - 1u);
    if (xs == 0) {
        return xm > ym ? 1 : -1;
    }
    return xm < ym ? 1 : -1;
}

static int tf_is_nan(long double a)
{
    union { long double f; unsigned __int128 u; } x;
    uint64_t e;
    unsigned __int128 f;

    x.f = a;
    e = (uint64_t)(x.u >> 112) & 0x7fffu;
    f = x.u & (((unsigned __int128)1u << 112) - 1u);
    return e == 0x7fffu && f != 0;
}

int __unordtf2(long double a, long double b)
{
    return tf_is_nan(a) || tf_is_nan(b);
}

int __eqtf2(long double a, long double b)
{
    return (__unordtf2(a, b) == 0) && tf_compare(a, b) == 0;
}

int __netf2(long double a, long double b)
{
    return !__eqtf2(a, b);
}

int __lttf2(long double a, long double b)
{
    return (__unordtf2(a, b) == 0) && tf_compare(a, b) < 0;
}

int __letf2(long double a, long double b)
{
    return (__unordtf2(a, b) == 0) && tf_compare(a, b) <= 0;
}

int __gttf2(long double a, long double b)
{
    return (__unordtf2(a, b) == 0) && tf_compare(a, b) > 0;
}

int __getf2(long double a, long double b)
{
    return (__unordtf2(a, b) == 0) && tf_compare(a, b) >= 0;
}
