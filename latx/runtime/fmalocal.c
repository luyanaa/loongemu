/*
 * Small transcendental/rounding gaps left by the vendored math set.
 * fma/fmaf use the naive two-step evaluation: this differs from a fused
 * implementation in the final rounding bit, which is acceptable until the
 * x87 paths are validated on hardware.
 */
#include "math.h"

float fabsf(float x)
{
    union { float f; unsigned int u; } v;

    v.f = x;
    v.u &= 0x7fffffffu;
    return v.f;
}

float fmaf(float x, float y, float z)
{
    return (float)((double)x * (double)y + (double)z);
}

double fma(double x, double y, double z)
{
    return x * y + z;
}
