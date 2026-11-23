/* FPSR flag mapping and stickiness, host-side. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <string.h>

#include "lata-fpsr.h"

/* FPSR lives in the legacy vfp.xregs slot; keep the index local to the test. */
#define TEST_FPSR_SLOT 8

static unsigned checks, failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main(void)
{
    uint64_t slot;

    /* Pure mapping: one host flag -> one guest bit, reversed order. */
    CHECK(lata_fpsr_flags_from_fcsr(LATA_FCSR_FLAG_V) == LATA_FPSR_IOC);
    CHECK(lata_fpsr_flags_from_fcsr(LATA_FCSR_FLAG_Z) == LATA_FPSR_DZC);
    CHECK(lata_fpsr_flags_from_fcsr(LATA_FCSR_FLAG_O) == LATA_FPSR_OFC);
    CHECK(lata_fpsr_flags_from_fcsr(LATA_FCSR_FLAG_U) == LATA_FPSR_UFC);
    CHECK(lata_fpsr_flags_from_fcsr(LATA_FCSR_FLAG_I) == LATA_FPSR_IXC);
    CHECK(lata_fpsr_flags_from_fcsr(LATA_FCSR_FLAG_MASK) == 0x1f);
    /* LoongArch Vol1 Table 6: bits 4:0 are enables, not flags. */
    CHECK(lata_fpsr_flags_from_fcsr(0x1f) == 0);
    CHECK(lata_fpsr_flags_from_fcsr(0x001f0000) == 0x1f);   /* V Z O U I */
    CHECK(lata_fpsr_flags_from_fcsr(0x000f0000) == 0x1e);   /* V clear */

    /* Accumulation is sticky and ignores the FCSR enable bits (15:8). */
    slot = 0;
    lata_fpsr_accumulate(&slot, LATA_FCSR_FLAG_I);
    CHECK(lata_fpsr_read(&slot) == LATA_FPSR_IXC);
    lata_fpsr_accumulate(&slot, LATA_FCSR_FLAG_Z | 0x0f00u);
    CHECK(lata_fpsr_read(&slot) == (LATA_FPSR_IXC | LATA_FPSR_DZC));
    lata_fpsr_accumulate(&slot, 0);
    CHECK(lata_fpsr_read(&slot) == (LATA_FPSR_IXC | LATA_FPSR_DZC));

    /* QC is not touched by the IEEE-flag path, and writes replace the word. */
    slot |= (UINT64_C(1) << 27);
    lata_fpsr_accumulate(&slot, LATA_FCSR_FLAG_V);
    CHECK(lata_fpsr_read(&slot) &
          ((UINT32_C(1) << 27) | LATA_FPSR_IOC | LATA_FPSR_IXC));
    lata_fpsr_write(&slot, 0);
    CHECK(lata_fpsr_read(&slot) == 0);
    lata_fpsr_accumulate(NULL, 0x1f); /* must not crash */
    CHECK(lata_fpsr_read(NULL) == 0);

    printf("FPSR: %u checks passed", checks);
    if (failures != 0) {
        printf(", %u FAILED\n", failures);
        return 1;
    }
    printf("\n");
    return 0;
}
