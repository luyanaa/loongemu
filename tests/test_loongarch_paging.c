/*
 * Unit tests for the LoongArch 4 KiB page table builder.
 *
 * The table lives in a heap buffer here and in real UEFI memory under the
 * VMM; the code under test cannot tell the difference, so these tests cover
 * the parts of the P0 path that do not need a CPU: entry encoding, the pool
 * allocator, map/walk symmetry, and the permission model.
 */
#include "loongemu/loongarch_paging.h"

#include <stdio.h>
#include <string.h>

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

/*
 * A physical memory image: 1 MiB of heap, addressed by physical offset from
 * the start of the buffer.  Reads and writes go through these callbacks so
 * the walker cannot accidentally use host pointers.
 */
#define POOL_PAGES 256
#define POOL_BYTES (POOL_PAGES * LAPG_PAGE_SIZE)

static unsigned char arena[POOL_BYTES] __attribute__((aligned(LAPG_PAGE_SIZE)));

/*
 * Physical addresses are simulated as offsets into the arena, starting at
 * ARENA_BASE so that a page-zero mapping is distinguishable from an
 * unmapped one.
 */
#define ARENA_BASE UINT64_C(0x80000000)

static int arena_read(void *opaque, uint64_t phys, uint64_t *value)
{
    uint64_t off;

    (void)opaque;
    if (phys < ARENA_BASE) {
        return 0;
    }
    off = phys - ARENA_BASE;
    if (off + sizeof(*value) > POOL_BYTES || (off & 7) != 0) {
        return 0;
    }
    memcpy(value, arena + off, sizeof(*value));
    return 1;
}

static int arena_write(void *opaque, uint64_t phys, uint64_t value)
{
    uint64_t off;

    (void)opaque;
    if (phys < ARENA_BASE) {
        return 0;
    }
    off = phys - ARENA_BASE;
    if (off + sizeof(value) > POOL_BYTES || (off & 7) != 0) {
        return 0;
    }
    memcpy(arena + off, &value, sizeof(value));
    return 1;
}

static uint64_t arena_phys(void)
{
    return ARENA_BASE;
}

/* The pool hands out physical addresses, so the arena is described by its
 * address in the same space the callbacks use. */
static void arena_reset(LapgPagePool *pool)
{
    memset(arena, 0, sizeof(arena));
    lapg_pool_init(pool, arena_phys(), POOL_BYTES);
}

static void test_pool_allocator(void)
{
    LapgPagePool pool;
    uint64_t a, b, c;

    arena_reset(&pool);
    CHECK(lapg_pool_pages_remaining(&pool) == POOL_PAGES);

    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &a));
    CHECK(a == arena_phys());
    CHECK(lapg_pool_pages_remaining(&pool) == POOL_PAGES - 1);

    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &b));
    CHECK(b == a + LAPG_PAGE_SIZE);

    /* A multi-page request is rounded up, never split. */
    CHECK(lapg_pool_alloc(&pool, 3 * LAPG_PAGE_SIZE, &c));
    CHECK(c == b + LAPG_PAGE_SIZE);
    CHECK(lapg_pool_pages_remaining(&pool) == POOL_PAGES - 5);

    /* Zero-byte and over-large requests fail without consuming space. */
    CHECK(!lapg_pool_alloc(&pool, 0, &a));
    CHECK(!lapg_pool_alloc(&pool, POOL_BYTES + LAPG_PAGE_SIZE, &a));
    CHECK(lapg_pool_pages_remaining(&pool) == POOL_PAGES - 5);

    /* Draining exactly to the end works, and one more page fails. */
    {
        LapgPagePool small;

        lapg_pool_init(&small, arena_phys(), 2 * LAPG_PAGE_SIZE);
        CHECK(lapg_pool_alloc(&small, 2 * LAPG_PAGE_SIZE, &a));
        CHECK(lapg_pool_pages_remaining(&small) == 0);
        CHECK(!lapg_pool_alloc(&small, LAPG_PAGE_SIZE, &a));
    }
}

static void test_csr_encoding(void)
{
    uint64_t pwcl = lapg_pwcl_value();
    uint64_t pwch = lapg_pwch_value();

    /* The registers store (base, width-1) pairs; check each field lands
     * where the hardware reads it. */
    CHECK(((pwcl >> 0) & 0x3f) == LAPG_DIR1_BASE);
    CHECK(((pwcl >> 6) & 0x3f) == LAPG_DIR1_WIDTH - 1);
    CHECK(((pwcl >> 12) & 0x3f) == LAPG_DIR2_BASE);
    CHECK(((pwcl >> 18) & 0x3f) == LAPG_DIR2_WIDTH - 1);
    CHECK(((pwcl >> 24) & 0x3f) == LAPG_PT_BASE);
    CHECK(((pwcl >> 30) & 0x3f) == LAPG_PT_WIDTH - 1);
    CHECK(((pwcl >> 36) & 0x3) == 0); /* PTEWIDTH: 64-bit entries */

    CHECK(((pwch >> 0) & 0x3f) == LAPG_DIR3_BASE);
    CHECK(((pwch >> 6) & 0x3f) == LAPG_DIR3_WIDTH - 1);

    /* 4 KiB granule. */
    CHECK(lapg_stlbps_value() == 12);
    CHECK((UINT64_C(1) << lapg_stlbps_value()) == LAPG_PAGE_SIZE);
}

static void test_leaf_encoding(void)
{
    uint64_t pte;

    /* Read/write/execute, supervisor. */
    pte = lapg_make_leaf(0x12345, LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
    CHECK(pte & LAPG_PTE_V);
    CHECK(pte & LAPG_PTE_D);   /* writable => D set up front, no PME later */
    CHECK(!(pte & LAPG_PTE_NR));  /* readable  */
    CHECK(!(pte & LAPG_PTE_NX));  /* executable */
    CHECK(((pte >> 12) & 0xfffffffffULL) == 0x12345);
    CHECK((pte & LAPG_PTE_PLV_MASK) == LAPG_PTE_PLV0);

    /* The inhibit bits are set for the access that is *not* granted. */
    pte = lapg_make_leaf(1, LAPG_PERM_WRITE);
    CHECK(pte & LAPG_PTE_NR);
    CHECK(pte & LAPG_PTE_NX);

    /* Read-only: readable, not executable, and D left clear so a store
     * traps with PME instead of silently succeeding. */
    pte = lapg_make_leaf(1, LAPG_PERM_READ);
    CHECK(!(pte & LAPG_PTE_NR));
    CHECK(pte & LAPG_PTE_NX);
    CHECK(!(pte & LAPG_PTE_D));

    pte = lapg_make_leaf(1, LAPG_PERM_EXEC);
    CHECK(pte & LAPG_PTE_NR);
    CHECK(!(pte & LAPG_PTE_NX));

    /* User pages carry PLV3. */
    pte = lapg_make_leaf(1, LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_USER);
    CHECK((pte & LAPG_PTE_PLV_MASK) == LAPG_PTE_PLV3);

    /* A page number wider than the PPN field must not corrupt the flags. */
    pte = lapg_make_leaf(UINT64_C(1) << 40, LAPG_PERM_READ);
    CHECK(pte & LAPG_PTE_V);
}

static void test_table_encoding(void)
{
    uint64_t pte = lapg_make_table(0x4000);

    CHECK(pte & LAPG_PTE_V);
    CHECK(((pte >> 12) & 0xfffffffffULL) == 4);
    /* A table entry must not look like a leaf with permissions. */
    CHECK(!(pte & LAPG_PTE_NR));
    CHECK(!(pte & LAPG_PTE_NX));
}

/* Map a handful of pages, then walk them back. */
static void test_map_walk_roundtrip(void)
{
    static const uint64_t vas[] = {
        0x0, 0x1000, 0x200000, 0x40000000, 0x7ffff000,
        0x9000000000200000ULL, 0xfffffffffffff000ULL,
    };
    LapgPagePool pool;
    LapgMemory memory = { .opaque = 0, .read_u64 = arena_read,
                          .write_u64 = arena_write };
    uint64_t root;

    arena_reset(&pool);
    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &root));

    for (unsigned i = 0; i < sizeof(vas) / sizeof(vas[0]); i++) {
        uint64_t pa = arena_phys() + (uint64_t)(i + 8) * LAPG_PAGE_SIZE;
        LapgTranslation tr;

        CHECK(lapg_map(&memory, &pool, root, vas[i], pa,
                       LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC) ==
              LAPG_OK);
        CHECK(lapg_walk(&memory, root, vas[i], LAPG_PERM_READ, &tr) == LAPG_OK);
        CHECK(tr.phys == pa);
        CHECK(tr.page_bits == LAPG_PAGE_BITS);
        CHECK((tr.perm & LAPG_PERM_READ) && (tr.perm & LAPG_PERM_WRITE) &&
              (tr.perm & LAPG_PERM_EXEC));
    }

    /* Within a page the offset is preserved. */
    {
        LapgTranslation tr;
        uint64_t va = 0x200123;

        CHECK(lapg_map(&memory, &pool, root, 0x200000,
                       arena_phys() + 0x1000,
                       LAPG_PERM_READ) == LAPG_OK);
        CHECK(lapg_walk(&memory, root, va, LAPG_PERM_READ, &tr) == LAPG_OK);
        CHECK(tr.phys == arena_phys() + 0x1000 + 0x123);
    }
}

static void test_unmapped_and_protection(void)
{
    LapgPagePool pool;
    LapgMemory memory = { .opaque = 0, .read_u64 = arena_read,
                          .write_u64 = arena_write };
    LapgTranslation tr;
    uint64_t root;

    arena_reset(&pool);
    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &root));

    /* Nothing mapped yet. */
    CHECK(lapg_walk(&memory, root, 0x40000, LAPG_PERM_READ, &tr) ==
          LAPG_FAULT_UNMAPPED);

    /* A read-only page must reject a write and an execute. */
    CHECK(lapg_map(&memory, &pool, root, 0x8000, arena_phys() + 0x1000,
                   LAPG_PERM_READ) == LAPG_OK);
    CHECK(lapg_walk(&memory, root, 0x8000, LAPG_PERM_READ, &tr) == LAPG_OK);
    CHECK(lapg_walk(&memory, root, 0x8000, LAPG_PERM_WRITE, &tr) ==
          LAPG_FAULT_PROTECTION);
    CHECK(lapg_walk(&memory, root, 0x8000, LAPG_PERM_EXEC, &tr) ==
          LAPG_FAULT_PROTECTION);

    /*
     * A write-only page (NR set) must reject a load.  This is the case that
     * makes the NR bit load-bearing rather than decorative.
     */
    CHECK(lapg_map(&memory, &pool, root, 0xa000, arena_phys() + 0x3000,
                   LAPG_PERM_WRITE) == LAPG_OK);
    CHECK(lapg_walk(&memory, root, 0xa000, LAPG_PERM_WRITE, &tr) == LAPG_OK);
    CHECK(!(tr.perm & LAPG_PERM_READ));
    CHECK(lapg_walk(&memory, root, 0xa000, LAPG_PERM_READ, &tr) ==
          LAPG_FAULT_PROTECTION);

    /* Two VAs sharing the top three levels still differ at the leaf. */
    CHECK(lapg_map(&memory, &pool, root, 0x9000, arena_phys() + 0x2000,
                   LAPG_PERM_READ | LAPG_PERM_EXEC) == LAPG_OK);
    CHECK(lapg_walk(&memory, root, 0x8000, LAPG_PERM_READ, &tr) == LAPG_OK);
    CHECK(tr.phys == arena_phys() + 0x1000);
    CHECK(lapg_walk(&memory, root, 0x9000, LAPG_PERM_READ, &tr) == LAPG_OK);
    CHECK(tr.phys == arena_phys() + 0x2000);
}

/* A misaligned root or a corrupt table must be reported, not followed. */
static void test_malformed_input(void)
{
    LapgPagePool pool;
    LapgMemory memory = { .opaque = 0, .read_u64 = arena_read,
                          .write_u64 = arena_write };
    LapgTranslation tr;
    uint64_t root;

    arena_reset(&pool);
    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &root));

    CHECK(lapg_map(&memory, &pool, root + 1, 0x1000, arena_phys(),
                   LAPG_PERM_READ) == LAPG_FAULT_INVALID);
    CHECK(lapg_map(&memory, &pool, root, 0x1000, arena_phys() + 1,
                   LAPG_PERM_READ) == LAPG_FAULT_INVALID);
    CHECK(lapg_walk(&memory, root + 1, 0x1000, LAPG_PERM_READ, &tr) ==
          LAPG_FAULT_INVALID);

    /* An out-of-range physical address is a memory error, not a walk. */
    CHECK(lapg_walk(&memory, ARENA_BASE + POOL_BYTES, 0x1000, LAPG_PERM_READ, &tr) ==
          LAPG_FAULT_MEMORY);
}

/* Mapping the same VA twice must replace the leaf, not chain another table. */
static void test_remap(void)
{
    LapgPagePool pool;
    LapgMemory memory = { .opaque = 0, .read_u64 = arena_read,
                          .write_u64 = arena_write };
    LapgTranslation tr;
    uint64_t root;
    uint64_t after_first;

    arena_reset(&pool);
    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &root));

    CHECK(lapg_map(&memory, &pool, root, 0x10000, arena_phys() + 0x1000,
                   LAPG_PERM_READ) == LAPG_OK);
    after_first = lapg_pool_pages_remaining(&pool);
    CHECK(lapg_map(&memory, &pool, root, 0x10000, arena_phys() + 0x2000,
                   LAPG_PERM_READ | LAPG_PERM_WRITE) == LAPG_OK);
    /* No new tables were allocated for the second mapping. */
    CHECK(lapg_pool_pages_remaining(&pool) == after_first);

    CHECK(lapg_walk(&memory, root, 0x10000, LAPG_PERM_READ, &tr) == LAPG_OK);
    CHECK(tr.phys == arena_phys() + 0x2000);
    CHECK(tr.perm & LAPG_PERM_WRITE);
}

/* Exhausting the pool must fail cleanly rather than hand out a page. */
static void test_pool_exhaustion(void)
{
    LapgPagePool pool;
    LapgMemory memory = { .opaque = 0, .read_u64 = arena_read,
                          .write_u64 = arena_write };
    uint64_t root;
    LapgResult r = LAPG_OK;
    int mapped = 0;

    arena_reset(&pool);
    CHECK(lapg_pool_alloc(&pool, LAPG_PAGE_SIZE, &root));

    /*
     * Walk VAs that each need a fresh level-4 table: stepping by 2 MiB moves
     * the PT index, stepping by 1 GiB moves the PD index too.  Consecutive
     * 4 KiB pages would all share the tables and never exhaust anything.
     */
    for (uint64_t i = 0; i < 8192; i++) {
        uint64_t va = i * (UINT64_C(1) << 21);

        r = lapg_map(&memory, &pool, root, va,
                     arena_phys() + LAPG_PAGE_SIZE, LAPG_PERM_READ);
        if (r != LAPG_OK) {
            break;
        }
        mapped++;
    }
    /* Running out of pool surfaces as a memory error, not a bad address. */
    CHECK(r == LAPG_FAULT_MEMORY);
    CHECK(mapped > 0);

    /* Every VA that reported success still walks. */
    for (uint64_t i = 0; i < (uint64_t)mapped; i++) {
        LapgTranslation tr;

        CHECK(lapg_walk(&memory, root, i * (UINT64_C(1) << 21), LAPG_PERM_READ,
                        &tr) == LAPG_OK);
    }
}

int main(void)
{
    test_pool_allocator();
    test_csr_encoding();
    test_leaf_encoding();
    test_table_encoding();
    test_map_walk_roundtrip();
    test_unmapped_and_protection();
    test_malformed_input();
    test_remap();
    test_pool_exhaustion();

    printf("loongarch_paging: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
