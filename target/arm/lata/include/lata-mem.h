#ifndef LATA_MEM_H
#define LATA_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "target/arm/cpu.h"

/*
 * Unified guest memory contract shared by scalar, NEON, and SVE paths.
 *
 * `direct_range_stable()` decides whether a contiguous span [vaddr, vaddr+size)
 * may take an inline direct memory fast path without per-element helper probes.
 * Any uncertain, guarded, MMU-protected, or first-fault/non-fault lane routes
 * through `probe()` / `load()` / `store()`, which return an architectural status
 * so precise synchronous Data Aborts or suppressed FFR updates stay exact.
 */
typedef enum {
    LATA_MEM_OK = 0,
    LATA_MEM_FAULT_TRANSLATION = 1,
    LATA_MEM_FAULT_PERMISSION = 2,
    LATA_MEM_FAULT_ALIGNMENT = 3,
    LATA_MEM_FAULT_UNMAPPED = 4,
} LataGuestMemStatus;

#define LATA_MEM_PERM_NONE 0u
#define LATA_MEM_PERM_READ (1u << 0)
#define LATA_MEM_PERM_WRITE (1u << 1)
#define LATA_MEM_PERM_RW (LATA_MEM_PERM_READ | LATA_MEM_PERM_WRITE)

typedef struct LataGuestMemOps {
    LataGuestMemStatus (*probe)(CPUARMState *env, uint64_t vaddr,
                                size_t size, bool is_write, bool clean_tbi);
    LataGuestMemStatus (*load)(CPUARMState *env, uint64_t vaddr,
                               size_t size, bool is_signed,
                               uint64_t *out_value);
    LataGuestMemStatus (*store)(CPUARMState *env, uint64_t vaddr,
                                size_t size, uint64_t value);
    bool (*direct_range_stable)(CPUARMState *env, uint64_t vaddr,
                                size_t size, bool is_write);
} LataGuestMemOps;

extern const LataGuestMemOps lata_default_guest_mem_ops;

void lata_set_guest_mem_ops(const LataGuestMemOps *ops);
const LataGuestMemOps *lata_get_guest_mem_ops(void);

LataGuestMemStatus lata_guest_mem_probe(CPUARMState *env, uint64_t vaddr,
                                        size_t size, bool is_write,
                                        bool clean_tbi);
LataGuestMemStatus lata_guest_mem_load(CPUARMState *env, uint64_t vaddr,
                                       size_t size, bool is_signed,
                                       uint64_t *out_value);
LataGuestMemStatus lata_guest_mem_store(CPUARMState *env, uint64_t vaddr,
                                        size_t size, uint64_t value);
bool lata_guest_mem_direct_range_stable(CPUARMState *env, uint64_t vaddr,
                                        size_t size, bool is_write);

/* Configure test/MMU fault windows on env->sve_ctrl. */
void lata_guest_mem_clear_fault_regions(CPUARMState *env);
bool lata_guest_mem_add_fault_region(CPUARMState *env, uint64_t base,
                                     uint64_t size, uint32_t perm);

/* Helper entry points invoked by generated IR2 (returning 0 on OK, else LataGuestMemStatus). */
uint64_t lata_guest_mem_load_checked(CPUARMState *env, uint64_t vaddr,
                                     uint32_t size_and_flags, uint64_t *out_val);
uint64_t lata_guest_mem_store_checked(CPUARMState *env, uint64_t vaddr,
                                      uint32_t size_and_flags, uint64_t val);
void lata_guest_mem_raise_fault(CPUARMState *env, uint64_t fault_pc,
                                  uint64_t vaddr, uint32_t is_write,
                                  uint32_t status);

#endif
