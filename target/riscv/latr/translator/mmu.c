#include "latr.h"
#include "mmu.h"
#include "translate.h"
#include "loongemu/phys_mem.h"
#include "loongemu/mmio_bus.h"

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
#define SATP_MODE(v) ((unsigned)((v) >> 60))
#define SATP_ROOT(v) (((v) & LATR_PTE_PPN_MASK) << 12)

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

int latr_pmp_check(const CPUArchState *env, uint64_t pa, unsigned size,
                   unsigned access, unsigned privilege)
{
    uint64_t mseccfg = env->csr[CSR_MSECCFG];
    unsigned perm = access == LATR_ACCESS_STORE ? PMP_W
                  : access == LATR_ACCESS_EXEC ? PMP_X
                                               : PMP_R;
    unsigned i;
    int matched = 0;

    if (size == 0) {
        size = 1;
    }

    for (i = 0; i < 16; i++) {
        uint8_t cfg = pmp_cfg_byte(env, i);
        unsigned a = cfg & PMP_A;
        uint64_t lo = 0, hi = 0;
        int match = 0;

        if (a == 0) {
            continue;
        }
        if ((cfg & (PMP_R | PMP_W | PMP_X)) == 0) {
            continue;
        }
        if (a == 0x08) { /* TOR */
            lo = i == 0 ? 0 : (pmp_addr(env, i - 1) << 2);
            hi = pmp_addr(env, i) << 2;
        } else if (a == 0x10) { /* NA4 */
            lo = pmp_addr(env, i) << 2;
            hi = lo + 4;
        } else if (a == 0x18) { /* NAPOT, QEMU pmp_decode_napot */
            uint64_t nar = (pmp_addr(env, i) << 2) | 3u;
            lo = nar & (nar + 1);
            hi = (nar | (nar + 1)) + 1;
        }
        if (hi > lo && pa >= lo && pa + size <= hi) {
            matched = 1;
            if ((cfg & perm) == 0) {
                return pmp_access_fault(access);
            }
            break;
        }
    }

    if (matched) {
        return 0;
    }
    /* No matching rule: MMWP/MML deny; otherwise only M-mode succeeds. */
    if ((mseccfg & (UINT64_C(1) << 1)) || (mseccfg & 1u)) {
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

int latr_walk(CPUArchState *env, uint64_t va, unsigned access, uint64_t *pa_out)
{
    unsigned mode = SATP_MODE(env->csr[CSR_SATP_ADDR]);
    unsigned privilege = effective_privilege(env, access);
    int levels, va_bits;

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

    uint64_t table = SATP_ROOT(env->csr[CSR_SATP_ADDR]);
    uint64_t status = env->csr[CSR_MSTATUS_ADDR];
    bool sum = (status >> MSTATUS_SUM_BIT) & 1;
    bool mxr = (status >> MSTATUS_MXR_BIT) & 1;
    for (int level = levels - 1; level >= 0; level--) {
        uint64_t vpn = (va >> (12 + 9 * level)) & 0x1ff;
        uint64_t pte_addr = table + vpn * sizeof(uint64_t);
        uint64_t pte_host = 0;
        uint64_t pte;
        int pmp = latr_pmp_check(env, pte_addr, 8, LATR_ACCESS_LOAD,
                                 privilege);
        int host_cause;

        if (pmp != 0) {
            return pmp;
        }
        host_cause = pte_host_pointer(env, pte_addr, 0, &pte_host);
        if (host_cause != 0) {
            return host_cause;
        }
        pte = *(volatile uint64_t *)(uintptr_t)pte_host;
        uint64_t ppn = (pte >> LATR_PTE_PPN_SHIFT) & LATR_PTE_PPN_MASK;

        /* Invalid, or W without R, is a fault at every level. */
        if (!(pte & LATR_PTE_V) ||
            ((pte & (LATR_PTE_R | LATR_PTE_W)) == LATR_PTE_W)) {
            return fault_cause(access);
        }
        if ((pte & PTE_PBMT_MASK) != 0 &&
            !(env->csr[CSR_MENVCFG] & (UINT64_C(1) << MENVCFG_PBMTE_BIT))) {
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

        bool user_page = (pte & LATR_PTE_U) != 0;
        if (privilege == LATR_RISCV_PRIV_U) {
            if (!user_page) {
                return fault_cause(access);
            }
        } else {
            if (access == LATR_ACCESS_EXEC) {
                if (user_page) {
                    return fault_cause(access);
                }
            } else if (user_page && !sum) {
                return fault_cause(access);
            }
        }

        if (access == LATR_ACCESS_LOAD) {
            if (!(pte & LATR_PTE_R) && !(mxr && (pte & LATR_PTE_X))) {
                return fault_cause(access);
            }
        } else if (access == LATR_ACCESS_STORE) {
            if (!(pte & LATR_PTE_W)) {
                return fault_cause(access);
            }
        } else if (!(pte & LATR_PTE_X)) {
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
                    int write_cause = pte_host_pointer(env, pte_addr, 1,
                                                       &write_host);
                    volatile uint64_t *slot;
                    if (write_cause != 0) {
                        return write_cause;
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
            }
        }

        {
            uint64_t pa = (ppn << 12) |
                          (va & ((UINT64_C(1) << (12 + 9 * level)) - 1));
            int pmp = latr_pmp_check(env, pa, 1, access, privilege);
            if (pmp != 0) {
                return pmp;
            }
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

static uint64_t cross_access(CPUArchState *env, uint64_t va, unsigned size,
                             unsigned access, uint64_t value, int is_load)
{
    unsigned done = 0;

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
        if (cause) {
            env->mem_fault_cause = cause;
            env->mem_va = addr;
            return 0;
        }
        if (env->mem_mmio) {
            if (is_load) {
                uint64_t part = latr_mmio_load(env, phys, chunk);
                if (env->mem_fault_cause) {
                    env->mem_va = addr;
                    return 0;
                }
                value |= part << (8 * done);
            } else {
                latr_mmio_store(env, phys, chunk, value >> (8 * done));
                if (env->mem_fault_cause) {
                    env->mem_va = addr;
                    return 0;
                }
            }
        } else if (is_load) {
            for (i = 0; i < chunk; i++) {
                value |= (uint64_t)((const uint8_t *)(uintptr_t)host)[i]
                         << (8 * (done + i));
            }
        } else {
            for (i = 0; i < chunk; i++) {
                ((uint8_t *)(uintptr_t)host)[i] =
                    (uint8_t)(value >> (8 * (done + i)));
            }
        }
        done += chunk;
    }
    return value;
}

uint64_t latr_cross_load(CPUArchState *env, uint64_t va, unsigned size)
{
    return cross_access(env, va, size, LATR_ACCESS_LOAD, 0, 1);
}

uint64_t latr_cross_store(CPUArchState *env, uint64_t va, unsigned size,
                          uint64_t value)
{
    return cross_access(env, va, size, LATR_ACCESS_STORE, value, 0);
}

int latr_translate(CPUArchState *env, uint64_t va, unsigned access,
                   unsigned size, uint64_t *host, uint64_t *phys)
{
    uint64_t pa = 0;
    int cause;

    env->mem_mmio = 0;
    cause = latr_walk(env, va, access, &pa);
    if (cause != 0) {
        return cause;
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
    int cause = latr_translate(env, va, access, size, &host, &phys);

    env->mem_fault_cause = cause ? (uint64_t)cause : 0;
    env->mem_phys = phys;
    return cause ? 0 : host;
}
