#ifndef LATR_MMU_H
#define LATR_MMU_H

#include <stdbool.h>
#include <stdint.h>
#include "riscv-env.h"

/* Access kinds for the walker; the numeric values are LATR-private. */
enum {
    LATR_ACCESS_LOAD = 0,
    LATR_ACCESS_STORE = 1,
    LATR_ACCESS_EXEC = 2,
    LATR_ACCESS_NONE = 3, /* any other value falls back to load semantics */
};

/* PTE bits (Sv39/Sv48/Sv57). */
#define LATR_PTE_V   (UINT64_C(1) << 0)
#define LATR_PTE_R   (UINT64_C(1) << 1)
#define LATR_PTE_W   (UINT64_C(1) << 2)
#define LATR_PTE_X   (UINT64_C(1) << 3)
#define LATR_PTE_U   (UINT64_C(1) << 4)
#define LATR_PTE_G   (UINT64_C(1) << 5)
#define LATR_PTE_A   (UINT64_C(1) << 6)
#define LATR_PTE_D   (UINT64_C(1) << 7)
#define LATR_PTE_PPN_SHIFT 10
#define LATR_PTE_PPN_MASK  ((UINT64_C(1) << 44) - 1)

/* RISC-V exception causes produced by the walker / physical map. */
#define LATR_EXCP_INST_ACCESS_FAULT 1
#define LATR_EXCP_LOAD_ACCESS_FAULT 5
#define LATR_EXCP_STORE_ACCESS_FAULT 7
#define LATR_EXCP_INST_PAGE_FAULT 12
#define LATR_EXCP_LOAD_PAGE_FAULT 13
#define LATR_EXCP_STORE_PAGE_FAULT 15

/*
 * Walks the active satp page table for `va`. Returns 0 and the physical
 * address, or a page-fault cause. A/D bits are updated in memory (Svadu
 * style); Svpbmt is ignored and physical memory is identity-mapped to host
 * memory (no LoongemuMemMap yet). M-mode accesses bypass translation; MPRV is
 * not modelled.
 */
int latr_walk(CPUArchState *env, uint64_t va, unsigned access,
              uint64_t *pa_out);

/*
 * Full VA -> host translation: guest page walk, then the VMM physical map
 * when env->mem_map is set. Returns 0 on success or an exception cause.
 */
int latr_translate(CPUArchState *env, uint64_t va, unsigned access,
                   unsigned size, uint64_t *host, uint64_t *phys);

/* PMP enforcement used by the walker and by data/instruction accesses. */
int latr_pmp_check(const CPUArchState *env, uint64_t pa, unsigned size,
                   unsigned access, unsigned privilege);

/* Cross-4KiB-page scalar accesses: translate each page chunk separately. */
uint64_t latr_cross_load(CPUArchState *env, uint64_t va, unsigned size);
uint64_t latr_cross_store(CPUArchState *env, uint64_t va, unsigned size,
                          uint64_t value);

/* True when satp selects Sv39/Sv48/Sv57. */
bool latr_mmu_enabled(const CPUArchState *env);

/*
 * Runtime entry point for emitted code: returns the host address and records
 * `env->mem_phys` (for LR/SC reservations) plus `env->mem_fault_cause`
 * (0 = success). The cause field, not the returned address, is authoritative:
 * physical page 0 is a legal mapping. `size` is reserved for the future
 * MMIO/misaligned path.
 */
uint64_t latr_resolve_guest_address(CPUArchState *env, uint64_t va,
                                    unsigned access, unsigned size);

#endif
