/*
 * Host-side probe of the vendored softfloat fused multiply-add.
 *
 * Why this exists: the RVV FMA batch was reverted because a direct
 * float32_muladd(1+2^-23, 1-2^-23, -1, 0) returned 0x337ffffe instead of the
 * IEEE fused value -2^-46 (0xa8800000). This test reproduces that on the host,
 * with no cross-compilation and no RISC-V hardware involved, and pins the cases
 * that do work so a future fix cannot regress them.
 *
 * Findings this test encodes (measured, not assumed):
 *   - the product alone rounds correctly (1+2^-23)(1-2^-23) -> 1.0;
 *   - an exactly representable  product + addend is correct (4*2+3 = 11);
 *   - a tiny addend is correct (1*1 + 2^-46 -> 1.0);
 *   - a plain subtractive case is correct (1+2^-23) - 1 = 2^-23;
 *   - the fused cancellation case is WRONG: -2^-46 comes out as
 *     0x337ffffe = 2^-24 * (1 - 2^-23), positive and 2^32 times too large;
 *   - the double-precision analogue is wrong the same way.
 *
 * The wrong cases are counted as "known broken" so this can live in the fast
 * host tier without turning the suite red on a defect nobody has fixed yet.
 * When softfloat is repaired, the known-broken count must drop to zero and the
 * driver should be changed to fail on it; the printf says which cases to flip.
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
     * Known broken: the cancellation cases that need the unrounded product.
     * IEEE: (1+2^-23)(1-2^-23) - 1 = -2^-46. Observed: 0x337ffffe.
     */
    expect32("fused cancellation", UINT32_C(0x3f800001),
             UINT32_C(0x3f7fffff), UINT32_C(0xbf800000), 0,
             UINT32_C(0xa8800000), true);
    expect64("fused cancellation (f64)", UINT64_C(0x3ff0000000000001),
             UINT64_C(0x3fefffffffffffff), UINT64_C(0xbff0000000000000), 0,
             UINT64_C(0xb970000000000000), true);

    printf("softfloat-fma: %d checks, %d fails, %d known-broken\n", checks,
           fails, known_broken);
    return fails == 0 ? 0 : 1;
}
