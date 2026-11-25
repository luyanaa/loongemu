#include "latr.h"
#include <string.h>
#include "mmu.h"
#include "translate.h"
#include "loongemu/phys_mem.h"
#include "loongemu/mmio_bus.h"
#include "loongemu-perf.h"

#define CSR_SATP_ADDR   0x180
#define CSR_MSTATUS_ADDR 0x300
#define MSTATUS_SUM_BIT 18
#define MSTATUS_MXR_BIT 19
#define MSTATUS_MPRV_BIT 17
#define MSTATUS_MPP_SHIFT 11
#define CSR_PMPCFG0 0x3a0
#define CSR_PMPCFG2 0x3a2
#define CSR_PMPADDR0 0x3b0
#define CSR_MENVCFG 0x30a
#define CSR_MSECCFG 0x747
#define MENVCFG_ADUE_BIT 61
#define MENVCFG_PBMTE_BIT 62
#define PTE_PBMT_MASK (UINT64_C(3) << 61)
#define PMP_R 1u
#define PMP_W 2u
#define PMP_X 4u
#define PMP_A 0x18u
#define PMP_L 0x80u
#define PMP_ADDR_MASK ((UINT64_C(1) << 54) - 1)
#define MSECCFG_RLB_BIT  0
#define MSECCFG_MMWP_BIT 1
#define MSECCFG_MML_BIT  2
#define SATP_MODE(v) ((unsigned)((v) >> 60))
#define SATP_ROOT(v) (((v) & LATR_PTE_PPN_MASK) << 12)
#define SATP_ASID(v) ((uint32_t)(((v) >> 44) & 0xffffu))

static int fault_cause(unsigned access)
{
    switch (access) {
    case LATR_ACCESS_STORE:
        return LATR_EXCP_STORE_PAGE_FAULT;
    case LATR_ACCESS_EXEC:
        return LATR_EXCP_INST_PAGE_FAULT;
    default:
        return LATR_EXCP_LOAD_PAGE_FAULT;
    }
}

static int pte_host_pointer(CPUArchState *env, uint64_t pa, int write,
                            uint64_t *host)
{
    LoongemuMemLookup lookup;

    if (env->mem_map == NULL) {
        *host = pa;
        return 0;
    }
    LoongemuMemKind kind = loongemu_mem_resolve(env->mem_map, pa, 8, &lookup);
    if (kind == LOONGEMU_MEM_RAM || (kind == LOONGEMU_MEM_ROM && !write)) {
        *host = lookup.hpa;
        return 0;
    }
    return write ? LATR_EXCP_STORE_ACCESS_FAULT : LATR_EXCP_LOAD_ACCESS_FAULT;
}

static int pmp_access_fault(unsigned access)
{
    switch (access) {
    case LATR_ACCESS_STORE:
        return LATR_EXCP_STORE_ACCESS_FAULT;
    case LATR_ACCESS_EXEC:
        return LATR_EXCP_INST_ACCESS_FAULT;
    default:
        return LATR_EXCP_LOAD_ACCESS_FAULT;
    }
}

static uint8_t pmp_cfg_byte(const CPUArchState *env, unsigned index)
{
    uint64_t cfg = env->csr[index < 8 ? CSR_PMPCFG0 : CSR_PMPCFG2];

    return (uint8_t)(cfg >> (8 * (index & 7u)));
}

static uint64_t pmp_addr(const CPUArchState *env, unsigned index)
{
    return env->csr[CSR_PMPADDR0 + index] & PMP_ADDR_MASK;
}

/* Decode a PMP entry into a half-open [lo, hi) range. Returns false for
 * A=OFF and for inactive TOR/NA4/NAPOT encodings (empty range). */
static bool pmp_entry_range(const CPUArchState *env, unsigned index,
                            uint8_t cfg, uint64_t *lo, uint64_t *hi)
{
    uint64_t base = pmp_addr(env, index) << 2;

    switch (cfg & PMP_A) {
    case 0x08: /* TOR */
        *lo = index == 0 ? 0 : (pmp_addr(env, index - 1) << 2);
        *hi = base;
        break;
    case 0x10: /* NA4 */
        *lo = base;
        *hi = base + 4;
        break;
    case 0x18: { /* NAPOT; QEMU pmp_decode_napot */
        uint64_t nar = base | 3u;
        *lo = nar & (nar + 1);
        *hi = (nar | (nar + 1)) + 1;
        break;
    }
    default:
        return false;
    }
    return *hi > *lo;
}

/* Smepmp truth table indexed by L:R:W:X. */
static bool pmp_mml_allows(uint8_t cfg, unsigned perm, unsigned privilege)
{
    static const uint8_t m_mode[16] = {
        0, 0, PMP_R | PMP_W, PMP_R | PMP_W,
        0, 0, 0, 0,
        0, PMP_X, PMP_X, PMP_R | PMP_X,
        PMP_R, PMP_R | PMP_X, PMP_R | PMP_W, PMP_R,
    };
    static const uint8_t su_mode[16] = {
        0, PMP_X, PMP_R, PMP_R | PMP_W,
        PMP_R, PMP_R | PMP_X, PMP_R | PMP_W, PMP_R | PMP_W | PMP_X,
        0, 0, PMP_X, PMP_X,
        0, 0, 0, PMP_R,
    };
    unsigned op = ((cfg & PMP_L) ? 8u : 0u) |
                  ((cfg & PMP_R) ? 4u : 0u) |
                  ((cfg & PMP_W) ? 2u : 0u) |
                  ((cfg & PMP_X) ? 1u : 0u);

    return ((privilege == LATR_RISCV_PRIV_M ? m_mode[op] : su_mode[op]) &
            perm) != 0;
}

static int pmp_entry_check(uint8_t cfg, unsigned perm,
                           unsigned access, unsigned privilege, bool mml)
{
    if (!mml) {
        /* An unlocked rule does not constrain M-mode. */
        if (privilege == LATR_RISCV_PRIV_M && (cfg & PMP_L) == 0) {
            return 0;
        }
        return ((cfg & (PMP_R | PMP_W | PMP_X)) & perm) != 0
                   ? 0 : pmp_access_fault(access);
    }
    return pmp_mml_allows(cfg, perm, privilege) ? 0 : pmp_access_fault(access);
}

int latr_pmp_check(const CPUArchState *env, uint64_t pa, unsigned size,
                   unsigned access, unsigned privilege)
{
    uint64_t mseccfg = env->csr[CSR_MSECCFG];
    bool mml = (mseccfg >> MSECCFG_MML_BIT) & 1u;
    unsigned perm = access == LATR_ACCESS_STORE ? PMP_W
                  : access == LATR_ACCESS_EXEC ? PMP_X
                                               : PMP_R;
    uint64_t end;
    unsigned i;

    if (size == 0) {
        size = 1;
    }
    end = pa + (size - 1);
    if (end < pa) {
        end = UINT64_MAX;
    }

    /* The lowest-numbered entry that matches any byte decides the access.
     * It must cover every byte, even when it grants no permission at all:
     * a partial match fails and never falls through to a later entry. */
    for (i = 0; i < 16; i++) {
        uint8_t cfg = pmp_cfg_byte(env, i);
        uint64_t lo, hi;

        if (!pmp_entry_range(env, i, cfg, &lo, &hi)) {
            continue;
        }
        if (pa >= hi || end < lo) {
            continue;
        }
        if (pa < lo || end >= hi) {
            return pmp_access_fault(access);
        }
        return pmp_entry_check(cfg, perm, access, privilege, mml);
    }

    /* No matching rule. */
    if (mseccfg & (UINT64_C(1) << MSECCFG_MMWP_BIT)) {
        return pmp_access_fault(access);
    }
    if (mml) {
        /* MML denies execution from unmatched regions; M-mode R/W is the
         * base-PMP default. S/U never succeeds without a matching rule. */
        if (privilege == LATR_RISCV_PRIV_M && access != LATR_ACCESS_EXEC) {
            return 0;
        }
        return pmp_access_fault(access);
    }
    if (privilege == LATR_RISCV_PRIV_M) {
        return 0;
    }
    return pmp_access_fault(access);
}

static unsigned effective_privilege(const CPUArchState *env, unsigned access)
{
    unsigned privilege = (unsigned)env->privilege;

    if (privilege == LATR_RISCV_PRIV_M &&
        access != LATR_ACCESS_EXEC &&
        ((env->csr[0x300] >> MSTATUS_MPRV_BIT) & 1u)) {
        privilege = (unsigned)((env->csr[0x300] >> MSTATUS_MPP_SHIFT) & 3u);
        if (privilege == 2) {
            privilege = LATR_RISCV_PRIV_U;
        }
    }
    return privilege;
}

bool latr_mmu_enabled(const CPUArchState *env)
{
    unsigned mode = SATP_MODE(env->csr[CSR_SATP_ADDR]);
    return mode == 8 || mode == 9 || mode == 10;
}

static int satp_geometry(unsigned mode, int *levels, int *va_bits)
{
    switch (mode) {
    case 8:
        *levels = 3;
        *va_bits = 39;
        return 0;
    case 9:
        *levels = 4;
        *va_bits = 48;
        return 0;
    case 10:
        *levels = 5;
        *va_bits = 57;
        return 0;
    default:
        return -1;
    }
}

static bool pte_pbmt_ok(const CPUArchState *env, uint64_t pte)
{
    unsigned pbmt = (unsigned)((pte >> 61) & 3u);

    if (pbmt == 0) {
        return true;
    }
    if (!(env->csr[CSR_MENVCFG] & (UINT64_C(1) << MENVCFG_PBMTE_BIT))) {
        return false;
    }
    return pbmt != 3; /* encoding 3 is reserved */
}

static bool pte_permits(const CPUArchState *env, uint64_t pte,
                        unsigned access, unsigned privilege)
{
    uint64_t status = env->csr[CSR_MSTATUS_ADDR];
    bool sum = (status >> MSTATUS_SUM_BIT) & 1;
    bool mxr = (status >> MSTATUS_MXR_BIT) & 1;
    bool user_page = (pte & LATR_PTE_U) != 0;

    if (privilege == LATR_RISCV_PRIV_U) {
        if (!user_page) {
            return false;
        }
    } else if (access == LATR_ACCESS_EXEC) {
        if (user_page) {
            return false;
        }
    } else if (user_page && !sum) {
        return false;
    }

    if (access == LATR_ACCESS_LOAD) {
        return (pte & LATR_PTE_R) != 0 || (mxr && (pte & LATR_PTE_X) != 0);
    }
    if (access == LATR_ACCESS_STORE) {
        return (pte & LATR_PTE_W) != 0;
    }
    return (pte & LATR_PTE_X) != 0;
}

static void tlb_flush_all(CPUArchState *env)
{
    memset(env->tlb, 0, sizeof(env->tlb));
}

/* Honours outstanding SFENCE.VMA/FENCE.I requests. The runtime clears these
 * flags after retiring the TB; until then every walk re-applies the flush. */
static void tlb_flush_asid(CPUArchState *env, uint32_t asid)
{
    unsigned i;

    for (i = 0; i < LATR_TLB_ENTRIES; i++) {
        if (env->tlb[i].valid &&
            SATP_ASID(env->tlb[i].satp) == asid) {
            env->tlb[i].valid = 0;
        }
    }
}

static void tlb_apply_pending_flush(CPUArchState *env)
{
    bool all = env->fence_i_pending != 0;
    uint32_t asid = UINT32_MAX;

    if (env->tlb_flush_seq != env->tlb_flush_seen) {
        uint64_t delta = env->tlb_flush_seq - env->tlb_flush_seen;

        /* A single unscoped SFENCE can still flush just its ASID; anything
         * else (FENCE.I, several pending requests, or the runtime already
         * clearing the flags) conservatively flushes everything. */
        if (delta == 1 && !all && env->sfence_pending &&
            env->sfence_asid != UINT32_MAX) {
            asid = env->sfence_asid;
        } else {
            all = true;
        }
        env->tlb_flush_seen = env->tlb_flush_seq;
    } else if (env->sfence_pending) {
        if (env->sfence_asid == UINT32_MAX) {
            all = true;
        } else {
            asid = env->sfence_asid;
        }
    }
    if (all) {
        tlb_flush_all(env);
    } else if (asid != UINT32_MAX) {
        tlb_flush_asid(env, asid);
    }
}

/* Returns 0 on a hit, a fault cause, or 1 on a miss. */
static int tlb_lookup(CPUArchState *env, uint64_t va, unsigned access,
                      unsigned privilege, uint64_t *pa_out)
{
    LatrTlbEntry *e = &env->tlb[(va >> 12) & (LATR_TLB_ENTRIES - 1)];
    uint64_t pte;
    uint64_t required;

    if (!e->valid || e->va_page != (va >> 12) ||
        e->satp != env->csr[CSR_SATP_ADDR]) {
        return 1;
    }
    /* Re-read the leaf PTE so an unsynchronised table edit cannot produce a
     * stale translation. */
    pte = *(volatile uint64_t *)(uintptr_t)e->pte_host;
    if (pte != e->pte) {
        e->valid = 0;
        return 1;
    }
    if (!pte_pbmt_ok(env, pte) || !pte_permits(env, pte, access, privilege)) {
        return fault_cause(access);
    }
    required = LATR_PTE_A;
    if (access == LATR_ACCESS_STORE) {
        required |= LATR_PTE_D;
    }
    if ((pte & required) != required) {
        return 1; /* the walker updates A/D and re-inserts */
    }
    {
        uint64_t pa = e->pa_page | (va & e->page_mask);
        int pmp = latr_pmp_check(env, pa, 1, access, privilege);

        if (pmp != 0) {
            return pmp;
        }
        env->tlb_hits++;
        *pa_out = pa;
        return 0;
    }
}

static void tlb_insert(CPUArchState *env, uint64_t va, uint64_t pte,
                       uint64_t pte_host, uint64_t ppn, int level)
{
    LatrTlbEntry *e = &env->tlb[(va >> 12) & (LATR_TLB_ENTRIES - 1)];

    e->valid = 0;
    e->plan_valid = 0;
    e->plan_ctx = 0;
    e->plan_ok_kinds = 0;
    e->va_page = va >> 12;
    e->satp = env->csr[CSR_SATP_ADDR];
    e->pte = pte;
    e->pte_host = pte_host;
    e->pa_page = ppn << 12;
    e->page_mask = (UINT64_C(1) << (12 + 9 * level)) - 1;
    e->valid = 1;
}

int latr_walk(CPUArchState *env, uint64_t va, unsigned access, uint64_t *pa_out)
{
    unsigned mode = SATP_MODE(env->csr[CSR_SATP_ADDR]);
    unsigned privilege = effective_privilege(env, access);
    int levels, va_bits;

    tlb_apply_pending_flush(env);

    if (mode == 0 || privilege == LATR_RISCV_PRIV_M) {
        int pmp = latr_pmp_check(env, va, 1, access, privilege);
        if (pmp != 0) {
            return pmp;
        }
        *pa_out = va;
        return 0;
    }
    if (satp_geometry(mode, &levels, &va_bits) != 0) {
        return fault_cause(access);
    }

    /* Non-canonical virtual addresses fault; the walker does not guess. */
    if ((int64_t)(va << (64 - va_bits)) >> (64 - va_bits) != (int64_t)va) {
        return fault_cause(access);
    }

    {
        int cached = tlb_lookup(env, va, access, privilege, pa_out);
        if (cached <= 0) {
            if (cached == 0) {
                loongemu_perf_count(LOONGEMU_PERF_TLB_HIT, 1);
            } else {
                loongemu_perf_count(LOONGEMU_PERF_MEM_FAULT, 1);
            }
            return cached;
        }
        /* Exactly 1: the software TLB has no entry, walk the page table. */
        loongemu_perf_count(LOONGEMU_PERF_TLB_MISS, 1);
    }

    uint64_t table = SATP_ROOT(env->csr[CSR_SATP_ADDR]);
    for (int level = levels - 1; level >= 0; level--) {
        uint64_t vpn = (va >> (12 + 9 * level)) & 0x1ff;
        uint64_t pte_addr = table + vpn * sizeof(uint64_t);
        uint64_t pte_host = 0;
        uint64_t pte;
        /* Implicit page-table accesses use S-mode PMP checks regardless of
         * the effective privilege of the original access, and a violation
         * raises an access fault matching the original access type. */
        int pmp = latr_pmp_check(env, pte_addr, 8, LATR_ACCESS_LOAD,
                                 LATR_RISCV_PRIV_S);
        int host_cause;

        if (pmp != 0) {
            return pmp_access_fault(access);
        }
        host_cause = pte_host_pointer(env, pte_addr, 0, &pte_host);
        if (host_cause != 0) {
            return pmp_access_fault(access);
        }
        pte = *(volatile uint64_t *)(uintptr_t)pte_host;
        uint64_t ppn = (pte >> LATR_PTE_PPN_SHIFT) & LATR_PTE_PPN_MASK;

        /* Invalid, or W without R, is a fault at every level. */
        if (!(pte & LATR_PTE_V) ||
            ((pte & (LATR_PTE_R | LATR_PTE_W)) == LATR_PTE_W)) {
            return fault_cause(access);
        }
        if (!pte_pbmt_ok(env, pte)) {
            return fault_cause(access);
        }

        if (!(pte & (LATR_PTE_R | LATR_PTE_W | LATR_PTE_X))) {
            if (level == 0) {
                return fault_cause(access);
            }
            table = ppn << 12;
            continue;
        }

        if (level > 0 && (ppn & ((UINT64_C(1) << (9 * level)) - 1))) {
            return fault_cause(access);
        }

        if (!pte_permits(env, pte, access, privilege)) {
            return fault_cause(access);
        }

        {
            uint64_t required = LATR_PTE_A;
            if (access == LATR_ACCESS_STORE) {
                required |= LATR_PTE_D;
            }
            if ((pte & required) != required) {
                if (!(env->csr[CSR_MENVCFG] &
                      (UINT64_C(1) << MENVCFG_ADUE_BIT))) {
                    /* Svade: software-managed A/D raises a page fault. */
                    return fault_cause(access);
                }
                {
                    uint64_t write_host = 0;
                    int write_pmp = latr_pmp_check(env, pte_addr, 8,
                                                   LATR_ACCESS_STORE,
                                                   LATR_RISCV_PRIV_S);
                    int write_cause;
                    volatile uint64_t *slot;

                    if (write_pmp != 0) {
                        return pmp_access_fault(access);
                    }
                    write_cause = pte_host_pointer(env, pte_addr, 1,
                                                   &write_host);
                    if (write_cause != 0) {
                        return pmp_access_fault(access);
                    }
                    slot = (volatile uint64_t *)(uintptr_t)write_host;
                    uint64_t old = __atomic_load_n(slot, __ATOMIC_RELAXED);
                    for (;;) {
                        uint64_t desired = old | required;
                        if (__atomic_compare_exchange_n(
                                slot, &old, desired, false,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                            break;
                        }
                    }
                }
                pte |= required;
            }
        }

        {
            uint64_t pa = (ppn << 12) |
                          (va & ((UINT64_C(1) << (12 + 9 * level)) - 1));
            int pmp = latr_pmp_check(env, pa, 1, access, privilege);
            if (pmp != 0) {
                return pmp;
            }
            tlb_insert(env, va, pte, pte_host, ppn, level);
            *pa_out = pa;
            return 0;
        }
    }

    return fault_cause(access);
}

static int access_fault_cause(unsigned access)
{
    switch (access) {
    case LATR_ACCESS_STORE:
        return LATR_EXCP_STORE_ACCESS_FAULT;
    case LATR_ACCESS_EXEC:
        return LATR_EXCP_INST_ACCESS_FAULT;
    default:
        return LATR_EXCP_LOAD_ACCESS_FAULT;
    }
}

/* Bit 0 of the epoch word is the writer lock; the release below advances the
 * epoch by two so LR/SC observes a changed generation after every write. */
volatile uint64_t latr_guest_store_lock;

static uint64_t reservation_lock(void)
{
    uint64_t old;

    for (;;) {
        old = __atomic_load_n(&latr_guest_store_lock, __ATOMIC_ACQUIRE);
        if (old & 1u) {
            continue;
        }
        if (__atomic_compare_exchange_n(&latr_guest_store_lock, &old,
                                        old + 1, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return old;
        }
    }
}

static void reservation_unlock(uint64_t epoch)
{
    __atomic_store_n(&latr_guest_store_lock, epoch + 2, __ATOMIC_RELEASE);
}

uint64_t latr_reservation_lock(void)
{
    return reservation_lock();
}

void latr_reservation_unlock(uint64_t epoch)
{
    reservation_unlock(epoch);
}



/* Non-CPU writers (device DMA) invalidate LR/SC by advancing the epoch. The
 * lock is not taken when it is already held: the active CPU writer releases
 * with epoch+2, which is exactly the invalidation a reservation needs, and
 * spinning here would deadlock when DMA is triggered from a guest store. */
void latr_reservation_notify_write(uint64_t pa, unsigned size)
{
    uint64_t old = __atomic_load_n(&latr_guest_store_lock, __ATOMIC_ACQUIRE);

    (void)pa;
    (void)size;
    while (!(old & 1u)) {
        if (__atomic_compare_exchange_n(&latr_guest_store_lock, &old,
                                        old + 1, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&latr_guest_store_lock, old + 2, __ATOMIC_RELEASE);
            return;
        }
    }
}

static int guest_access(CPUArchState *env, uint64_t va, unsigned size,
                        unsigned access, uint64_t value, bool is_load,
                        uint64_t *result)
{
    unsigned done = 0;
    uint64_t acc = 0;
    uint64_t write_epoch = 0;
    bool write_locked = false;

    env->mem_fault_cause = 0;
    while (done < size) {
        uint64_t addr = va + done;
        unsigned chunk = (unsigned)(0x1000 - (addr & 0xfff));
        uint64_t host = 0;
        uint64_t phys = 0;
        unsigned i;
        int cause;

        if (chunk > size - done) {
            chunk = size - done;
        }
        cause = latr_translate(env, addr, access, chunk, &host, &phys);
        if (cause != 0) {
            if (write_locked) {
                reservation_unlock(write_epoch);
            }
            env->mem_fault_cause = cause;
            env->mem_va = addr;
            return cause;
        }
        /*
         * The write domain is held across the RAM store only. A device call
         * (MMIO) must not run under it: a device that DMAs synchronously - and
         * every device write now takes the same domain - would deadlock on a
         * non-recursive lock. The emitted fast path already has this property;
         * this C path is the misaligned / page-crossing one.
         */
        if (env->mem_mmio) {
            if (is_load) {
                uint64_t part = latr_mmio_load(env, phys, chunk);
                if (env->mem_fault_cause) {
                    env->mem_va = addr;
                    return env->mem_fault_cause;
                }
                acc |= part << (8 * done);
            } else {
                latr_mmio_store(env, phys, chunk, value >> (8 * done));
                if (env->mem_fault_cause) {
                    env->mem_va = addr;
                    return env->mem_fault_cause;
                }
            }
        } else if (is_load) {
            for (i = 0; i < chunk; i++) {
                acc |= (uint64_t)((const uint8_t *)(uintptr_t)host)[i]
                       << (8 * (done + i));
            }
        } else {
            if (!write_locked) {
                write_epoch = reservation_lock();
                write_locked = true;
            }
            for (i = 0; i < chunk; i++) {
                ((uint8_t *)(uintptr_t)host)[i] =
                    (uint8_t)(value >> (8 * (done + i)));
            }
        }
        done += chunk;
    }
    if (write_locked) {
        reservation_unlock(write_epoch);
    }
    if (is_load) {
        *result = acc;
    }
    return 0;
}

int latr_guest_load(CPUArchState *env, uint64_t va, unsigned size,
                    uint64_t *value)
{
    if (size == 0 || size > 8) {
        env->mem_fault_cause = LATR_EXCP_LOAD_ACCESS_FAULT;
        env->mem_va = va;
        return env->mem_fault_cause;
    }
    return guest_access(env, va, size, LATR_ACCESS_LOAD, 0, true, value);
}

int latr_guest_store(CPUArchState *env, uint64_t va, unsigned size,
                     uint64_t value)
{
    uint64_t ignored = 0;

    if (size == 0 || size > 8) {
        env->mem_fault_cause = LATR_EXCP_STORE_ACCESS_FAULT;
        env->mem_va = va;
        return env->mem_fault_cause;
    }
    return guest_access(env, va, size, LATR_ACCESS_STORE, value, false,
                        &ignored);
}

uint64_t latr_cross_load(CPUArchState *env, uint64_t va, unsigned size)
{
    uint64_t value = 0;

    (void)latr_guest_load(env, va, size, &value);
    return value;
}

uint64_t latr_cross_store(CPUArchState *env, uint64_t va, unsigned size,
                          uint64_t value)
{
    (void)latr_guest_store(env, va, size, value);
    return 0;
}

/*
 * Cache the physical-memory resolution of the page that `pa` falls in, so the
 * next TLB hit on the same page skips loongemu_mem_resolve(). The plan records
 * the byte range [plan_page, plan_page + plan_span) that resolves identically:
 * it is clipped to the containing region and to the 4 KiB page, and an access
 * that would cross either edge simply misses and re-resolves.
 *
 * UNMAPPED and RESERVED are deliberately never planned: their verdict depends
 * on where the *next* region starts, which the lookup does not report.
 */
static void tlb_plan_fill(CPUArchState *env, LatrTlbEntry *e, uint64_t pa,
                          unsigned size)
{
    LoongemuMemLookup lookup;
    LoongemuMemKind kind = loongemu_mem_resolve(env->mem_map, pa, size, &lookup);
    uint64_t page_start;
    uint64_t lo;
    uint64_t hi;

    e->plan_valid = 0;
    e->plan_kind = (uint8_t)kind;
    e->plan_gen = env->mem_map->generation;
    e->plan_map = env->mem_map;
    if (kind != LOONGEMU_MEM_RAM && kind != LOONGEMU_MEM_ROM &&
        kind != LOONGEMU_MEM_MMIO) {
        return;
    }
    uint64_t region_start = pa - lookup.offset;
    uint64_t region_end = pa + lookup.avail;

    page_start = pa & ~UINT64_C(0xfff);
    lo = region_start > page_start ? region_start : page_start;
    hi = region_end < page_start + UINT64_C(0x1000)
             ? region_end
             : page_start + UINT64_C(0x1000);
    if (pa < lo || pa + size > hi) {
        return;
    }
    e->plan_page = lo;
    e->plan_span = hi - lo;
    e->plan_hpa = (kind == LOONGEMU_MEM_RAM || kind == LOONGEMU_MEM_ROM)
                      ? lookup.hpa - (pa - lo)
                      : 0;
    e->plan_valid = 1;
}

/*
 * The permission verdict for a TLB entry depends on who is asking (privilege,
 * mstatus.SUM/MXR) and on what for (load/store/fetch). `plan_ctx` records the
 * first half and `plan_ok_kinds` the kinds already validated under it, so the
 * fast path can prove "this entry was permission-checked for exactly this
 * request" from two bytes instead of re-running pte_permits().
 */
static uint8_t plan_ctx_key(const CPUArchState *env, unsigned access)
{
    uint64_t status = env->csr[CSR_MSTATUS_ADDR];
    unsigned privilege = effective_privilege(env, access);
    unsigned sum = (unsigned)((status >> MSTATUS_SUM_BIT) & 1u);
    unsigned mxr = (unsigned)((status >> MSTATUS_MXR_BIT) & 1u);

    return (uint8_t)((privilege & 3u) << 2 | sum << 1 | mxr);
}

static unsigned plan_kind_bit(unsigned access)
{
    if (access == LATR_ACCESS_STORE) {
        return 1u;
    }
    if (access == LATR_ACCESS_EXEC) {
        return 2u;
    }
    return 0u;
}

/* The walker just accepted this access for this entry: record it. */
static void tlb_plan_note_access(CPUArchState *env, LatrTlbEntry *e,
                                 unsigned access)
{
    uint8_t ctx = plan_ctx_key(env, access);

    if (e->plan_ctx != ctx) {
        e->plan_ctx = ctx;
        e->plan_ok_kinds = 0;
    }
    e->plan_ok_kinds |= (uint8_t)(1u << plan_kind_bit(access));
}

/*
 * Serve an access straight from the cached plan, without the walker.
 *
 * Every check here corresponds to something latr_walk() would have done:
 *   - pending SFENCE/FENCE.I or an unapplied flush  -> let the walker apply it
 *   - satp / va_page mismatch                       -> different translation
 *   - plan map/generation mismatch                  -> region set changed
 *   - plan_ctx / kind not validated                 -> permission not proven
 *   - PTE no longer equals the snapshot             -> unsynchronised edit
 *   - A/D not set                                   -> walker updates + refills
 *   - PMP                                           -> always re-evaluated
 *
 * Returns 0 when it served the access, a fault cause (> 0) when it decided a
 * fault, and -1 when the caller must take the normal path.
 */
static int latr_fast_access(CPUArchState *env, uint64_t va, unsigned access,
                            unsigned size, uint64_t *host, uint64_t *phys)
{
    LatrTlbEntry *e;
    uint64_t pte;
    uint64_t required;
    uint64_t pa;
    int pmp;

    if (env->mem_map == NULL) {
        return -1;
    }
    if (env->sfence_pending || env->fence_i_pending ||
        env->tlb_flush_seq != env->tlb_flush_seen) {
        return -1;
    }
    e = &env->tlb[(va >> 12) & (LATR_TLB_ENTRIES - 1)];
    if (!e->valid || e->va_page != (va >> 12) ||
        e->satp != env->csr[CSR_SATP_ADDR]) {
        return -1;
    }
    if (!e->plan_valid || e->plan_map != env->mem_map ||
        e->plan_gen != env->mem_map->generation) {
        return -1;
    }
    if (e->plan_ctx != plan_ctx_key(env, access) ||
        !(e->plan_ok_kinds & (uint8_t)(1u << plan_kind_bit(access)))) {
        return -1;
    }
    pte = *(volatile uint64_t *)(uintptr_t)e->pte_host;
    if (pte != e->pte) {
        return -1;
    }
    required = LATR_PTE_A;
    if (access == LATR_ACCESS_STORE) {
        required |= LATR_PTE_D;
    }
    if ((pte & required) != required) {
        return -1;
    }
    pa = e->pa_page | (va & e->page_mask);
    if (pa < e->plan_page || pa + size > e->plan_page + e->plan_span) {
        return -1;
    }
    pmp = latr_pmp_check(env, pa, size, access,
                         effective_privilege(env, access));
    if (pmp != 0) {
        return pmp;
    }
    if (phys != NULL) {
        *phys = pa;
    }
    switch ((LoongemuMemKind)e->plan_kind) {
    case LOONGEMU_MEM_RAM:
        *host = e->plan_hpa + (pa - e->plan_page);
        loongemu_perf_count(LOONGEMU_PERF_FAST_HIT, 1);
        return 0;
    case LOONGEMU_MEM_ROM:
        if (access == LATR_ACCESS_STORE) {
            return LATR_EXCP_STORE_ACCESS_FAULT;
        }
        *host = e->plan_hpa + (pa - e->plan_page);
        loongemu_perf_count(LOONGEMU_PERF_FAST_HIT, 1);
        return 0;
    case LOONGEMU_MEM_MMIO:
        env->mem_mmio = 1;
        *host = pa;
        loongemu_perf_count(LOONGEMU_PERF_FAST_HIT, 1);
        return 0;
    default:
        return -1;
    }
}

int latr_translate(CPUArchState *env, uint64_t va, unsigned access,
                   unsigned size, uint64_t *host, uint64_t *phys)
{
    uint64_t pa = 0;
    int cause;

    env->mem_mmio = 0;
    cause = latr_fast_access(env, va, access, size, host, phys);
    if (cause == 0) {
        return 0;
    }
    if (cause > 0) {
        loongemu_perf_count(LOONGEMU_PERF_MEM_FAULT, 1);
        return cause;
    }
    cause = latr_walk(env, va, access, &pa);
    if (cause != 0) {
        /* TLB permission faults are already counted at tlb_lookup; this
         * catches what the walk itself rejects. */
        loongemu_perf_count(LOONGEMU_PERF_MEM_FAULT, 1);
        return cause;
    }
    {
        /* The walker validated this access for the entry it filled: remember
         * the context so the next identical access can take the fast path. */
        LatrTlbEntry *e = &env->tlb[(va >> 12) & (LATR_TLB_ENTRIES - 1)];

        if (e->valid && e->va_page == (va >> 12)) {
            tlb_plan_note_access(env, e, access);
        }
    }
    cause = latr_pmp_check(env, pa, size, access,
                           effective_privilege(env, access));
    if (cause != 0) {
        return cause;
    }
    if (phys != NULL) {
        *phys = pa;
    }
    if (env->mem_map == NULL) {
        *host = pa;
        return 0;
    }

    {
        LatrTlbEntry *e = &env->tlb[(va >> 12) & (LATR_TLB_ENTRIES - 1)];

        if (e->valid && e->va_page == (va >> 12) && e->plan_valid &&
            e->plan_map == env->mem_map &&
            e->plan_gen == env->mem_map->generation &&
            pa >= e->plan_page && pa + size <= e->plan_page + e->plan_span) {
            loongemu_perf_count(LOONGEMU_PERF_PLAN_HIT, 1);
            switch ((LoongemuMemKind)e->plan_kind) {
            case LOONGEMU_MEM_RAM:
                *host = e->plan_hpa + (pa - e->plan_page);
                return 0;
            case LOONGEMU_MEM_ROM:
                if (access == LATR_ACCESS_STORE) {
                    return LATR_EXCP_STORE_ACCESS_FAULT;
                }
                *host = e->plan_hpa + (pa - e->plan_page);
                return 0;
            case LOONGEMU_MEM_MMIO:
                env->mem_mmio = 1;
                loongemu_perf_count(LOONGEMU_PERF_MMIO_ACCESS, 1);
                *host = pa;
                return 0;
            default:
                break; /* not planned; fall through and re-resolve */
            }
        }
        tlb_plan_fill(env, e, pa, size);
    }

    LoongemuMemLookup lookup;
    LoongemuMemKind kind = loongemu_mem_resolve(env->mem_map, pa, size, &lookup);

    switch (kind) {
    case LOONGEMU_MEM_RAM:
        *host = lookup.hpa;
        return 0;
    case LOONGEMU_MEM_ROM:
        if (access == LATR_ACCESS_STORE) {
            return LATR_EXCP_STORE_ACCESS_FAULT;
        }
        *host = lookup.hpa;
        return 0;
    case LOONGEMU_MEM_MMIO:
        /* The emitted path must call the MMIO helpers; `host` carries the
         * guest physical address and must never be dereferenced. */
        env->mem_mmio = 1;
        loongemu_perf_count(LOONGEMU_PERF_MMIO_ACCESS, 1);
        *host = pa;
        return 0;
    default:
        /* UNMAPPED / RESERVED: never alias host memory or VMM state. */
        return access_fault_cause(access);
    }
}

uint64_t latr_resolve_guest_address(CPUArchState *env, uint64_t va,
                                    unsigned access, unsigned size)
{
    uint64_t host = 0;
    uint64_t phys = 0;
#ifdef LOONGEMU_PERF
    /* Every guest load/store the emitter cannot serve inline lands here, so
     * this counter over guest_insns is the helper-per-instruction ratio. */
    loongemu_perf_count(LOONGEMU_PERF_HELPER_CALLS, 1);
#endif
    int cause = latr_translate(env, va, access, size, &host, &phys);

    env->mem_fault_cause = cause ? (uint64_t)cause : 0;
    env->mem_phys = phys;
    return cause ? 0 : host;
}
