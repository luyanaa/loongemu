/*
 * ARMv8-A Stage-1 page-table walker (4 KiB granule, 48-bit VA).
 *
 * Same role as the x86-64 walker in latx-x86-paging.h: turn a guest virtual
 * address into a guest physical address through the guest's own tables, and
 * report the leaf permissions and the fault the hardware would raise.  The
 * translator's memory path is direct mapped, so without this a guest page
 * marked read-only is still writable and an unmapped page is still reachable.
 *
 * Nothing here installs mappings, touches a TLB, or injects an exception: a
 * caller that wants shadow paging uses the translation to build its own entry,
 * and one that wants to deliver a fault uses the result directly.
 *
 * Table memory is read through latx_platform_phys_read(), so a board can put
 * guest RAM anywhere.
 */
#ifndef LOONGEMU_ARM64_PAGING_H
#define LOONGEMU_ARM64_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Access being attempted; the bits mirror the ESR fault status for W/R. */
#define ARM64_ACCESS_READ  (1u << 0)
#define ARM64_ACCESS_WRITE (1u << 1)
#define ARM64_ACCESS_EXEC  (1u << 2)
#define ARM64_ACCESS_USER  (1u << 3)   /* EL0 */

typedef enum Arm64WalkResult {
    ARM64_WALK_OK = 0,
    ARM64_WALK_NOT_PRESENT,   /* invalid descriptor: translation fault */
    ARM64_WALK_PROTECTION,    /* valid, but AP/XN/PXN deny this access */
    ARM64_WALK_RESERVED,      /* malformed: bad block/page size or attribute */
    ARM64_WALK_UNSUPPORTED,   /* MMU off, or a granule/TBI this walker lacks */
} Arm64WalkResult;

/*
 * Guest-physical memory access.  A callback, not a direct call into the board:
 * that is what makes the walker testable on the host, and it keeps the board's
 * choice of RAM placement out of here.
 */
typedef struct Arm64PagingMemory {
    void *opaque;
    /* Read one aligned guest-physical 64-bit descriptor. */
    int (*read_u64)(void *opaque, uint64_t guest_physical_address,
                    uint64_t *value);
} Arm64PagingMemory;

typedef struct Arm64Translation {
    uint64_t pa;         /* guest physical address of the access */
    uint64_t desc;       /* the leaf descriptor that produced it */
    uint32_t page_bits;  /* 12, 21 or 30 */
    uint32_t perm;       /* the same bits as the access mask */
} Arm64Translation;

/*
 * Walk one address.
 *
 * `ttbr` is the base register for the half of the address space the VA lives
 * in (TTBR0_EL1 below 2^48/2, TTBR1_EL1 above), selected exactly as the
 * hardware does: bit 55 of the VA.  `tcr` supplies T0SZ/T1SZ, TG0/TG1 and the
 * EPD disable bits.
 *
 * Returns ARM64_WALK_UNSUPPORTED when the configuration is one this walker
 * does not model (MMU disabled, 16 KiB/64 KiB granule, or a disabled table
 * walk for this half); every other result names a real outcome.
 */
Arm64WalkResult arm64_walk(const Arm64PagingMemory *memory,
                           uint64_t ttbr0, uint64_t ttbr1, uint64_t tcr,
                           uint64_t va, uint32_t access,
                           Arm64Translation *out);

/* The ESR_EL1 fault status class / ISS this walk would raise, or 0 for OK. */
uint32_t arm64_fault_status(Arm64WalkResult result, uint32_t access);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_ARM64_PAGING_H */
