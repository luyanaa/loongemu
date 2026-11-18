/*
 * Unit tests for the CPU-side LBT glue: back-end selection and the
 * EFLAGS pack/unpack helpers.
 *
 * The pack/unpack pair is the bridge between the emulator's private EFLAGS
 * layout and the architectural x86 bit positions, and the mask is the only
 * thing that decides which flags an instruction is allowed to touch.  Both
 * are pure functions, so they are checked exhaustively where that is cheap.
 */
#include "loongemu/loongbt.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                  \
        }                                                                \
    } while (0)

/* Not part of the public surface; the tests reach in to compare back ends. */
uint64_t loongbt_exec_soft(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                           uint64_t a, uint64_t b);

/* Architectural x86 positions of the six flags the LBT subset carries. */
#define X86_CF 0
#define X86_PF 2
#define X86_AF 4
#define X86_ZF 6
#define X86_SF 7
#define X86_OF 11

/* Every flag set, so a wrong mask shows up as a leaked bit. */
#define ALL_FLAGS (LOONGBT_EF_CF | LOONGBT_EF_PF | LOONGBT_EF_AF | \
                   LOONGBT_EF_ZF | LOONGBT_EF_SF | LOONGBT_EF_OF)

static void test_pack_single_flags(void)
{
    /* Each eflags bit must land on its architectural position. */
    CHECK(loongbt_pack_flags(LOONGBT_EF_CF, 0x3f) == (UINT64_C(1) << X86_CF));
    CHECK(loongbt_pack_flags(LOONGBT_EF_PF, 0x3f) == (UINT64_C(1) << X86_PF));
    CHECK(loongbt_pack_flags(LOONGBT_EF_AF, 0x3f) == (UINT64_C(1) << X86_AF));
    CHECK(loongbt_pack_flags(LOONGBT_EF_ZF, 0x3f) == (UINT64_C(1) << X86_ZF));
    CHECK(loongbt_pack_flags(LOONGBT_EF_SF, 0x3f) == (UINT64_C(1) << X86_SF));
    CHECK(loongbt_pack_flags(LOONGBT_EF_OF, 0x3f) == (UINT64_C(1) << X86_OF));
}

static void test_pack_all_flags(void)
{
    uint64_t want = (UINT64_C(1) << X86_CF) | (UINT64_C(1) << X86_PF) |
                    (UINT64_C(1) << X86_AF) | (UINT64_C(1) << X86_ZF) |
                    (UINT64_C(1) << X86_SF) | (UINT64_C(1) << X86_OF);

    CHECK(loongbt_pack_flags(ALL_FLAGS, 0x3f) == want);
    CHECK(loongbt_pack_flags(0, 0x3f) == 0);
}

/*
 * The mask is what keeps MOV to/from a flag register from clobbering flags
 * the instruction does not name: bits outside the mask must stay clear in
 * the packed word, whatever eflags holds.
 */
static void test_pack_mask_isolation(void)
{
    static const uint8_t masks[] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20 };

    for (unsigned i = 0; i < sizeof(masks); i++) {
        uint64_t got = loongbt_pack_flags(ALL_FLAGS, masks[i]);
        uint64_t keep = (UINT64_C(1) << ((const int[]){ X86_CF, X86_PF, X86_AF,
                                                         X86_ZF, X86_SF, X86_OF })[i]);

        CHECK(got == keep);
        /* And the complement: nothing else survives at all. */
        CHECK(loongbt_pack_flags(ALL_FLAGS, (uint8_t)~masks[i] & 0x3f) ==
              (((UINT64_C(1) << X86_CF) | (UINT64_C(1) << X86_PF) |
                (UINT64_C(1) << X86_AF) | (UINT64_C(1) << X86_ZF) |
                (UINT64_C(1) << X86_SF) | (UINT64_C(1) << X86_OF)) & ~keep));
    }

    CHECK(loongbt_pack_flags(ALL_FLAGS, 0x00) == 0);
}

static void test_unpack_single_flags(void)
{
    uint32_t f;

    f = 0;
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_CF, 0x3f);
    CHECK(f == LOONGBT_EF_CF);

    f = 0;
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_PF, 0x3f);
    CHECK(f == LOONGBT_EF_PF);

    f = 0;
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_AF, 0x3f);
    CHECK(f == LOONGBT_EF_AF);

    f = 0;
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_ZF, 0x3f);
    CHECK(f == LOONGBT_EF_ZF);

    f = 0;
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_SF, 0x3f);
    CHECK(f == LOONGBT_EF_SF);

    f = 0;
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_OF, 0x3f);
    CHECK(f == LOONGBT_EF_OF);
}

/*
 * Bits in the value that the mask does not name must not reach eflags.
 * CF is the only flag the mask allows here, so a value that carries a stray
 * AF bit must leave the previous AF alone.
 */
static void test_unpack_ignores_unmasked_bits(void)
{
    uint32_t f = LOONGBT_EF_PF | LOONGBT_EF_AF;

    loongbt_unpack_flags(&f, UINT64_C(1) << X86_AF, 0x01);
    CHECK(f == (LOONGBT_EF_PF | LOONGBT_EF_AF));

    /* Masking PF off must leave the old PF untouched, not clear it. */
    f = LOONGBT_EF_PF;
    loongbt_unpack_flags(&f, 0, 0x01);
    CHECK(f == LOONGBT_EF_PF);

    /* A named flag is rewritten in both directions. */
    f = LOONGBT_EF_AF;
    loongbt_unpack_flags(&f, 0, 0x04);
    CHECK(f == 0);
    loongbt_unpack_flags(&f, UINT64_C(1) << X86_AF, 0x04);
    CHECK(f == LOONGBT_EF_AF);
}

/* A packed word with stray bits must not set flags the mask excludes. */
static void test_unpack_ignores_foreign_positions(void)
{
    uint32_t f = 0;
    uint64_t v = UINT64_C(1) << X86_CF;

    /* Bit 1 is not a flag position: unmapped by the mask semantics. */
    loongbt_unpack_flags(&f, v | (UINT64_C(1) << 1), 0x3f);
    CHECK(f == LOONGBT_EF_CF);
}

/*
 * pack/unpack must round-trip: the pair is used on both sides of a
 * save/restore, so any asymmetry would corrupt flags silently.
 */
static void test_pack_unpack_roundtrip(void)
{
    for (uint32_t f = 0; f <= ALL_FLAGS; f++) {
        uint64_t packed;
        uint32_t back = 0;

        if (f & ~ALL_FLAGS) {
            continue;
        }
        packed = loongbt_pack_flags(f, 0x3f);
        loongbt_unpack_flags(&back, packed, 0x3f);
        CHECK(back == f);
    }

    /*
     * Under a partial mask the pair restores the flags it names and leaves
     * the rest at their previous value -- unpack is a read-modify-write of
     * the selected flags, not a wholesale assignment.
     */
    for (unsigned m = 0; m < 0x40; m++) {
        static const uint32_t bit[] = { LOONGBT_EF_CF, LOONGBT_EF_PF,
                                        LOONGBT_EF_AF, LOONGBT_EF_ZF,
                                        LOONGBT_EF_SF, LOONGBT_EF_OF };
        uint32_t named = 0;
        uint32_t back = 0;

        for (unsigned i = 0; i < 6; i++) {
            if (m & (1u << i)) {
                named |= bit[i];
            }
        }
        loongbt_unpack_flags(&back, loongbt_pack_flags(ALL_FLAGS, (uint8_t)m),
                             (uint8_t)m);
        CHECK((back & named) == named);
    }
}

/*
 * A flag the mask does not name must be immune in both directions: neither
 * a 1 nor a 0 in the value may change it.  This is what makes MOV to a flag
 * register a partial update instead of a clobber.
 */
static void test_unpack_unmasked_flag_is_immune(void)
{
    static const uint32_t bit[] = { LOONGBT_EF_CF, LOONGBT_EF_PF, LOONGBT_EF_AF,
                                    LOONGBT_EF_ZF, LOONGBT_EF_SF, LOONGBT_EF_OF };
    static const uint64_t pos[] = { X86_CF, X86_PF, X86_AF, X86_ZF, X86_SF,
                                    X86_OF };

    for (unsigned i = 0; i < 6; i++) {
        unsigned mask_without = 0x3f & ~(1u << i);
        uint32_t f;

        /* Excluded flag set, value carries 0 for it: must stay set. */
        f = bit[i];
        loongbt_unpack_flags(&f, 0, (uint8_t)mask_without);
        CHECK(f & bit[i]);

        /* Excluded flag clear, value carries 1 for it: must stay clear. */
        f = 0;
        loongbt_unpack_flags(&f, UINT64_C(1) << pos[i], (uint8_t)mask_without);
        CHECK(!(f & bit[i]));
    }
}

/* Every subset of the mask must leave the flags outside it untouched. */
static void test_unpack_mask_isolation(void)
{
    static const uint32_t bit[] = { LOONGBT_EF_CF, LOONGBT_EF_PF, LOONGBT_EF_AF,
                                    LOONGBT_EF_ZF, LOONGBT_EF_SF, LOONGBT_EF_OF };

    for (unsigned m = 0; m < 0x40; m++) {
        uint32_t outside = 0, f;

        for (unsigned i = 0; i < 6; i++) {
            if (!(m & (1u << i))) {
                outside |= bit[i];
            }
        }
        /* Start with every flag set, so losing an excluded one shows up. */
        f = ALL_FLAGS;
        /* A value with every architectural bit set must not reach the
         * flags the mask excludes. */
        loongbt_unpack_flags(&f, (UINT64_C(1) << X86_CF) | (UINT64_C(1) << X86_PF) |
                                (UINT64_C(1) << X86_AF) | (UINT64_C(1) << X86_ZF) |
                                (UINT64_C(1) << X86_SF) | (UINT64_C(1) << X86_OF),
                            (uint8_t)m);
        CHECK((f & outside) == outside);   /* excluded flags survive */
    }
}

static void test_exec_backend_selection(void)
{
    LoongbtState st = { .scr = { 0 }, .eflags = 0, .top = 0, .tm = false };
    uint64_t r;

    /* The feature register is plain storage: read back exactly what was set. */
    loongbt_set_cpu_features(0);
    CHECK(loongbt_cpu_features() == 0);
    loongbt_set_cpu_features(LOONGEMU_CPU_LBT_X86);
    CHECK(loongbt_cpu_features() == LOONGEMU_CPU_LBT_X86);
    loongbt_set_hw_available(true);
    CHECK(loongbt_hw_available());
    loongbt_set_hw_available(false);
    CHECK(!loongbt_hw_available());

    /*
     * loongbt_exec() must never lose an operation: for every combination of
     * the feature register and the availability flag it has to produce the
     * same architectural result as the fallback.  On a host without LBT
     * loongbt_hw_probe() is false, so this also pins the requirement that the
     * dispatcher falls back rather than returning a stub value -- a back end
     * that returned 0, or skipped the probe, would break these.
     */
    for (int features = 0; features <= 1; features++) {
        for (int avail = 0; avail <= 1; avail++) {
            LoongbtState soft = { .scr = { 0 }, .eflags = 0, .top = 0,
                                  .tm = false };

            loongbt_set_cpu_features(features ? LOONGEMU_CPU_LBT_X86 : 0);
            loongbt_set_hw_available(avail != 0);

            st.eflags = soft.eflags = 0;
            r = loongbt_exec(&st, LOONGBT_ADD, LOONGBT_W, 0x7fffffffu, 1);
            CHECK(r == loongbt_exec_soft(&soft, LOONGBT_ADD, LOONGBT_W,
                                         0x7fffffffu, 1));
            CHECK(r == 0x80000000u);
            CHECK(st.eflags == soft.eflags);

            st.eflags = soft.eflags = 0;
            r = loongbt_exec(&st, LOONGBT_SUB, LOONGBT_W, 0, 1);
            CHECK(r == loongbt_exec_soft(&soft, LOONGBT_SUB, LOONGBT_W, 0, 1));
            CHECK(r == 0xffffffffu);
            CHECK(st.eflags == soft.eflags);

            st.eflags = soft.eflags = LOONGBT_EF_CF;
            r = loongbt_exec(&st, LOONGBT_RCL, LOONGBT_W, 0x89abcdefu, 1);
            CHECK(r == loongbt_exec_soft(&soft, LOONGBT_RCL, LOONGBT_W,
                                         0x89abcdefu, 1));
            CHECK(st.eflags == soft.eflags);
        }
    }

    /* Leave the globals as we found them for the rest of the suite. */
    loongbt_set_cpu_features(0);
    loongbt_set_hw_available(false);
}

int main(void)
{
    test_pack_single_flags();
    test_pack_all_flags();
    test_pack_mask_isolation();
    test_unpack_single_flags();
    test_unpack_ignores_unmasked_bits();
    test_unpack_ignores_foreign_positions();
    test_pack_unpack_roundtrip();
    test_unpack_unmasked_flag_is_immune();
    test_unpack_mask_isolation();
    test_exec_backend_selection();

    printf("loongbt_cpu: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
