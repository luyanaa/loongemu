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
#include "exec/exec-all.h"
#include "cpu.h"

#include "latx-x86-paging.h"
#include "latx_bare.h"

/* PTE bits we care about (Intel SDM 4.5). */
#define PTE_P       (UINT64_C(1) << 0)
#define PTE_RW      (UINT64_C(1) << 1)
#define PTE_US      (UINT64_C(1) << 2)
#define PTE_PS      (UINT64_C(1) << 7)
#define PTE_NX      (UINT64_C(1) << 63)

/* Bits 51:12 of an entry hold the next table or the page frame. */
#define PTE_PHYS_MASK UINT64_C(0x000ffffffffff000)
#define LAPG_SIZE      UINT64_C(4096)

__attribute__((weak)) void latx_platform_phys_write(uint64_t pa, int size,
                                                   uint64_t value)
{
    switch (size) {
    case 1:
        *(volatile uint8_t *)(uintptr_t)pa = (uint8_t)value;
        break;
    case 2:
        *(volatile uint16_t *)(uintptr_t)pa = (uint16_t)value;
        break;
    case 4:
        *(volatile uint32_t *)(uintptr_t)pa = (uint32_t)value;
        break;
    default:
        *(volatile uint64_t *)(uintptr_t)pa = value;
        break;
    }
}

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

/* ---------------- cached walks ---------------- */

typedef struct LatxX86TlbEntry {
    uint64_t cr3;
    uint64_t tag;        /* va >> page_bits */
    uint64_t gpa_base;   /* leaf base */
    uint32_t page_bits;
    uint32_t perm;
    bool valid;
} LatxX86TlbEntry;

static LatxX86TlbEntry latx_x86_tlb[LATX_X86_TLB_ENTRIES];

void latx_x86_tlb_flush_all(void)
{
    memset(latx_x86_tlb, 0, sizeof(latx_x86_tlb));
}

void latx_x86_tlb_flush_page(uint64_t va)
{
    for (unsigned i = 0; i < LATX_X86_TLB_ENTRIES; i++) {
        LatxX86TlbEntry *e = &latx_x86_tlb[i];

        /*
         * An entry answers every address inside its leaf, so a flush for any
         * address in that leaf invalidates it.  Entries for smaller pages are
         * separate slots and are matched by their own page size.
         */
        if (e->valid && (va >> e->page_bits) == e->tag) {
            e->valid = false;
        }
    }
}

LatxX86WalkResult latx_x86_walk_cached(const CPUX86State *env, uint64_t va,
                                       uint32_t access,
                                       LatxX86Translation *out)
{
    uint64_t cr3 = env->cr[3] & PTE_PHYS_MASK;
    LatxX86TlbEntry *e = &latx_x86_tlb[(va >> 12) & (LATX_X86_TLB_ENTRIES - 1)];
    LatxX86WalkResult r;

    /* Paging off, or no long-mode paging at all: no cache to consult. */
    if (!(env->cr[0] & CR0_PG_MASK) || !(env->cr[4] & CR4_PAE_MASK)) {
        return latx_x86_walk(env, va, access, out);
    }

    if (e->valid && e->cr3 == cr3 && (va >> e->page_bits) == e->tag) {
        memset(out, 0, sizeof(*out));
        out->page_bits = e->page_bits;
        out->gpa = e->gpa_base | (va & ((UINT64_C(1) << e->page_bits) - 1));
        out->perm = e->perm;
        out->pte = 0;            /* not kept: the cache may outlive an A/D bump */

        if ((access & LATX_X86_ACCESS_USER) && !(e->perm & LATX_X86_ACCESS_USER)) {
            return LATX_X86_WALK_PROTECTION;
        }
        if ((access & LATX_X86_ACCESS_WRITE) &&
            !(e->perm & LATX_X86_ACCESS_WRITE)) {
            bool user = (access & LATX_X86_ACCESS_USER) != 0;

            if (user || (env->cr[0] & CR0_WP_MASK)) {
                return LATX_X86_WALK_PROTECTION;
            }
        }
        if ((access & LATX_X86_ACCESS_EXEC) &&
            !(e->perm & LATX_X86_ACCESS_EXEC)) {
            return LATX_X86_WALK_PROTECTION;
        }
        return LATX_X86_WALK_OK;
    }

    r = latx_x86_walk(env, va, access, out);
    if (r != LATX_X86_WALK_OK && r != LATX_X86_WALK_PROTECTION) {
        /*
         * A not-present or malformed entry is never cached: the guest may be
         * about to fix it, and a negative entry would then need its own
         * invalidation protocol.
         */
        return r;
    }
    e->cr3 = cr3;
    e->page_bits = out->page_bits;
    e->tag = va >> out->page_bits;
    e->gpa_base = out->gpa & ~((UINT64_C(1) << out->page_bits) - 1);
    e->perm = out->perm;
    e->valid = true;
    return r;
}

uint64_t latx_x86_access_hpa;
/* latx_x86_checked_operand lives in mem-interface.c: it is translation-time. */

uint64_t latx_x86_check_guest_access(CPUX86State *env, uint64_t gva,
                                     uint32_t access, uint32_t size_bits,
                                     uint64_t insn_pc)
{
    uint64_t bytes = size_bits ? (size_bits + 7) / 8 : 1;
    uint64_t page = gva & ~(LAPG_SIZE - 1);
    uint64_t hpa = 0;
    uint64_t delta = 0;
    bool have_delta = false;

    (void)insn_pc;

    for (;;) {
        LatxX86Translation t;
        LatxX86WalkResult r = latx_x86_walk_cached(env, page, access, &t);

        if (r != LATX_X86_WALK_OK && r != LATX_X86_WALK_PAGING_OFF) {
            /*
             * Whatever produced this refusal must not be reused: a guest
             * handler exists precisely because the mapping is about to
             * change, and a cached leaf would make the retry fault forever.
             */
            latx_x86_tlb_flush_page(gva);
            env->cr[2] = gva;
            env->exception_index = EXCP0E_PAGE;
            env->error_code = (int)latx_x86_fault_error_code(env, r, access);
            return LATX_X86_ACCESS_FAULTED;
        }
        {
            uint64_t d = t.gpa - page;

            if (!have_delta) {
                delta = d;
                have_delta = true;
                hpa = gva + delta;
            } else if (d != delta) {
                /*
                 * The access spans pages with different translations.  The
                 * emitted code can only carry one host address, so refuse
                 * rather than write half of it to the wrong place.
                 */
                env->cr[2] = gva;
                env->exception_index = EXCP0E_PAGE;
                env->error_code = 1 | 8;        /* protection, RSVD */
                return LATX_X86_ACCESS_FAULTED;
            }
        }
        if (page + LAPG_SIZE >= gva + bytes) {
            break;
        }
        page += LAPG_SIZE;
    }

    return hpa;
}

int latx_x86_deliver_fault(CPUX86State *env, uint32_t vector)
{
    uint64_t gate_phys = env->idt.base + (uint64_t)vector * 16;
    uint64_t lo, hi, off, selector, rsp;

    if ((uint64_t)vector * 16 + 15 > env->idt.limit + 1) {
        return 1;                       /* no such gate */
    }
    lo = latx_platform_phys_read(gate_phys, 8);
    hi = latx_platform_phys_read(gate_phys + 8, 8);

    {
        uint8_t type_attr = (uint8_t)(lo >> 40);

        /* Present, and an interrupt (0xE) or trap (0xF) gate. */
        if (!(type_attr & 0x80) || (type_attr & 0x1f) < 0x0e) {
            return 1;
        }
    }
    selector = (lo >> 16) & 0xffff;
    off = (lo & 0xffff) | (hi & 0xffff0000u) | (hi << 32);

    rsp = env->regs[R_ESP];
    /*
     * Long-mode frame, top first: error code, RIP, CS, RFLAGS, RSP, SS.
     * RSP and SS belong to the privilege-change form; they are pushed
     * unconditionally because the translator's IRETQ pops them regardless,
     * and SS selector 0 is what long mode uses.
     */
#define PUSH64(v)                                                             \
    do {                                                                      \
        latx_platform_phys_write(rsp - 8, 8, (uint64_t)(v));                   \
        rsp -= 8;                                                             \
    } while (0)

    PUSH64(env->segs[R_SS].selector);
    PUSH64(env->regs[R_ESP]);
    PUSH64(env->eflags & ~(UINT64_C(1) << 8));   /* TF clear in the handler */
    PUSH64(env->segs[R_CS].selector);
    PUSH64(env->eip);
    PUSH64((uint64_t)(uint32_t)env->error_code);
#undef PUSH64

    /*
     * A handler runs because a mapping or a permission is about to change,
     * and nothing here can know which, so every cached walk is suspect.
     */
    latx_x86_tlb_flush_all();

    env->regs[R_ESP] = rsp;
    env->eip = off;
    env->segs[R_CS].selector = (uint32_t)selector;
    env->segs[R_CS].base = 0;                    /* ignored in long mode */
    env->exception_index = 0;                    /* the guest now owns it */
    return 0;
}

uint8_t latx_x86_fetch_byte(const CPUX86State *env, uint64_t gva)
{
    LatxX86Translation t;
    LatxX86WalkResult r;

    /*
     * Paging is only real once the guest has loaded a PML4.  A harness that
     * sets CR0.PG with CR3 == 0 has no tables at all and means the flat layout,
     * which is also how the translator treated it before fetch went through
     * the walker.
     */
    if (!(env->cr[0] & CR0_PG_MASK) || !(env->cr[4] & CR4_PAE_MASK) ||
        env->cr[3] == 0) {
        return latx_bare_ldub_code(gva);
    }

    r = latx_x86_walk_cached(env, gva, LATX_X86_ACCESS_EXEC, &t);
    if (r != LATX_X86_WALK_OK) {
        CPUX86State *mutable = (CPUX86State *)(uintptr_t)env;

        mutable->cr[2] = gva;
        mutable->exception_index = EXCP0E_PAGE;
        mutable->error_code = (int)latx_x86_fault_error_code(env, r,
                                                            LATX_X86_ACCESS_EXEC);
        return 0xcc;
    }
    return (uint8_t)latx_platform_phys_read(t.gpa, 1);
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
