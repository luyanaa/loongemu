/*
 * Host-math boundary for QEMU's softfloat on a freestanding LoongArch build.
 * The compiler lowers these to the scalar FP instructions, so no libm is
 * required.
 */
#include <stdint.h>

float fmaf(float a, float b, float c)
{
    return __builtin_fmaf(a, b, c);
}

double fma(double a, double b, double c)
{
    return __builtin_fma(a, b, c);
}

float sqrtf(float a)
{
    return __builtin_sqrtf(a);
}

double sqrt(double a)
{
    return __builtin_sqrt(a);
}

float fabsf(float a)
{
    return __builtin_fabsf(a);
}

double fabs(double a)
{
    return __builtin_fabs(a);
}

/* Compiler-rt's 128-bit unsigned divide, without calling itself. */
__uint128_t __udivti3(__uint128_t n, __uint128_t d)
{
    __uint128_t q = 0;

    if (d == 0) {
        return 0;
    }
    for (int i = 127; i >= 0; i--) {
        if ((n >> i) >= d) {
            q |= (__uint128_t)1 << i;
            n -= d << i;
        }
    }
    return q;
}
