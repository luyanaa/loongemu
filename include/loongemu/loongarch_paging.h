/*
 * LoongArch 4 KiB hardware page tables.
 *
 * The thin VMM runs in PLV0 with paging enabled, so every address it touches
 * -- its own code, the guest RAM it copies into, the code cache it emits --
 * has to be reachable through the TLB.  These helpers build and walk that
 * table in software.
 *
 * The layout mirrors x86-64 on purpose: a 4 KiB granule and four levels of
 * nine bits give the same PML4 -> PDPT -> PD -> PT shape, so a guest virtual
 * address maps to a host virtual address with the same index arithmetic and
 * a guest translation can be turned into a host TLB entry without reshaping
 * the address.
 *
 *   VA[47:39] -> level 1 / Dir3
 *   VA[38:30] -> level 2 / Dir2
 *   VA[29:21] -> level 3 / Dir1
 *   VA[20:12] -> level 4 / PT (page)
 *
 * CSR.PWCL/PWCH describe this to the hardware (LoongArch Vol 1, bottom-up):
 *
 *   PWCL.PTbase    = 12, PTwidth   = 9
 *   PWCL.Dir1_base = 21, Dir1_width = 9
 *   PWCL.Dir2_base = 30, Dir2_width = 9
 *   PWCL.PTEWidth  = 0  (64-bit entries)
 *   PWCH.Dir3_base = 39, Dir3_width = 9
 *   CSR.STLBPS.PS  = 12 (4 KiB)
 */

#ifndef LOONGEMU_LOONGARCH_PAGING_H
#define LOONGEMU_LOONGARCH_PAGING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAPG_PAGE_BITS   12
#define LAPG_PAGE_SIZE   (UINT64_C(1) << LAPG_PAGE_BITS)
#define LAPG_PAGE_MASK   (~(LAPG_PAGE_SIZE - 1))
#define LAPG_INDEX_BITS  9
#define LAPG_INDEX_COUNT (1 << LAPG_INDEX_BITS)
#define LAPG_LEVELS      4

#define LAPG_PS_4K       12
#define LAPG_PS_1M       20
#define LAPG_PS_1G       30

/* The four VA ranges the hardware walks, as (base, width) pairs (bottom-up). */
#define LAPG_PT_BASE     12
#define LAPG_PT_WIDTH    9
#define LAPG_DIR1_BASE   21
#define LAPG_DIR1_WIDTH  9
#define LAPG_DIR2_BASE   30
#define LAPG_DIR2_WIDTH  9
#define LAPG_DIR3_BASE   39
#define LAPG_DIR3_WIDTH  9

/* Hardware PTE bits (LoongArch manual, TLB entry format). */
#define LAPG_PTE_V       (UINT64_C(1) << 0)
#define LAPG_PTE_D       (UINT64_C(1) << 1)
#define LAPG_PTE_PLV_SHIFT 2
#define LAPG_PTE_PLV_MASK  (UINT64_C(3) << LAPG_PTE_PLV_SHIFT)
#define LAPG_PTE_PLV0    (UINT64_C(0) << LAPG_PTE_PLV_SHIFT)
#define LAPG_PTE_PLV1    (UINT64_C(1) << LAPG_PTE_PLV_SHIFT)
#define LAPG_PTE_PLV3    (UINT64_C(3) << LAPG_PTE_PLV_SHIFT)
#define LAPG_PTE_MAT_SHIFT 4
#define LAPG_PTE_MAT_MASK  (UINT64_C(3) << LAPG_PTE_MAT_SHIFT)
#define LAPG_MAT_SUC       0u
#define LAPG_MAT_CC        1u
#define LAPG_MAT_WUC       2u
#define LAPG_PTE_MAT     (UINT64_C(0) << 4)
#define LAPG_PTE_G       (UINT64_C(1) << 6)
#define LAPG_PTE_PPN_SHIFT 12
#define LAPG_PTE_NR      (UINT64_C(1) << 61)
#define LAPG_PTE_NX      (UINT64_C(1) << 62)
#define LAPG_PTE_RPLV    (UINT64_C(1) << 63)

/* Permission bits the caller passes in; they are converted to PTE bits. */
#define LAPG_PERM_READ   (1u << 0)
#define LAPG_PERM_WRITE  (1u << 1)
#define LAPG_PERM_EXEC   (1u << 2)
#define LAPG_PERM_USER   (1u << 3)
#define LAPG_PERM_GLOBAL (1u << 4)

typedef enum LapgResult {
    LAPG_OK = 0,
    LAPG_FAULT_INVALID,     /* malformed entry: misaligned PPN or bad level */
    LAPG_FAULT_UNMAPPED,    /* entry not present */
    LAPG_FAULT_PROTECTION,  /* present but the access is not permitted */
    LAPG_FAULT_MEMORY,      /* the memory callback failed */
} LapgResult;

/*
 * Physical memory access.  Everything is in host physical addresses; the
 * pool below is the only place that hands them out.
 */
typedef struct LapgMemory {
    void *opaque;
    int (*read_u64)(void *opaque, uint64_t phys, uint64_t *value);
    /* Write-back of a hardware-updated A/D bit; may be a plain store. */
    int (*write_u64)(void *opaque, uint64_t phys, uint64_t value);
} LapgMemory;

/* A bump allocator over one contiguous page-aligned pool. */
typedef struct LapgPagePool {
    uint64_t base;          /* first page, page aligned */
    uint64_t size;          /* bytes, a multiple of LAPG_PAGE_SIZE */
    uint64_t used;          /* bytes handed out */
    uint64_t pages_total;
    uint64_t pages_used;
} LapgPagePool;

/* One translation: host physical address plus the PTE that produced it. */
typedef struct LapgTranslation {
    uint64_t phys;
    uint64_t pte;
    uint32_t page_bits;
    uint32_t perm;
} LapgTranslation;

void lapg_pool_init(LapgPagePool *pool, uint64_t base, uint64_t size);

/* Returns 0 when the pool is exhausted or the request is not page aligned. */
int lapg_pool_alloc(LapgPagePool *pool, uint64_t bytes, uint64_t *out);
uint64_t lapg_pool_pages_remaining(const LapgPagePool *pool);

/*
 * Build the two CSR values that describe the table.  Kept as plain functions
 * so the encoding can be checked without a CPU.
 */
uint64_t lapg_pwcl_value(void);
uint64_t lapg_pwch_value(void);
uint64_t lapg_stlbps_value(void);

/* Encode a leaf entry.  ppn is a physical page number, not an address. */
uint64_t lapg_make_leaf(uint64_t ppn, uint32_t perm);
uint64_t lapg_make_leaf_mat(uint64_t ppn, uint32_t perm, uint32_t mat);

/* Encode a non-leaf entry pointing at the next level. */
uint64_t lapg_make_table(uint64_t next_table_phys);

/* Zero a freshly allocated page so a new table starts empty. */
void lapg_zero_page(void *page);

/*
 * Map one 4 KiB page.  root is the physical address of the level-1 table;
 * memory must be able to read and write the pool the tables live in.
 * Intermediate tables are allocated from pool as needed.
 */
LapgResult lapg_map(LapgMemory *memory, LapgPagePool *pool, uint64_t root,
                    uint64_t va, uint64_t pa, uint32_t perm);
LapgResult lapg_map_mat(LapgMemory *memory, LapgPagePool *pool, uint64_t root,
                        uint64_t va, uint64_t pa, uint32_t perm, uint32_t mat);

/*
 * Walk a virtual address.  On LAPG_OK the translation holds the host
 * physical address and the leaf PTE.  This is the routine the exception
 * handler calls before filling the TLB.
 */
LapgResult lapg_walk(LapgMemory *memory, uint64_t root, uint64_t va,
                     uint32_t access, LapgTranslation *out);

#ifdef __cplusplus
}
#endif

#endif
