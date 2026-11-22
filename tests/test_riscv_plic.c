/* Host tests for the PLIC model. */
#include <stdio.h>
#include <string.h>
#include "loongemu/riscv_plic.h"

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "PLIC: FAIL %s\n", name);                       \
        }                                                                   \
    } while (0)

static uint32_t rd(LoongemuPlic *p, uint32_t off, int *err)
{
    return loongemu_plic_read32(p, off, err);
}

static void wr(LoongemuPlic *p, uint32_t off, uint32_t value)
{
    int err = 0;
    loongemu_plic_write32(p, off, value, &err);
}

int main(void)
{
    LoongemuPlic plic;
    int err;

    CHECK(loongemu_plic_init(&plic, 64, 2) == 1, "init accepts valid counts");
    CHECK(loongemu_plic_init(&plic, LOONGEMU_PLIC_MAX_SOURCES + 1, 1) == 0,
          "init rejects too many sources");
    CHECK(loongemu_plic_init(&plic, 4, LOONGEMU_PLIC_MAX_CONTEXTS + 1) == 0,
          "init rejects too many contexts");
    CHECK(loongemu_plic_init(&plic, 64, 2) == 1, "re-init succeeds");

    /* Priority and threshold gating. */
    wr(&plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 5, 3);
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 5, &err) == 3,
          "priority round-trips");
    CHECK(rd(&plic, LOONGEMU_PLIC_PRIORITY_BASE, &err) == 0 && err == 1,
          "source 0 has no priority register");
    wr(&plic, LOONGEMU_PLIC_ENABLE_BASE, 1u << 5);
    CHECK(loongemu_plic_pending(&plic, 0) == 0,
          "no pending before the line asserts");
    loongemu_plic_set_irq(&plic, 5, 1);
    CHECK(loongemu_plic_pending(&plic, 0) == 1, "pending after assert");
    CHECK(loongemu_plic_pending(&plic, 1) == 0,
          "enable bits are per context");
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_PENDING_BASE, &err) == (1u << 5),
          "pending register reports the source");
    wr(&plic, LOONGEMU_PLIC_PENDING_BASE, 0xffffffff); /* read-only */
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_PENDING_BASE, &err) == (1u << 5),
          "pending register ignores writes");

    /* Threshold blocks low priorities. */
    wr(&plic, LOONGEMU_PLIC_CONTEXT_BASE, 3);
    CHECK(loongemu_plic_pending(&plic, 0) == 0,
          "threshold equal to priority blocks the interrupt");
    wr(&plic, LOONGEMU_PLIC_CONTEXT_BASE, 2);
    CHECK(loongemu_plic_pending(&plic, 0) == 1,
          "threshold below the priority admits it");

    /* Claim picks the highest priority and clears pending. */
    wr(&plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 6, 5);
    wr(&plic, LOONGEMU_PLIC_ENABLE_BASE, (1u << 5) | (1u << 6));
    loongemu_plic_set_irq(&plic, 6, 1);
    err = 0;
    uint32_t id = rd(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, &err);
    CHECK(id == 6, "claim returns the highest priority source");
    CHECK(rd(&plic, LOONGEMU_PLIC_PENDING_BASE, &err) == (1u << 5),
          "claim clears the claimed pending bit");
    CHECK(rd(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, &err) == 5,
          "next claim returns the remaining source");
    CHECK(rd(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, &err) == 0,
          "claim returns zero when nothing is pending");

    /* Completing a still-asserted line re-pends it. */
    wr(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, 5);
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_PENDING_BASE, &err) == (1u << 5),
          "completing a still-asserted line re-pends it");
    CHECK(loongemu_plic_pending(&plic, 0) == 1,
          "re-pended source is claimable again");

    /* Level deassert + reassert. */
    loongemu_plic_set_irq(&plic, 5, 1);
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, &err) == 5,
          "reasserted line is claimable again");
    wr(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, 5); /* complete while high */
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_PENDING_BASE, &err) == (1u << 5),
          "complete with the line still high re-pends it");
    loongemu_plic_set_irq(&plic, 5, 0);
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_PENDING_BASE, &err) == 0,
          "deassert clears pending");

    /* Tie-break: equal priorities pick the lowest source id. */
    wr(&plic, LOONGEMU_PLIC_ENABLE_BASE, (1u << 3) | (1u << 9));
    wr(&plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 3, 4);
    wr(&plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 9, 4);
    loongemu_plic_set_irq(&plic, 9, 1);
    loongemu_plic_set_irq(&plic, 3, 1);
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, &err) == 3,
          "equal priorities break ties to the lowest source");
    wr(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, 3);
    wr(&plic, LOONGEMU_PLIC_CONTEXT_BASE + 4, 9);

    /* Out-of-range and reserved accesses report errors. */
    err = 0;
    (void)rd(&plic, 0x3f0000, &err);
    CHECK(err == 1, "unmapped PLIC offset errors");
    wr(&plic, LOONGEMU_PLIC_ENABLE_BASE, 0xffffffff);
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_ENABLE_BASE, &err) == 0xfffffffeu,
          "enable words cover every source");
    CHECK(rd(&plic, LOONGEMU_PLIC_ENABLE_BASE + 4 * 3, &err) == 0,
          "enable words beyond the source count read as zero");

    /* Contexts are independent. */
    wr(&plic, LOONGEMU_PLIC_ENABLE_BASE + LOONGEMU_PLIC_ENABLE_STRIDE,
       1u << 5);
    loongemu_plic_set_irq(&plic, 5, 1);
    CHECK(loongemu_plic_pending(&plic, 1) == 1,
          "second context sees the enabled source");
    err = 0;
    CHECK(rd(&plic, LOONGEMU_PLIC_CONTEXT_BASE +
             LOONGEMU_PLIC_CONTEXT_STRIDE + 4, &err) == 5,
          "second context claims from its own window");

    /* NULL / invalid arguments are safe. */
    CHECK(loongemu_plic_pending(NULL, 0) == 0, "NULL pending is safe");
    err = 0;
    CHECK(rd(NULL, 0, &err) == 0 && err == 1, "NULL read errors");
    wr(NULL, 0, 0);

    printf("PLIC: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
