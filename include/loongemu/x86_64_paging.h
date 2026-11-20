#ifndef LOONGEMU_X86_64_PAGING_H
#define LOONGEMU_X86_64_PAGING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X86_CR0_WP       (UINT64_C(1) << 16)
#define X86_CR0_PG       (UINT64_C(1) << 31)
#define X86_CR4_PSE      (UINT64_C(1) << 4)
#define X86_CR4_PAE      (UINT64_C(1) << 5)
#define X86_CR4_PGE      (UINT64_C(1) << 7)
#define X86_CR4_LA57     (UINT64_C(1) << 12)
#define X86_CR4_PCIDE    (UINT64_C(1) << 17)
#define X86_CR4_SMEP     (UINT64_C(1) << 20)
#define X86_CR4_SMAP     (UINT64_C(1) << 21)
#define X86_CR4_PKE      (UINT64_C(1) << 22)
#define X86_CR4_PKS      (UINT64_C(1) << 24)
#define X86_EFER_LMA     (UINT64_C(1) << 10)
#define X86_EFER_NXE     (UINT64_C(1) << 11)
#define X86_RFLAGS_AC    (UINT64_C(1) << 18)

#define X86_PF_PRESENT   (UINT32_C(1) << 0)
#define X86_PF_WRITE     (UINT32_C(1) << 1)
#define X86_PF_USER      (UINT32_C(1) << 2)
#define X86_PF_RESERVED  (UINT32_C(1) << 3)
#define X86_PF_INSN      (UINT32_C(1) << 4)
#define X86_PF_PROTECTION_KEY (UINT32_C(1) << 5)

#define X86_PAGING_FEATURE_1G_PAGES (UINT32_C(1) << 0)

typedef enum X86PagingAccess {
    X86_PAGING_READ,
    X86_PAGING_WRITE,
    X86_PAGING_EXECUTE,
} X86PagingAccess;

typedef enum X86PagingResult {
    X86_PAGING_OK,
    X86_PAGING_PAGE_FAULT,
    X86_PAGING_GENERAL_PROTECTION,
    X86_PAGING_UNSUPPORTED,
    X86_PAGING_INVALID_STATE,
    X86_PAGING_MEMORY_ERROR,
} X86PagingResult;

typedef struct X86PagingState {
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;
    uint64_t efer;
    uint64_t rflags;
    uint32_t pkru;
    uint32_t pkrs;
    uint32_t features;
    uint8_t maxphyaddr;
    uint8_t cpl;
} X86PagingState;

typedef struct X86PagingMemory {
    void *opaque;
    /* Read one aligned guest-physical 64-bit paging entry. */
    int (*read_u64)(void *opaque, uint64_t guest_physical_address,
                    uint64_t *value);
    /* Atomically OR bits into one aligned entry and return its new value. */
    int (*atomic_or_u64)(void *opaque, uint64_t guest_physical_address,
                         uint64_t bits, uint64_t *new_value);
} X86PagingMemory;

typedef struct X86PageFault {
    uint64_t linear_address;
    uint32_t error_code;
} X86PageFault;

typedef struct X86Translation {
    uint64_t guest_physical_address;
    uint64_t page_size;
    uint64_t leaf_pte;
    uint8_t writable;
    uint8_t user_accessible;
    uint8_t executable;
    uint8_t global;
    uint8_t pkey;
    uint8_t dirty;
} X86Translation;

/*
 * Walk a four-level x86-64 guest page table without allocating memory.
 *
 * The caller owns guest-physical memory, CR3/PCID state, TLBs, and TB
 * invalidation. The callbacks must reject unmapped or misaligned physical
 * addresses. atomic_or_u64 must be atomic with respect to guest writes and
 * other page walks; it implements hardware-like Accessed/Dirty-bit updates.
 * The walker validates reserved entry bits, aggregates R/W, U/S, and NX
 * permissions across every level, and enforces protection keys when enabled.
 * Five-level paging (CR4.LA57) and non-canonical linear addresses are not
 * silently truncated: they return UNSUPPORTED and GENERAL_PROTECTION,
 * respectively.
 * PAGE_FAULT fills fault when non-NULL. All other errors leave the output
 * translation unspecified. This routine is a page-table walker, not a
 * complete x86 MMU: the caller must still handle TLB caching, global pages,
 * INVLPG, PCID, cross-vCPU shootdowns, and instruction/data emulation.
 */
X86PagingResult x86_64_walk_page_tables(
    const X86PagingState *state,
    const X86PagingMemory *memory,
    uint64_t linear_address,
    X86PagingAccess access,
    X86Translation *translation,
    X86PageFault *fault);

#ifdef __cplusplus
}
#endif

#endif
