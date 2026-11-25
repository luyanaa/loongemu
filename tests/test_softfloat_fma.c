/*
 * Host-side probe of the vendored softfloat fused multiply-add.
 *
 * Why this exists: the RVV FMA batch was reverted because a direct
 * float32_muladd(1+2^-23, 1-2^-23, -1, 0) returned 0x337ffffe instead of the
 * IEEE fused value -2^-46 (0xa8800000). This test reproduces that on the host,
 * with no cross-compilation and no RISC-V hardware involved, and pins the cases
 * that do work so a future fix cannot regress them.
 *
 * History worth keeping: the first version of this file declared softfloat
 * broken. It was the test that was wrong - 0x3f7fffff is 1-2^-24, not 1-2^-23,
 * so the "expected" fused result had been computed from the wrong operands.
 * Instrumenting parts_muladd showed the widened product and the post-subtract
 * value matching the operands exactly. A discriminating fused case has to use
 * operands whose product needs more precision than the format carries, e.g.
 * (1+2^-13)(1-2^-13) = 1-2^-26, which rounds to 1.0 on its own and only the
 * fused add of -1 exposes the tail.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "fpu/softfloat.h"

/* The upstream softfloat build expects the QEMU assertion hook. */
void g_assert_not_reached(void)
{
    __builtin_trap();
}

static int checks;
static int fails;
static int known_broken;

static void setup(float_status *st)
{
    *st = (float_status){ 0 };
    set_float_detect_tininess(float_tininess_after_rounding, st);
    set_float_rounding_mode(float_round_nearest_even, st);
    set_default_nan_mode(true, st);
    set_snan_bit_is_one(false, st);
}

static void expect32(const char *what, float32 a, float32 b, float32 c,
                     int flags, uint32_t want, bool known)
{
    float_status st;
    float32 got;

    setup(&st);
    got = float32_muladd(a, b, c, flags, &st);
    checks++;
    if (got != want) {
        if (known) {
            known_broken++;
            printf("known-broken %s: got=0x%08x want=0x%08x\n", what, got, want);
        } else {
            fails++;
            printf("FAIL %s: got=0x%08x want=0x%08x\n", what, got, want);
        }
    }
}

static void expect64(const char *what, float64 a, float64 b, float64 c,
                     int flags, uint64_t want, bool known)
{
    float_status st;
    float64 got;

    setup(&st);
    got = float64_muladd(a, b, c, flags, &st);
    checks++;
    if (got != want) {
        if (known) {
            known_broken++;
            printf("known-broken %s: got=0x%016llx want=0x%016llx\n", what,
                   (unsigned long long)got, (unsigned long long)want);
        } else {
            fails++;
            printf("FAIL %s: got=0x%016llx want=0x%016llx\n", what,
                   (unsigned long long)got, (unsigned long long)want);
        }
    }
}

int main(void)
{
    /* Working cases: rounding, exact arithmetic, sign flags, tiny addends. */
    expect32("product only", UINT32_C(0x3f800001), UINT32_C(0x3f7fffff),
             UINT32_C(0x00000000), 0, UINT32_C(0x3f800000), false);
    expect32("exact product + addend", UINT32_C(0x40800000),
             UINT32_C(0x40000000), UINT32_C(0x40400000), 0,
             UINT32_C(0x41300000), false);
    expect32("subtract exact", UINT32_C(0x3f800001), UINT32_C(0x3f800000),
             UINT32_C(0xbf800000), 0, UINT32_C(0x34000000), false);
    expect32("tiny addend", UINT32_C(0x3f800000), UINT32_C(0x3f800000),
             UINT32_C(0x28800000), 0, UINT32_C(0x3f800000), false);
    expect32("negate product", UINT32_C(0x40000000), UINT32_C(0x40400000),
             UINT32_C(0x40800000), float_muladd_negate_product,
             UINT32_C(0xc0000000), false);
    expect32("negate addend", UINT32_C(0x40000000), UINT32_C(0x40400000),
             UINT32_C(0x40800000), float_muladd_negate_c,
             UINT32_C(0x40000000), false);

    /*
     * Single rounding, f32: (1+2^-13)(1-2^-13) = 1-2^-26, which rounds to 1.0 on
     * its own; fused with -1 the tail must survive as -2^-26 (0xb2800000).
     * A mul-then-add implementation would return 0.
     */
    expect32("fused cancellation", UINT32_C(0x3f800400),
             UINT32_C(0x3f7ff800), UINT32_C(0xbf800000), 0,
             UINT32_C(0xb2800000), false);
    /* f64 analogue: (1+2^-30)(1-2^-30) - 1 = -2^-60. */
    expect64("fused cancellation (f64)", UINT64_C(0x3ff0000000400000),
             UINT64_C(0x3fefffffff800000), UINT64_C(0xbff0000000000000), 0,
             UINT64_C(0xbc30000000000000), false);

    printf("softfloat-fma: %d checks, %d fails, %d known-broken\n", checks,
           fails, known_broken);
    return fails == 0 ? 0 : 1;
}
