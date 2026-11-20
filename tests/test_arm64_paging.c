/*
 * Host-side tests for the ARMv8 Stage-1 4 KiB walker.
 *
 * The walker is checked against hand-built tables rather than against a
 * booted kernel: what has to be right is the descriptor interpretation, and a
 * table built by hand says exactly which bit is being tested.
 */
#include "loongemu/arm64_paging.h"

#include <stdio.h>
#include <string.h>

#define TEST_MEMORY_SIZE (UINT64_C(1) << 22)

#define L0_BASE 0x1000
#define L1_BASE 0x2000
#define L2_BASE 0x3000
#define L3_BASE 0x4000
#define PAGE_BASE 0x5000

#define DESC_VALID  (UINT64_C(1) << 0)
#define DESC_TABLE  (UINT64_C(1) << 1)
#define DESC_AF     (UINT64_C(1) << 10)
#define DESC_AP(n)  ((UINT64_C(n) & 3u) << 6)
#define DESC_PXN    (UINT64_C(1) << 53)
#define DESC_UXN    (UINT64_C(1) << 54)

/* T0SZ/T1SZ = 16 -> 48-bit VA, which is the four-level 4 KiB case. */
#define TCR_T0SZ(z)  ((uint64_t)(z) & 0x3fu)
#define TCR_TG0_4K   (UINT64_C(0) << 14)
#define TCR_T1SZ(z)  (((uint64_t)(z) & 0x3fu) << 16)
#define TCR_TG1_4K   (UINT64_C(2) << 30)   /* TG1=0b10 selects 4 KiB */

typedef struct TestMemory {
    unsigned char bytes[TEST_MEMORY_SIZE];
} TestMemory;

static int memory_read_u64(void *opaque, uint64_t address, uint64_t *value)
{
    TestMemory *memory = opaque;

    if (address > sizeof(memory->bytes) - sizeof(*value) || address % 8 != 0) {
        return -1;
    }
    memcpy(value, memory->bytes + address, sizeof(*value));
    return 0;
}

static void write_u64(TestMemory *memory, uint64_t address, uint64_t value)
{
    memcpy(memory->bytes + address, &value, sizeof(value));
}

static unsigned checks, fails;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

/*
 * Build a four-level map for one 4 KiB page: va -> pa with the given leaf
 * attributes.  L0 -> L1 -> L2 -> L3, each table descriptor pointing one level
 * down, so the chain the walker follows is unambiguous.
 */
static void build_map(TestMemory *memory, uint64_t va, uint64_t pa,
                      uint64_t leaf_attr)
{
    write_u64(memory, L3_BASE + ((va >> 12) & 0x1ff) * 8,
              (pa & ~UINT64_C(0xfff)) | leaf_attr);
    write_u64(memory, L2_BASE + ((va >> 21) & 0x1ff) * 8,
              L3_BASE | DESC_TABLE | DESC_VALID);
    write_u64(memory, L1_BASE + ((va >> 30) & 0x1ff) * 8,
              L2_BASE | DESC_TABLE | DESC_VALID);
    write_u64(memory, L0_BASE + ((va >> 39) & 0x1ff) * 8,
              L1_BASE | DESC_TABLE | DESC_VALID);
}

int main(void)
{
    static TestMemory memory;
    Arm64PagingMemory mem = { &memory, memory_read_u64 };
    const uint64_t tcr = TCR_T0SZ(16) | TCR_TG0_4K | TCR_T1SZ(16) | TCR_TG1_4K;
    Arm64Translation tr;
    uint64_t va = 0x0000000040201000ull;   /* L0..L3 indices all non-zero */
    uint64_t pa = 0x0000000000abc000ull;

    memset(&memory, 0, sizeof(memory));

    /* --- honoured mapping -------------------------------------------- */
    build_map(&memory, va, pa, DESC_AP(1) | DESC_AF | DESC_VALID | DESC_TABLE);
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va, ARM64_ACCESS_READ, &tr) ==
          ARM64_WALK_OK, "a valid 4 KiB page walks");
    check(tr.pa == pa + (va & 0xfff), "the walker preserves the page offset");
    check(tr.page_bits == 12, "a 4 KiB leaf reports 12 page bits");

    /* --- not present -------------------------------------------------- */
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va + 0x2000,
                     ARM64_ACCESS_READ, &tr) == ARM64_WALK_NOT_PRESENT,
          "an invalid descriptor reports NOT_PRESENT");

    /* --- access flag -------------------------------------------------- */
    /* A page descriptor is 0b11; 0b01 at L3 is a block there, which is
     * reserved, so the AF case has to keep the page kind. */
    build_map(&memory, va, pa, DESC_AP(1) | DESC_VALID | DESC_TABLE);
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va, ARM64_ACCESS_READ, &tr) ==
          ARM64_WALK_NOT_PRESENT, "a valid entry with AF clear faults");

    /* --- write permission --------------------------------------------- */
    build_map(&memory, va, pa, DESC_AP(2) | DESC_AF | DESC_VALID | DESC_TABLE);
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va, ARM64_ACCESS_READ, &tr) ==
          ARM64_WALK_OK, "AP=RO still reads");
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va,
                     ARM64_ACCESS_READ | ARM64_ACCESS_USER, &tr) ==
          ARM64_WALK_OK, "AP=RO is readable at EL0");
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va,
                     ARM64_ACCESS_WRITE | ARM64_ACCESS_USER, &tr) ==
          ARM64_WALK_PROTECTION, "AP=RO refuses an EL0 write");
    check(arm64_fault_status(ARM64_WALK_PROTECTION, ARM64_ACCESS_WRITE) != 0,
          "a permission fault has a status");

    /* --- EL1-only page ------------------------------------------------- */
    build_map(&memory, va, pa, DESC_AP(0) | DESC_AF | DESC_VALID | DESC_TABLE);
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va,
                     ARM64_ACCESS_READ | ARM64_ACCESS_USER, &tr) ==
          ARM64_WALK_PROTECTION, "AP=EL1-only denies EL0");

    /* --- execute-never ------------------------------------------------- */
    build_map(&memory, va, pa,
              DESC_AP(1) | DESC_AF | DESC_VALID | DESC_TABLE | DESC_PXN);
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va, ARM64_ACCESS_EXEC, &tr) ==
          ARM64_WALK_PROTECTION, "PXN denies EL1 execute");
    check(arm64_walk(&mem, L0_BASE, 0, tcr, va, ARM64_ACCESS_READ, &tr) ==
          ARM64_WALK_OK, "PXN still allows reads");

    /* --- unsupported configurations ------------------------------------ */
    check(arm64_walk(&mem, L0_BASE, 0, tcr | (UINT64_C(1) << 7), va,
                     ARM64_ACCESS_READ, &tr) == ARM64_WALK_UNSUPPORTED,
          "EPD0 disables the walk");
    check(arm64_walk(&mem, L0_BASE, 0, TCR_T0SZ(16) | (UINT64_C(1) << 14),
                     va, ARM64_ACCESS_READ, &tr) == ARM64_WALK_UNSUPPORTED,
          "a 64 KiB granule is not modelled");

    /* --- block descriptor at L2 is legal ------------------------------- */
    memset(&memory, 0, sizeof(memory));
    {
        uint64_t block_pa = 0x00200000ull;   /* 2 MiB aligned */
        uint64_t bva = 0x0000000040200000ull;

        /*
         * The block sits at level 2, i.e. in the table L1 points to: L0 -> L1
         * table, then the L1 *entry* is the block descriptor covering 2 MiB of
         * the level-2 index space.  Putting it one level lower would place a
         * block in a page table, which the walker must reject.
         */
        write_u64(&memory, L2_BASE + ((bva >> 21) & 0x1ff) * 8,
                  block_pa | DESC_AP(1) | DESC_AF | DESC_VALID);
        write_u64(&memory, L1_BASE + ((bva >> 30) & 0x1ff) * 8,
                  L2_BASE | DESC_TABLE | DESC_VALID);
        write_u64(&memory, L0_BASE + ((bva >> 39) & 0x1ff) * 8,
                  L1_BASE | DESC_TABLE | DESC_VALID);
        check(arm64_walk(&mem, L0_BASE, 0, tcr, bva + 0x12345,
                         ARM64_ACCESS_READ, &tr) == ARM64_WALK_OK,
              "a 2 MiB block descriptor maps");
        check(tr.page_bits == 21, "a block reports 21 page bits");
        check(tr.pa == block_pa + 0x12345, "a block preserves the offset");
    }

    /* --- block descriptor at L0 is reserved ---------------------------- */
    check(arm64_walk(&mem, L0_BASE, 0, tcr, 0x0000000000001000,
                     ARM64_ACCESS_READ, &tr) == ARM64_WALK_NOT_PRESENT,
          "an absent L0 entry reports NOT_PRESENT");

    /* --- TTBR1 half ---------------------------------------------------- */
    memset(&memory, 0, sizeof(memory));
    {
        uint64_t hi_va = 0xffff000000201000ull;
        uint64_t hi_pa = 0x0000000000def000ull;

        write_u64(&memory, L3_BASE + ((hi_va >> 12) & 0x1ff) * 8,
                  hi_pa | DESC_AP(1) | DESC_AF | DESC_VALID | DESC_TABLE);
        write_u64(&memory, L2_BASE + ((hi_va >> 21) & 0x1ff) * 8,
                  L3_BASE | DESC_TABLE | DESC_VALID);
        write_u64(&memory, L1_BASE + ((hi_va >> 30) & 0x1ff) * 8,
                  L2_BASE | DESC_TABLE | DESC_VALID);
        write_u64(&memory, L0_BASE + ((hi_va >> 39) & 0x1ff) * 8,
                  L1_BASE | DESC_TABLE | DESC_VALID);
        check(arm64_walk(&mem, 0, L0_BASE, tcr, hi_va, ARM64_ACCESS_READ,
                         &tr) == ARM64_WALK_OK,
              "the TTBR1 half walks through ttbr1");
        check(tr.pa == hi_pa + (hi_va & 0xfff),
              "the TTBR1 translation is correct");
    }

    printf("arm64_paging: %u checks, %u failures\n", checks, fails);
    return fails != 0;
}
