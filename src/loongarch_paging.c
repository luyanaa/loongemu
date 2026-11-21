/*
 * LoongArch 4 KiB hardware page table construction and walking.
 *
 * See loongarch_paging.h for the level layout.  Everything here is pure
 * arithmetic over a caller-owned physical memory region, which is what makes
 * it testable on the host: the same code runs under the UEFI VMM, where the
 * physical memory is real, and under the test harness, where it is a heap
 * buffer.
 */
#include "loongemu/loongarch_paging.h"

/* Bits of the VA that select the entry at each level, most significant
 * first.  Level 1 is the top of the four. */
static const unsigned level_base[LAPG_LEVELS] = {
    LAPG_DIR1_BASE, LAPG_DIR2_BASE, LAPG_PT_BASE, LAPG_DIR3_BASE,
};
static const unsigned level_width[LAPG_LEVELS] = {
    LAPG_DIR1_WIDTH, LAPG_DIR2_WIDTH, LAPG_PT_WIDTH, LAPG_DIR3_WIDTH,
};

static uint64_t index_mask(unsigned width)
{
    return (UINT64_C(1) << width) - 1;
}

/*
 * The PPN field is bits [47:12] -- 36 bits.  Shifting right and left only
 * clears the low flags; the bits above 47 (NX, NR, RPLV) would survive and
 * turn a table pointer into a wild address, so the field is masked
 * explicitly.
 */
static uint64_t entry_ppn(uint64_t entry)
{
    uint64_t ppn_mask = (UINT64_C(1) << 36) - 1;

    return ((entry >> LAPG_PTE_PPN_SHIFT) & ppn_mask) << LAPG_PTE_PPN_SHIFT;
}

static unsigned va_index(uint64_t va, int level)
{
    return (unsigned)((va >> level_base[level]) & index_mask(level_width[level]));
}

void lapg_pool_init(LapgPagePool *pool, uint64_t base, uint64_t size)
{
    /* Trim to whole pages: a partial page cannot be handed out without the
     * last page running past the reservation. */
    pool->base = base & LAPG_PAGE_MASK;
    pool->size = size & LAPG_PAGE_MASK;
    pool->used = 0;
    pool->pages_total = pool->size >> LAPG_PAGE_BITS;
    pool->pages_used = 0;
}

int lapg_pool_alloc(LapgPagePool *pool, uint64_t bytes, uint64_t *out)
{
    uint64_t rounded;

    if (bytes == 0) {
        return 0;
    }
    rounded = (bytes + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;
    if (rounded > pool->size - pool->used) {
        return 0;
    }
    *out = pool->base + pool->used;
    pool->used += rounded;
    pool->pages_used = pool->used >> LAPG_PAGE_BITS;
    return 1;
}

uint64_t lapg_pool_pages_remaining(const LapgPagePool *pool)
{
    return pool->pages_total - pool->pages_used;
}

/*
 * PWCL packs DIR1, DIR2 and the PT level; PWCH packs DIR3.  The widths are
 * stored as (width - 1) in hardware, the usual LoongArch convention for
 * these two registers.
 */
uint64_t lapg_pwcl_value(void)
{
    uint64_t v = 0;

    v |= ((uint64_t)LAPG_DIR1_BASE & 0x3f) << 0;
    v |= (((uint64_t)LAPG_DIR1_WIDTH - 1) & 0x3f) << 6;
    v |= ((uint64_t)LAPG_DIR2_BASE & 0x3f) << 12;
    v |= (((uint64_t)LAPG_DIR2_WIDTH - 1) & 0x3f) << 18;
    v |= ((uint64_t)LAPG_PT_BASE & 0x3f) << 24;
    v |= (((uint64_t)LAPG_PT_WIDTH - 1) & 0x3f) << 30;
    v |= ((uint64_t)0 & 0x3) << 36;   /* PTEWIDTH = 0: 64-bit entries */
    return v;
}

uint64_t lapg_pwch_value(void)
{
    uint64_t v = 0;

    v |= ((uint64_t)LAPG_DIR3_BASE & 0x3f) << 0;
    v |= (((uint64_t)LAPG_DIR3_WIDTH - 1) & 0x3f) << 6;
    return v;
}

uint64_t lapg_stlbps_value(void)
{
    return (uint64_t)LAPG_PAGE_BITS;   /* CSR.STLBPS.PS := 12 for 4 KiB */
}

uint64_t lapg_make_leaf_mat(uint64_t ppn, uint32_t perm, uint32_t mat)
{
    uint64_t pte;

    /* PPN occupies bits [47:12]; a page number wider than that would alias. */
    pte = (ppn & ((UINT64_C(1) << 36) - 1)) << LAPG_PTE_PPN_SHIFT;
    pte |= LAPG_PTE_V;

    /*
     * D is the hardware "dirty" bit: a store to an entry with D=0 raises
     * PME, so it must be set for every page the caller marked writable and
     * left clear otherwise -- setting it unconditionally would silently
     * turn read-only pages into writable ones.
     */
    if (perm & LAPG_PERM_WRITE) {
        pte |= LAPG_PTE_D;
    }

    /* NR/NX are inhibit bits: set when the access is *not* allowed. */
    if (!(perm & LAPG_PERM_READ)) {
        pte |= LAPG_PTE_NR;
    }
    if (!(perm & LAPG_PERM_EXEC)) {
        pte |= LAPG_PTE_NX;
    }
    if (perm & LAPG_PERM_USER) {
        pte |= LAPG_PTE_PLV3;
    } else {
        pte |= LAPG_PTE_PLV0;
    }
    if (mat > LAPG_MAT_WUC) {
        mat = LAPG_MAT_CC;
    }
    pte |= ((uint64_t)(mat & 3u)) << LAPG_PTE_MAT_SHIFT;
    if (perm & LAPG_PERM_GLOBAL) {
        pte |= LAPG_PTE_G;
    }
    return pte;
}

uint64_t lapg_make_leaf(uint64_t ppn, uint32_t perm)
{
    return lapg_make_leaf_mat(ppn, perm, LAPG_MAT_CC);
}

uint64_t lapg_make_table(uint64_t next_table_phys)
{
    uint64_t ppn = (next_table_phys & LAPG_PAGE_MASK) >> LAPG_PTE_PPN_SHIFT;

    return (ppn << LAPG_PTE_PPN_SHIFT) | LAPG_PTE_V | LAPG_PTE_D;
}

void lapg_zero_page(void *page)
{
    volatile uint64_t *p = (volatile uint64_t *)page;
    uint64_t i;

    for (i = 0; i < LAPG_PAGE_SIZE / sizeof(uint64_t); i++) {
        p[i] = 0;
    }
}

static int read_phys(LapgMemory *memory, uint64_t phys, uint64_t *value)
{
    return memory->read_u64(memory->opaque, phys, value);
}

static int write_phys(LapgMemory *memory, uint64_t phys, uint64_t value)
{
    if (memory->write_u64 == 0) {
        return 0;
    }
    return memory->write_u64(memory->opaque, phys, value);
}

/* Is this a well-formed entry for the level it was found at? */
static int entry_is_table(uint64_t entry)
{
    /* A non-leaf entry must be present, and its PPN must be page aligned,
     * which is guaranteed by the encoding: the low 12 bits of the PPN field
     * are the bits above the offset. */
    return (entry & LAPG_PTE_V) != 0;
}

LapgResult lapg_map_mat(LapgMemory *memory, LapgPagePool *pool, uint64_t root,
                        uint64_t va, uint64_t pa, uint32_t perm, uint32_t mat)
{
    uint64_t table = root;
    int level;

    if ((root & ~LAPG_PAGE_MASK) != 0 || (pa & ~LAPG_PAGE_MASK) != 0) {
        return LAPG_FAULT_INVALID;
    }

    for (level = 0; level < LAPG_LEVELS - 1; level++) {
        unsigned idx = va_index(va, level);
        uint64_t slot = table + (uint64_t)idx * sizeof(uint64_t);
        uint64_t entry;

        if (!read_phys(memory, slot, &entry)) {
            return LAPG_FAULT_MEMORY;
        }
        if (!entry_is_table(entry)) {
            uint64_t next;

            if (!lapg_pool_alloc(pool, LAPG_PAGE_SIZE, &next)) {
                return LAPG_FAULT_MEMORY;
            }
            /* The pool is plain memory; zeroing goes through the same
             * physical addresses the walk uses. */
            {
                uint64_t i;

                for (i = 0; i < LAPG_INDEX_COUNT; i++) {
                    if (!write_phys(memory, next + i * sizeof(uint64_t), 0)) {
                        return LAPG_FAULT_MEMORY;
                    }
                }
            }
            entry = lapg_make_table(next);
            if (!write_phys(memory, slot, entry)) {
                return LAPG_FAULT_MEMORY;
            }
        }
        /* A stored table entry keeps its PPN in bits [47:12]. */
        table = entry & UINT64_C(0x0000fffffffff000);
    }

    {
        unsigned idx = va_index(va, LAPG_LEVELS - 1);
        uint64_t slot = table + (uint64_t)idx * sizeof(uint64_t);
        uint64_t entry = lapg_make_leaf_mat(pa >> LAPG_PAGE_BITS, perm, mat);

        if (!write_phys(memory, slot, entry)) {
            return LAPG_FAULT_MEMORY;
        }
    }
    return LAPG_OK;
}

LapgResult lapg_map(LapgMemory *memory, LapgPagePool *pool, uint64_t root,
                    uint64_t va, uint64_t pa, uint32_t perm)
{
    return lapg_map_mat(memory, pool, root, va, pa, perm, LAPG_MAT_CC);
}

LapgResult lapg_walk(LapgMemory *memory, uint64_t root, uint64_t va,
                     uint32_t access, LapgTranslation *out)
{
    uint64_t table = root;
    uint64_t entry = 0;
    int level;

    if ((root & ~LAPG_PAGE_MASK) != 0) {
        return LAPG_FAULT_INVALID;
    }

    for (level = 0; level < LAPG_LEVELS; level++) {
        unsigned idx = va_index(va, level);
        uint64_t slot = table + (uint64_t)idx * sizeof(uint64_t);

        if (!read_phys(memory, slot, &entry)) {
            return LAPG_FAULT_MEMORY;
        }
        if (!entry_is_table(entry)) {
            return LAPG_FAULT_UNMAPPED;
        }
        if (level < LAPG_LEVELS - 1) {
            table = entry_ppn(entry);
            continue;
        }
        /* Leaf. */
        if (access & LAPG_PERM_WRITE) {
            /* The D bit was set at map time, so a write never needs a
             * second-chance fault in this model. */
        }
        out->pte = entry;
        out->page_bits = LAPG_PAGE_BITS;
        out->phys = entry_ppn(entry) | (va & (LAPG_PAGE_SIZE - 1));

        /* Derive the granted permissions from the inhibit bits only. */
        out->perm = 0;
        if (!(entry & LAPG_PTE_NR)) {
            out->perm |= LAPG_PERM_READ;
        }
        if (entry & LAPG_PTE_D) {
            out->perm |= LAPG_PERM_WRITE;
        }
        if (!(entry & LAPG_PTE_NX)) {
            out->perm |= LAPG_PERM_EXEC;
        }
        if ((entry & LAPG_PTE_PLV_MASK) == LAPG_PTE_PLV3) {
            out->perm |= LAPG_PERM_USER;
        }

        /*
         * R/W/X are checked unconditionally.  LAPG_PERM_USER in the request
         * means "this access originates at PLV3", which then also requires
         * a PLV3 page; a request without it is a supervisor access and may
         * reach a user page (the usual x86 supervisor bypass).
         */
        if (access & ~out->perm & 0x7) {
            return LAPG_FAULT_PROTECTION;
        }
        if ((access & LAPG_PERM_USER) && !(out->perm & LAPG_PERM_USER)) {
            return LAPG_FAULT_PROTECTION;
        }
        return LAPG_OK;
    }
    return LAPG_FAULT_UNMAPPED;
}
