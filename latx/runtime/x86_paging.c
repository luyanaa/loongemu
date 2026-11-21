/*
 * LATX runtime x86-64 MMU adapter & software TLB cache.
 *
 * Single authoritative walker architecture:
 *   All x86-64 page-table walks, A/D-bit updates, reserved-bit validation,
 *   SMEP/SMAP/PKU/NX/WP permission checks, and #PF error-code generation are
 *   delegated to `x86_64_walk_page_tables()` in `src/x86_64_paging.c`.
 *   This file adapts `CPUX86State`, maintains the fast software TLB cache
 *   (honoring CR3/PCID, Global pages, Dirty-bit promotion, SMEP/SMAP/PKU),
 *   and implements fault/syscall delivery.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"

#include "loongemu/x86_64_paging.h"
#include "latx-x86-paging.h"
#include "latx_bare.h"

#define PTE_PHYS_MASK UINT64_C(0x000000fffffff000) /* MAXPHYADDR = 40 */
#define LAPG_SIZE     UINT64_C(4096)

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

__attribute__((weak)) int latx_platform_phys_read_u64_checked(uint64_t pa,
                                                              uint64_t *value)
{
    if ((pa & 7u) != 0) {
        return -1;
    }
    *value = latx_platform_phys_read(pa, 8);
    return 0;
}

__attribute__((weak)) int latx_platform_phys_atomic_or_u64(uint64_t pa,
                                                           uint64_t bits,
                                                           uint64_t *new_value)
{
    uint64_t cur;

    if ((pa & 7u) != 0) {
        return -1;
    }
    cur = latx_platform_phys_read(pa, 8);
    if ((cur & bits) != bits) {
        cur |= bits;
        latx_platform_phys_write(pa, 8, cur);
    }
    *new_value = cur;
    return 0;
}

__attribute__((weak)) int latx_platform_phys_cmpxchg_u64(uint64_t pa,
                                                         uint64_t *expected,
                                                         uint64_t desired)
{
    uint64_t cur;
    if ((pa & 7u) != 0 || expected == 0) {
        return -1;
    }
    cur = latx_platform_phys_read(pa, 8);
    if (cur != *expected) {
        *expected = cur;
        return 1;
    }
    latx_platform_phys_write(pa, 8, desired);
    return 0;
}

static int walker_read_u64(void *opaque, uint64_t gpa, uint64_t *value)
{
    (void)opaque;
    return latx_platform_phys_read_u64_checked(gpa, value);
}

static int walker_atomic_or_u64(void *opaque, uint64_t gpa, uint64_t bits,
                                uint64_t *new_value)
{
    (void)opaque;
    return latx_platform_phys_atomic_or_u64(gpa, bits, new_value);
}

static int walker_cmpxchg_u64(void *opaque, uint64_t gpa,
                              uint64_t *expected, uint64_t desired)
{
    (void)opaque;
    return latx_platform_phys_cmpxchg_u64(gpa, expected, desired);
}

static uint32_t page_size_to_bits(uint64_t page_size)
{
    if (page_size == (UINT64_C(1) << 30)) {
        return 30;
    }
    if (page_size == (UINT64_C(1) << 21)) {
        return 21;
    }
    return 12;
}

LatxX86WalkResult latx_x86_walk(const CPUX86State *env, uint64_t va,
                                uint32_t access, LatxX86Translation *out)
{
    X86PagingState state;
    X86PagingMemory mem;
    X86Translation tr;
    X86PageFault pf;
    X86PagingAccess xacc;
    X86PagingResult res;

    memset(out, 0, sizeof(*out));
    out->gpa = va;
    out->page_bits = 12;

    if (!(env->cr[0] & CR0_PG_MASK)) {
        out->perm = access & (LATX_X86_ACCESS_READ | LATX_X86_ACCESS_WRITE |
                              LATX_X86_ACCESS_EXEC);
        return LATX_X86_WALK_PAGING_OFF;
    }
    if (!(env->cr[4] & CR4_PAE_MASK)) {
        return LATX_X86_WALK_UNSUPPORTED;
    }
    if (env->cr[4] & CR4_LA57_MASK) {
        return LATX_X86_WALK_RESERVED;
    }

    memset(&state, 0, sizeof(state));
    state.cr0 = env->cr[0];
    state.cr3 = env->cr[3];
    state.cr4 = env->cr[4];
    state.efer = env->efer | ((env->hflags & HF_LMA_MASK) ? X86_EFER_LMA : 0);
    state.rflags = env->eflags;
    state.pkru = env->pkru;
    state.pkrs = 0;
    state.features = X86_PAGING_FEATURE_1G_PAGES;
    state.maxphyaddr = 40;
    state.cpl = (access & LATX_X86_ACCESS_USER) ? 3 : 0;

    if (access & LATX_X86_ACCESS_EXEC) {
        xacc = X86_PAGING_EXECUTE;
    } else if (access & LATX_X86_ACCESS_WRITE) {
        xacc = X86_PAGING_WRITE;
    } else {
        xacc = X86_PAGING_READ;
    }

    mem.opaque = 0;
    mem.read_u64 = walker_read_u64;
    mem.atomic_or_u64 = walker_atomic_or_u64;
    mem.cmpxchg_u64 = walker_cmpxchg_u64;

    memset(&tr, 0, sizeof(tr));
    memset(&pf, 0, sizeof(pf));
    res = x86_64_walk_page_tables(&state, &mem, va, xacc, &tr, &pf);

    if (res == X86_PAGING_OK) {
        uint32_t perm = LATX_X86_ACCESS_READ;
        if (tr.writable) {
            perm |= LATX_X86_ACCESS_WRITE;
        }
        if (tr.user_accessible) {
            perm |= LATX_X86_ACCESS_USER;
        }
        if (tr.executable) {
            perm |= LATX_X86_ACCESS_EXEC;
        }
        out->gpa = tr.guest_physical_address;
        out->pte = tr.leaf_pte;
        out->page_bits = page_size_to_bits(tr.page_size);
        out->perm = perm;
        out->global = tr.global;
        out->pkey = tr.pkey;
        out->dirty = tr.dirty;
        out->fault_error_code = 0;
        return LATX_X86_WALK_OK;
    }

    if (res == X86_PAGING_PAGE_FAULT) {
        out->fault_error_code = pf.error_code;
        if (pf.error_code & X86_PF_RESERVED) {
            return LATX_X86_WALK_RESERVED;
        }
        if (pf.error_code & X86_PF_PRESENT) {
            return LATX_X86_WALK_PROTECTION;
        }
        return LATX_X86_WALK_NOT_PRESENT;
    }
    if (res == X86_PAGING_INVALID_STATE) {
        out->fault_error_code = X86_PF_PRESENT | X86_PF_RESERVED;
        return LATX_X86_WALK_RESERVED;
    }
    if (res == X86_PAGING_GENERAL_PROTECTION) {
        out->fault_error_code = 0;
        return LATX_X86_WALK_NOT_PRESENT;
    }
    if (res == X86_PAGING_MEMORY_ERROR) {
        out->fault_error_code = 0;
        return LATX_X86_WALK_NOT_PRESENT;
    }
    return LATX_X86_WALK_UNSUPPORTED;
}

/* ---------------- cached walks ---------------- */

typedef struct LatxX86TlbEntry {
    uint64_t cr3;
    uint64_t tag;        /* va >> page_bits */
    uint64_t gpa_base;   /* leaf base */
    uint32_t page_bits;
    uint32_t perm;
    uint16_t pcid;
    uint8_t  global;
    uint8_t  pkey;
    uint8_t  dirty;
    bool     valid;
} LatxX86TlbEntry;

#define LATX_MAX_PCPU 4u

__attribute__((weak)) uint32_t latx_platform_cpu_id(void)
{
    return 0u;
}

static LatxX86TlbEntry latx_x86_tlb[LATX_MAX_PCPU][LATX_X86_TLB_ENTRIES];
static LatxX86TlbEntry latx_x86_large_tlb[LATX_MAX_PCPU][LATX_X86_LARGE_TLB_ENTRIES];
static uint32_t latx_x86_large_victim[LATX_MAX_PCPU];
static LatxX86TlbStats latx_x86_stats[LATX_MAX_PCPU];

static inline uint32_t cur_tlb_cpu_idx(void)
{
    uint32_t cid = latx_platform_cpu_id();
    return cid < LATX_MAX_PCPU ? cid : 0u;
}

static inline LatxX86TlbEntry *cur_cpu_tlb(void)
{
    return latx_x86_tlb[cur_tlb_cpu_idx()];
}

static inline LatxX86TlbEntry *cur_cpu_large_tlb(void)
{
    return latx_x86_large_tlb[cur_tlb_cpu_idx()];
}

static inline LatxX86TlbStats *cur_cpu_tlb_stats(void)
{
    return &latx_x86_stats[cur_tlb_cpu_idx()];
}

const LatxX86TlbStats *latx_x86_tlb_stats(void)
{
    return cur_cpu_tlb_stats();
}

void latx_x86_tlb_stats_reset(void)
{
    memset(cur_cpu_tlb_stats(), 0, sizeof(LatxX86TlbStats));
}

void latx_x86_tlb_flush_all(void)
{
    uint32_t cid = cur_tlb_cpu_idx();
    memset(latx_x86_tlb[cid], 0, sizeof(latx_x86_tlb[0]));
    memset(latx_x86_large_tlb[cid], 0, sizeof(latx_x86_large_tlb[0]));
    latx_x86_large_victim[cid] = 0;
    latx_x86_stats[cid].flushes_all++;
}

void latx_x86_tlb_flush_cpu(uint32_t cpu)
{
    if (cpu < LATX_MAX_PCPU) {
        memset(latx_x86_tlb[cpu], 0, sizeof(latx_x86_tlb[cpu]));
        memset(latx_x86_large_tlb[cpu], 0, sizeof(latx_x86_large_tlb[cpu]));
        latx_x86_large_victim[cpu] = 0;
        latx_x86_stats[cpu].flushes_all++;
    }
}

void latx_x86_tlb_flush_all_cpus(void)
{
    memset(latx_x86_tlb, 0, sizeof(latx_x86_tlb));
    memset(latx_x86_large_tlb, 0, sizeof(latx_x86_large_tlb));
    memset(latx_x86_large_victim, 0, sizeof(latx_x86_large_victim));
    for (uint32_t i = 0; i < LATX_MAX_PCPU; i++) {
        latx_x86_stats[i].flushes_all++;
    }
}

static void flush_non_global_array(LatxX86TlbEntry *arr, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (arr[i].valid && !arr[i].global) {
            arr[i].valid = false;
        }
    }
}

void latx_x86_tlb_flush_non_global(void)
{
    uint32_t cid = cur_tlb_cpu_idx();
    flush_non_global_array(latx_x86_tlb[cid], LATX_X86_TLB_ENTRIES);
    flush_non_global_array(latx_x86_large_tlb[cid], LATX_X86_LARGE_TLB_ENTRIES);
    latx_x86_stats[cid].flushes_all++;
}

static void flush_pcid_array(LatxX86TlbEntry *arr, unsigned count, uint16_t pcid)
{
    for (unsigned i = 0; i < count; i++) {
        if (arr[i].valid && !arr[i].global && arr[i].pcid == pcid) {
            arr[i].valid = false;
        }
    }
}

void latx_x86_tlb_flush_pcid(uint16_t pcid)
{
    uint32_t cid = cur_tlb_cpu_idx();
    flush_pcid_array(latx_x86_tlb[cid], LATX_X86_TLB_ENTRIES, pcid);
    flush_pcid_array(latx_x86_large_tlb[cid], LATX_X86_LARGE_TLB_ENTRIES, pcid);
    latx_x86_stats[cid].flushes_all++;
}

static void flush_page_array(LatxX86TlbEntry *arr, unsigned count, uint64_t va)
{
    for (unsigned i = 0; i < count; i++) {
        LatxX86TlbEntry *e = &arr[i];

        if (e->valid && (va >> e->page_bits) == e->tag) {
            e->valid = false;
        }
    }
}

void latx_x86_tlb_flush_page(uint64_t va)
{
    uint32_t cid = cur_tlb_cpu_idx();
    flush_page_array(latx_x86_tlb[cid], LATX_X86_TLB_ENTRIES, va);
    flush_page_array(latx_x86_large_tlb[cid], LATX_X86_LARGE_TLB_ENTRIES, va);
    latx_x86_stats[cid].flushes_page++;
}

static uint32_t cached_access_fault_code(const CPUX86State *env, uint32_t access,
                                         uint32_t extra)
{
    uint32_t ec = X86_PF_PRESENT | extra;

    if (access & LATX_X86_ACCESS_WRITE) {
        ec |= X86_PF_WRITE;
    }
    if (access & LATX_X86_ACCESS_USER) {
        ec |= X86_PF_USER;
    }
    if ((access & LATX_X86_ACCESS_EXEC) &&
        ((env->efer & MSR_EFER_NXE) || (env->cr[4] & X86_CR4_SMEP))) {
        ec |= X86_PF_INSN;
    }
    return ec;
}

static int check_tlb_entry_hit(const CPUX86State *env, LatxX86TlbEntry *e,
                               uint64_t va, uint64_t cr3, uint16_t pcid,
                               uint32_t access, LatxX86Translation *out,
                               LatxX86WalkResult *out_res)
{
    if (!e->valid || (va >> e->page_bits) != e->tag ||
        !((e->global && (env->cr[4] & X86_CR4_PGE)) ||
          (e->cr3 == cr3 && e->pcid == pcid))) {
        return 0;
    }

    bool user = (access & LATX_X86_ACCESS_USER) != 0;
    bool write = (access & LATX_X86_ACCESS_WRITE) != 0;
    bool exec = (access & LATX_X86_ACCESS_EXEC) != 0;
    bool user_page = (e->perm & LATX_X86_ACCESS_USER) != 0;

    memset(out, 0, sizeof(*out));
    out->page_bits = e->page_bits;
    out->gpa = e->gpa_base | (va & ((UINT64_C(1) << e->page_bits) - 1));
    out->perm = e->perm;
    out->global = e->global;
    out->pkey = e->pkey;
    out->dirty = e->dirty;

    if (user && !user_page) {
        out->fault_error_code = cached_access_fault_code(env, access, 0);
        *out_res = LATX_X86_WALK_PROTECTION;
        return 1;
    }
    if (write && !(e->perm & LATX_X86_ACCESS_WRITE) &&
        (user || (env->cr[0] & CR0_WP_MASK))) {
        out->fault_error_code = cached_access_fault_code(env, access, 0);
        *out_res = LATX_X86_WALK_PROTECTION;
        return 1;
    }
    if (exec && !(e->perm & LATX_X86_ACCESS_EXEC)) {
        out->fault_error_code = cached_access_fault_code(env, access, 0);
        *out_res = LATX_X86_WALK_PROTECTION;
        return 1;
    }
    if (exec && !user && user_page && (env->cr[4] & X86_CR4_SMEP)) {
        out->fault_error_code = cached_access_fault_code(env, access, 0);
        *out_res = LATX_X86_WALK_PROTECTION;
        return 1;
    }
    if (!exec && !user && user_page && (env->cr[4] & X86_CR4_SMAP) &&
        !(env->eflags & X86_RFLAGS_AC)) {
        out->fault_error_code = cached_access_fault_code(env, access, 0);
        *out_res = LATX_X86_WALK_PROTECTION;
        return 1;
    }
    if (!exec && user && user_page && (env->cr[4] & X86_CR4_PKE)) {
        uint32_t rights = (env->pkru >> (e->pkey * 2u)) & 3u;
        if ((rights & 1u) || (write && (rights & 2u))) {
            out->fault_error_code = cached_access_fault_code(
                env, access, X86_PF_PROTECTION_KEY);
            *out_res = LATX_X86_WALK_PROTECTION;
            return 1;
        }
    }
    /* Dirty-bit promotion: fall through to authoritative walker on first write. */
    if (write && !e->dirty) {
        LatxX86WalkResult r = latx_x86_walk(env, va, access, out);
        if (r == LATX_X86_WALK_OK) {
            e->dirty = out->dirty;
        }
        *out_res = r;
        return 1;
    }
    *out_res = LATX_X86_WALK_OK;
    return 1;
}

static void store_tlb_entry(LatxX86TlbEntry *e, uint64_t va, uint64_t cr3,
                            uint16_t pcid, const LatxX86Translation *out)
{
    e->cr3 = cr3;
    e->pcid = pcid;
    e->page_bits = out->page_bits;
    e->tag = va >> out->page_bits;
    e->gpa_base = out->gpa & ~((UINT64_C(1) << out->page_bits) - 1);
    e->perm = out->perm;
    e->global = out->global;
    e->pkey = out->pkey;
    e->dirty = out->dirty;
    e->valid = true;
}

LatxX86WalkResult latx_x86_walk_cached(const CPUX86State *env, uint64_t va,
                                       uint32_t access,
                                       LatxX86Translation *out)
{
    uint32_t cid = cur_tlb_cpu_idx();
    LatxX86TlbStats *stats = &latx_x86_stats[cid];
    uint64_t cr3 = env->cr[3] & PTE_PHYS_MASK;
    uint16_t pcid = (env->cr[4] & X86_CR4_PCIDE) ? (uint16_t)(env->cr[3] & 0xfffu) : 0u;
    LatxX86TlbEntry *ltlb = latx_x86_large_tlb[cid];
    LatxX86TlbEntry *e4k = &latx_x86_tlb[cid][(va >> 12) & (LATX_X86_TLB_ENTRIES - 1)];
    LatxX86WalkResult r;

    /* Paging off, or no long-mode paging at all: delegate directly. */
    if (!(env->cr[0] & CR0_PG_MASK) || !(env->cr[4] & CR4_PAE_MASK)) {
        return latx_x86_walk(env, va, access, out);
    }

    stats->lookups++;
    for (unsigned i = 0; i < LATX_X86_LARGE_TLB_ENTRIES; i++) {
        if (check_tlb_entry_hit(env, &ltlb[i], va, cr3, pcid, access, out, &r)) {
            if (ltlb[i].page_bits == 30) {
                stats->hits_1g++;
            } else {
                stats->hits_2m++;
            }
            return r;
        }
    }

    if (check_tlb_entry_hit(env, e4k, va, cr3, pcid, access, out, &r)) {
        stats->hits_4k++;
        return r;
    }

    stats->misses++;
    r = latx_x86_walk(env, va, access, out);
    if (r != LATX_X86_WALK_OK) {
        /* Never cache faulting entries; only cache valid translations. */
        return r;
    }

    if (out->page_bits >= 21u) {
        LatxX86TlbEntry *victim = 0;
        for (unsigned i = 0; i < LATX_X86_LARGE_TLB_ENTRIES; i++) {
            if (!ltlb[i].valid) {
                victim = &ltlb[i];
                break;
            }
        }
        if (victim == 0) {
            uint32_t v = latx_x86_large_victim[cid] % LATX_X86_LARGE_TLB_ENTRIES;
            victim = &ltlb[v];
            latx_x86_large_victim[cid] = (v + 1u) % LATX_X86_LARGE_TLB_ENTRIES;
        }
        store_tlb_entry(victim, va, cr3, pcid, out);
        if (out->page_bits == 30u) {
            stats->fills_1g++;
        } else {
            stats->fills_2m++;
        }
    } else {
        store_tlb_entry(e4k, va, cr3, pcid, out);
        stats->fills_4k++;
    }
    return r;
}

uint64_t latx_x86_access_hpa;

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
            latx_x86_tlb_flush_page(gva);
            env->cr[2] = gva;
            env->exception_index = EXCP0E_PAGE;
            env->error_code = (int)(t.fault_error_code
                                        ? t.fault_error_code
                                        : latx_x86_fault_error_code(env, r, access));
            return LATX_X86_ACCESS_FAULTED;
        }
        {
            uint64_t d = t.gpa - page;

            if (!have_delta) {
                delta = d;
                have_delta = true;
                hpa = gva + delta;
            } else if (d != delta) {
                env->cr[2] = gva;
                env->exception_index = EXCP0E_PAGE;
                env->error_code = 1 | 8;
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

static int walk_or_flat(const CPUX86State *env, uint64_t gva, uint32_t access,
                        uint64_t *gpa_out)
{
    LatxX86Translation t;
    LatxX86WalkResult r = latx_x86_walk(env, gva, access, &t);

    if (r != LATX_X86_WALK_OK && r != LATX_X86_WALK_PAGING_OFF) {
        return 0;
    }
    *gpa_out = t.gpa;
    return 1;
}

int latx_x86_get_tss_stack(const CPUX86State *env, uint32_t ist,
                           uint64_t *out_rsp)
{
    uint16_t tr_sel = (uint16_t)(env->tr.selector & ~7u);
    uint64_t desc_gva = env->gdt.base + (uint64_t)tr_sel;
    uint64_t lo_pa, hi_pa, lo, hi, tss_base, slot_gva, slot_pa, slot_hi_pa;
    uint64_t tss_off;

    if (tr_sel == 0 || env->gdt.base == 0) {
        return 0;
    }
    if (!walk_or_flat(env, desc_gva, LATX_X86_ACCESS_READ, &lo_pa) ||
        !walk_or_flat(env, desc_gva + 8, LATX_X86_ACCESS_READ, &hi_pa)) {
        return 0;
    }
    lo = latx_platform_phys_read(lo_pa, 8);
    hi = latx_platform_phys_read(hi_pa, 8);
    tss_base = ((lo >> 16) & 0xffffffULL) |
               (((lo >> 56) & 0xffULL) << 24) |
               ((hi & 0xffffffffULL) << 32);
    if (tss_base == 0) {
        return 0;
    }
    tss_off = (ist >= 1u && ist <= 7u) ? (36ULL + (uint64_t)(ist - 1u) * 8ULL)
                                       : 4ULL; /* RSP0 */
    slot_gva = tss_base + tss_off;
    if (!walk_or_flat(env, slot_gva, LATX_X86_ACCESS_READ, &slot_pa) ||
        !walk_or_flat(env, slot_gva + 4, LATX_X86_ACCESS_READ, &slot_hi_pa)) {
        return 0;
    }
    *out_rsp = latx_platform_phys_read(slot_pa, 4) |
               (latx_platform_phys_read(slot_hi_pa, 4) << 32);
    return *out_rsp != 0;
}

void helper_syscall_sys(CPUX86State *env, uint64_t next_rip)
{
    uint64_t rflags = (env->eflags & 0x3c7fd7ULL) | 0x2ULL;
    uint16_t cs_sel = (uint16_t)((env->star >> 32) & 0xfffcU);

    env->regs[R_ECX] = next_rip;
    env->regs[R_R11] = rflags;
    env->eflags = (rflags & ~(env->fmask & 0x3c7fd7ULL)) | 0x2ULL;
    env->segs[R_CS].selector = cs_sel;
    env->segs[R_SS].selector = (uint16_t)(cs_sel + 8U);
    env->hflags &= ~(uint32_t)HF_CPL_MASK;
    env->eip = env->lstar;
    latx_x86_tlb_flush_non_global();
    latx_platform_tlb_flush_all();
}

void helper_sysret_sys(CPUX86State *env)
{
    uint16_t star_hi = (uint16_t)(env->star >> 48);
    uint64_t rflags = (env->regs[R_R11] & 0x3c7fd7ULL) | 0x2ULL;

    env->eip = env->regs[R_ECX];
    env->eflags = rflags;
    env->segs[R_CS].selector = (uint16_t)((star_hi + 16U) | 3U);
    env->segs[R_SS].selector = (uint16_t)((star_hi + 8U) | 3U);
    env->hflags = (env->hflags & ~(uint32_t)HF_CPL_MASK) | 3U;
    latx_x86_tlb_flush_non_global();
    latx_platform_tlb_flush_all();
}

int latx_x86_deliver_fault(CPUX86State *env, uint32_t vector)
{
    uint64_t gate_gva = env->idt.base + (uint64_t)vector * 16;
    uint64_t gate_lo_pa, gate_hi_pa;
    uint64_t lo, hi, off, selector, rsp;
    uint32_t ist;
    uint8_t gate_type;
    uint32_t old_cpl = (uint32_t)(env->hflags & HF_CPL_MASK);
    uint64_t old_rsp = env->regs[R_ESP];
    uint32_t old_ss = env->segs[R_SS].selector;
    uint32_t old_cs = env->segs[R_CS].selector;

    if ((uint64_t)vector * 16 + 15 > env->idt.limit + 1) {
        return 1;
    }
    if (!walk_or_flat(env, gate_gva, LATX_X86_ACCESS_READ, &gate_lo_pa) ||
        !walk_or_flat(env, gate_gva + 8, LATX_X86_ACCESS_READ, &gate_hi_pa)) {
        return 1;
    }
    lo = latx_platform_phys_read(gate_lo_pa, 8);
    hi = latx_platform_phys_read(gate_hi_pa, 8);

    {
        uint8_t type_attr = (uint8_t)(lo >> 40);

        if (!(type_attr & 0x80) || (type_attr & 0x1f) < 0x0e) {
            return 1;
        }
        gate_type = (uint8_t)(type_attr & 0x1fu);
    }
    ist = (uint32_t)((lo >> 32) & 7u);
    selector = (lo >> 16) & 0xffff;
    off = (lo & 0xffffULL) |
          (((lo >> 48) & 0xffffULL) << 16) |
          ((hi & 0xffffffffULL) << 32);

    rsp = old_rsp;
    if (ist != 0 || old_cpl != 0) {
        uint64_t tss_rsp = 0;
        if (latx_x86_get_tss_stack(env, ist, &tss_rsp)) {
            rsp = tss_rsp;
        }
    }
    rsp &= ~UINT64_C(15);

#define PUSH64(v)                                                             \
    do {                                                                      \
        uint64_t _pa;                                                         \
        if (!walk_or_flat(env, rsp - 8, LATX_X86_ACCESS_WRITE, &_pa)) {       \
            return 1;                                                         \
        }                                                                     \
        latx_platform_phys_write(_pa, 8, (uint64_t)(v));                      \
        rsp -= 8;                                                             \
    } while (0)

    PUSH64(old_ss);
    PUSH64(old_rsp);
    PUSH64(env->eflags & ~(UINT64_C(1) << 8));
    PUSH64(old_cs);
    PUSH64(env->eip);
    PUSH64((uint64_t)(uint32_t)env->error_code);
#undef PUSH64

    latx_x86_tlb_flush_all();

    env->regs[R_ESP] = rsp;
    env->eip = off;
    env->segs[R_CS].selector = (uint32_t)selector;
    env->segs[R_CS].base = 0;
    if (old_cpl != 0) {
        env->segs[R_SS].selector = 0;
    }
    env->hflags &= ~(uint32_t)HF_CPL_MASK;
    env->eflags &= ~(UINT64_C(1) << 8);
    if (gate_type == 0x0eu) {
        env->eflags &= ~IF_MASK;
    }
    env->exception_index = 0;
    return 0;
}

uint8_t latx_x86_fetch_byte(const CPUX86State *env, uint64_t gva)
{
    LatxX86Translation t;
    LatxX86WalkResult r;

    if (!(env->cr[0] & CR0_PG_MASK) || !(env->cr[4] & CR4_PAE_MASK) ||
        env->cr[3] == 0) {
        return latx_bare_ldub_code(gva);
    }

    uint32_t fetch_acc = LATX_X86_ACCESS_EXEC |
                         ((env->hflags & HF_CPL_MASK) ? LATX_X86_ACCESS_USER : 0);
    r = latx_x86_walk_cached(env, gva, fetch_acc, &t);
    if (r != LATX_X86_WALK_OK) {
        CPUX86State *mutable = (CPUX86State *)(uintptr_t)env;

        mutable->cr[2] = gva;
        mutable->exception_index = EXCP0E_PAGE;
        mutable->error_code = (int)(t.fault_error_code
                                        ? t.fault_error_code
                                        : latx_x86_fault_error_code(env, r, fetch_acc));
        return 0xcc;
    }
    return (uint8_t)latx_platform_phys_read(t.gpa, 1);
}

uint32_t latx_x86_fault_error_code(const CPUX86State *env,
                                   LatxX86WalkResult result, uint32_t access)
{
    uint32_t ec = 0;

    if (result == LATX_X86_WALK_PROTECTION) {
        ec |= 1u;
    } else if (result == LATX_X86_WALK_RESERVED) {
        ec |= 1u | 8u;
    }
    if (access & LATX_X86_ACCESS_WRITE) {
        ec |= 2u;
    }
    if (access & LATX_X86_ACCESS_USER) {
        ec |= 4u;
    }
    if ((access & LATX_X86_ACCESS_EXEC) &&
        ((env->efer & MSR_EFER_NXE) || (env->cr[4] & X86_CR4_SMEP))) {
        ec |= 16u;
    }
    return ec;
}
