/*
 * x86-64 guest page-table walker.  See latx-x86-paging.h for the contract.
 *
 * Every reserved-bit and permission rule below is the one the manual states
 * for 4-level paging; the point of writing them out is that a malformed table
 * must be reported as #PF(RSVD) rather than silently producing an address,
 * because a shadow-paging layer that trusts a bad entry installs a mapping the
 * guest never asked for.
 */
#include "qemu/osdep.h"
#include "cpu.h"

#include "latx-x86-paging.h"

/* PTE bits we care about (Intel SDM 4.5). */
#define PTE_P       (UINT64_C(1) << 0)
#define PTE_RW      (UINT64_C(1) << 1)
#define PTE_US      (UINT64_C(1) << 2)
#define PTE_PS      (UINT64_C(1) << 7)
#define PTE_NX      (UINT64_C(1) << 63)

/* Bits 51:12 of an entry hold the next table or the page frame. */
#define PTE_PHYS_MASK UINT64_C(0x000ffffffffff000)

__attribute__((weak)) uint64_t latx_platform_phys_read(uint64_t pa, int size)
{
    switch (size) {
    case 1:
        return *(const volatile uint8_t *)(uintptr_t)pa;
    case 2:
        return *(const volatile uint16_t *)(uintptr_t)pa;
    case 4:
        return *(const volatile uint32_t *)(uintptr_t)pa;
    default:
        return *(const volatile uint64_t *)(uintptr_t)pa;
    }
}

static uint64_t pte_lookup(uint64_t table_phys, unsigned index)
{
    return latx_platform_phys_read(table_phys + (uint64_t)index * 8, 8);
}

/*
 * Bits that must be zero in a large-page leaf: the hardware ignores page
 * offset below the leaf size, so anything between it and bit 12 is reserved.
 *   1 GiB leaf keeps 51:30, so 29:13 must be clear.
 *   2 MiB leaf keeps 51:21, so 20:13 must be clear.
 */
static bool leaf_is_reserved(uint64_t pte, int level)
{
    switch (level) {
    case 3:
        return (pte & UINT64_C(0x3fffe000)) != 0;
    case 2:
        return (pte & UINT64_C(0x1fe000)) != 0;
    default:
        return false;
    }
}

LatxX86WalkResult latx_x86_walk(const CPUX86State *env, uint64_t va,
                                uint32_t access, LatxX86Translation *out)
{
    bool user = (access & LATX_X86_ACCESS_USER) != 0;
    bool write = (access & LATX_X86_ACCESS_WRITE) != 0;
    bool exec = (access & LATX_X86_ACCESS_EXEC) != 0;
    bool nx_enabled = (env->efer & MSR_EFER_NXE) != 0;
    uint64_t table, pte;
    uint32_t perm;
    int level;

    memset(out, 0, sizeof(*out));
    out->gpa = va;
    out->page_bits = 12;

    if (!(env->cr[0] & CR0_PG_MASK)) {
        out->perm = access & (LATX_X86_ACCESS_READ | LATX_X86_ACCESS_WRITE |
                              LATX_X86_ACCESS_EXEC);
        return LATX_X86_WALK_PAGING_OFF;
    }
    /* Long mode always uses PAE paging; LA57 changes the index widths. */
    if (!(env->cr[4] & CR4_PAE_MASK)) {
        return LATX_X86_WALK_UNSUPPORTED;
    }
    if (env->cr[4] & CR4_LA57_MASK) {
        return LATX_X86_WALK_RESERVED;
    }
    if (env->cr[3] & ~PTE_PHYS_MASK) {
        return LATX_X86_WALK_RESERVED;
    }

    table = env->cr[3] & PTE_PHYS_MASK;
    for (level = 4; level >= 1; level--) {
        unsigned shift = 12 + 9 * (level - 1);
        unsigned index = (unsigned)((va >> shift) & 0x1ff);

        pte = pte_lookup(table, index);
        if (!(pte & PTE_P)) {
            return LATX_X86_WALK_NOT_PRESENT;
        }
        if (!nx_enabled && (pte & PTE_NX)) {
            return LATX_X86_WALK_RESERVED;
        }
        if (pte & PTE_PS) {
            /* PS is legal only at the 1 GiB (level 3) and 2 MiB (level 2)
             * leaves; at the PML4 and PT levels the bit is reserved. */
            if (level == 4 || level == 1) {
                return LATX_X86_WALK_RESERVED;
            }
            if (leaf_is_reserved(pte, level)) {
                return LATX_X86_WALK_RESERVED;
            }
            break;
        }
        if (level == 1) {
            break;              /* 4 KiB leaf */
        }
        table = pte & PTE_PHYS_MASK;
    }

    perm = LATX_X86_ACCESS_READ;
    out->page_bits = 12 + 9 * (uint32_t)(level - 1);
    out->gpa = (pte & PTE_PHYS_MASK & ~((UINT64_C(1) << out->page_bits) - 1)) |
               (va & ((UINT64_C(1) << out->page_bits) - 1));
    if (pte & PTE_RW) {
        perm |= LATX_X86_ACCESS_WRITE;
    }
    if (pte & PTE_US) {
        perm |= LATX_X86_ACCESS_USER;
    }
    if (!(pte & PTE_NX)) {
        perm |= LATX_X86_ACCESS_EXEC;
    }
    out->pte = pte;
    out->perm = perm;

    if (user && !(pte & PTE_US)) {
        return LATX_X86_WALK_PROTECTION;
    }
    if (write && !(pte & PTE_RW)) {
        /*
         * CR0.WP decides whether a supervisor write to a read-only page
         * faults; a user write always does.  A kernel that clears WP is
         * asking to write through page protection, e.g. to patch its text.
         */
        if (user || (env->cr[0] & CR0_WP_MASK)) {
            return LATX_X86_WALK_PROTECTION;
        }
    }
    if (exec && (pte & PTE_NX)) {
        return LATX_X86_WALK_PROTECTION;
    }
    return LATX_X86_WALK_OK;
}

uint32_t latx_x86_fault_error_code(const CPUX86State *env,
                                   LatxX86WalkResult result, uint32_t access)
{
    uint32_t ec = 0;

    /* bit 0: 0 = not present, 1 = protection violation */
    if (result == LATX_X86_WALK_PROTECTION) {
        ec |= 1u;
    } else if (result == LATX_X86_WALK_RESERVED) {
        ec |= 1u | 8u;
    }
    /* bit 1: the access was a write */
    if (access & LATX_X86_ACCESS_WRITE) {
        ec |= 2u;
    }
    /* bit 2: the access came from CPL 3 */
    if (access & LATX_X86_ACCESS_USER) {
        ec |= 4u;
    }
    /* bit 4: instruction fetch, only reported when NX is enabled */
    if ((access & LATX_X86_ACCESS_EXEC) && (env->efer & MSR_EFER_NXE)) {
        ec |= 16u;
    }
    return ec;
}
