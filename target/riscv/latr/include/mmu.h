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
 * address, or a fault cause. Implicit PTE reads and hardware A/D writes use
 * S-mode PMP checks. M-mode accesses bypass translation unless MPRV selects a
 * lower effective privilege.
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

/*
 * Unified guest-memory access used by scalar slow paths, RVV lanes and PVIR
 * lanes. The range is split at 4 KiB virtual page boundaries so every chunk
 * is translated, PMP-checked and (for MMIO) dispatched with its own mapping.
 * `size` is 1..8 and the value is assembled little-endian. Returns 0 on
 * success or an exception cause; on failure env->mem_fault_cause and
 * env->mem_va describe the faulting chunk.
 */
int latr_guest_load(CPUArchState *env, uint64_t va, unsigned size,
                    uint64_t *value);
int latr_guest_store(CPUArchState *env, uint64_t va, unsigned size,
                     uint64_t value);

/*
 * Shared reservation domain for LR/SC. Bit 0 is the writer lock; the upper
 * bits count completed guest writes. Emitted scalar stores and all C-level
 * memory paths use this same word, so a write through any path invalidates
 * outstanding reservations. Non-CPU writers (device DMA) call
 * latr_reservation_notify_write(). If a CPU writer already holds the lock the
 * notification returns immediately: that writer's release advances the epoch.
 */
extern volatile uint64_t latr_guest_store_lock;
void latr_reservation_notify_write(uint64_t pa, unsigned size);
/*
 * The same domain as a mutex, for writers that are not generated stores
 * (device DMA). Bit 0 is the writer lock and the upper bits carry the epoch, so
 * holding it excludes a CPU store and releasing it invalidates outstanding
 * LR/SC reservations - the store and its invalidation stay one critical
 * section. Device writes take it too, so a DMA cannot interleave with a CPU
 * store inside one guest-visible write.
 */
uint64_t latr_reservation_lock(void);
void latr_reservation_unlock(uint64_t epoch);

/* Emitted-code entry points for page-crossing scalar accesses. */
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
