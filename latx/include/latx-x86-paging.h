/*
 * x86-64 guest page-table walker.
 *
 * The translator's memory path is direct mapped: it hands the guest virtual
 * address to the host as an address.  That is fine for a bring-up harness,
 * but it means the tables the guest builds are never consulted -- a guest
 * page marked read-only is still writable, and an unmapped page is still
 * reachable.  This walker is the read side of fixing that: it turns a guest
 * virtual address into a guest physical address through the guest's own
 * four-level tables, and reports the leaf permissions and the fault the
 * hardware would raise.
 *
 * It is deliberately free of policy: nothing here installs a mapping, touches
 * a TLB or injects an exception.  A caller that wants shadow paging uses the
 * translation to build its own entry, and a caller that wants to deliver a
 * #PF uses latx_x86_fault_error_code() for the stack slot.
 *
 * Table memory is read through latx_platform_phys_read(), so a board can put
 * guest RAM anywhere; the identity harness maps it one-to-one.
 */
#ifndef LATX_X86_PAGING_H
#define LATX_X86_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Access being attempted; the bits mirror the #PF error code for W/R and I/D. */
#define LATX_X86_ACCESS_READ   (1u << 0)
#define LATX_X86_ACCESS_WRITE  (1u << 1)
#define LATX_X86_ACCESS_EXEC   (1u << 2)
#define LATX_X86_ACCESS_USER   (1u << 3)

typedef enum LatxX86WalkResult {
    LATX_X86_WALK_OK = 0,
    LATX_X86_WALK_NOT_PRESENT,   /* entry not present */
    LATX_X86_WALK_PROTECTION,    /* present, but this access is not allowed */
    LATX_X86_WALK_RESERVED,      /* malformed entry (PS at the wrong level, NX
                                  * without EFER.NXE, LA57) */
    LATX_X86_WALK_UNSUPPORTED,   /* no long-mode paging configured */
    LATX_X86_WALK_PAGING_OFF,    /* CR0.PG clear: gpa == va */
} LatxX86WalkResult;

typedef struct LatxX86Translation {
    uint64_t gpa;        /* guest physical address of the access */
    uint64_t pte;        /* the leaf entry that produced it */
    uint32_t page_bits;  /* 12, 21 or 30 */
    uint32_t perm;       /* the same bits as the access mask */
} LatxX86Translation;

struct CPUX86State;

/*
 * Walk one address.  `access` is a mask of LATX_X86_ACCESS_*; USER selects
 * the ring-3 rules (US bit required), otherwise the supervisor rules apply
 * and CR0.WP decides whether a write to a read-only page faults.
 */
LatxX86WalkResult latx_x86_walk(const struct CPUX86State *env, uint64_t va,
                                uint32_t access, LatxX86Translation *out);

/* The value that belongs in the #PF error-code slot for this walk result. */
uint32_t latx_x86_fault_error_code(const struct CPUX86State *env,
                                   LatxX86WalkResult result, uint32_t access);

/*
 * Cached walks.
 *
 * A page-granular cache keyed by (cr3, leaf index) is what makes a per-access
 * check affordable.  It is direct mapped and small: a miss simply walks, and
 * correctness does not depend on the cache at all, only on the flushes below
 * being called.  A large leaf is stored with its own size, so an entry for a
 * 2 MiB page answers every address inside it.
 */
#define LATX_X86_TLB_ENTRIES 64

void latx_x86_tlb_flush_all(void);
void latx_x86_tlb_flush_page(uint64_t va);
LatxX86WalkResult latx_x86_walk_cached(const struct CPUX86State *env,
                                       uint64_t va, uint32_t access,
                                       LatxX86Translation *out);

/*
 * Physical-memory read used for the tables themselves.  Weak default reads
 * the identity mapping; a board overrides it when guest RAM lives elsewhere.
 */
uint64_t latx_platform_phys_read(uint64_t pa, int size);

#ifdef __cplusplus
}
#endif

#endif /* LATX_X86_PAGING_H */
