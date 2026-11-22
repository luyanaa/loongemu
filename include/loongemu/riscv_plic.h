#ifndef LOONGEMU_RISCV_PLIC_H
#define LOONGEMU_RISCV_PLIC_H

#include <stdint.h>
#include "loongemu/mmio_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SiFive PLIC v1.0 model (the classic RISC-V platform external interrupt
 * controller). MMIO register layout:
 *
 *   0x000000 + 4*source     priority (source 1..N; source 0 reserved)
 *   0x001000 + 4*(source/32) pending (read-only)
 *   0x002000 + ctx*0x80 + 4*(source/32) enable bits
 *   0x200000 + ctx*0x1000   priority threshold
 *   0x200000 + ctx*0x1000+4 claim (read) / complete (write)
 *
 * The model is a plain library so it is host-testable; the VMM routes guest
 * MMIO to loongemu_plic_read32/write32 and injects device lines through
 * loongemu_plic_set_irq.
 */

#define LOONGEMU_PLIC_MAX_SOURCES  96
#define LOONGEMU_PLIC_MAX_CONTEXTS 4
#define LOONGEMU_PLIC_MAX_PRIORITY 7

#define LOONGEMU_PLIC_PRIORITY_BASE 0x000000
#define LOONGEMU_PLIC_PENDING_BASE  0x001000
#define LOONGEMU_PLIC_ENABLE_BASE   0x002000
#define LOONGEMU_PLIC_ENABLE_STRIDE 0x80
#define LOONGEMU_PLIC_CONTEXT_BASE  0x200000
#define LOONGEMU_PLIC_CONTEXT_STRIDE 0x1000
#define LOONGEMU_PLIC_MMIO_SIZE     0x400000

typedef struct LoongemuPlic {
    uint32_t num_sources;  /* sources 1..num_sources; 0 reserved */
    uint32_t num_contexts;
    uint32_t priority[LOONGEMU_PLIC_MAX_SOURCES + 1];
    uint32_t pending[LOONGEMU_PLIC_MAX_SOURCES + 1]; /* bit per source */
    uint32_t level[LOONGEMU_PLIC_MAX_SOURCES + 1];   /* input line state */
    uint32_t claimed[LOONGEMU_PLIC_MAX_SOURCES + 1]; /* gateway claimed */
    uint32_t enable[LOONGEMU_PLIC_MAX_CONTEXTS]
                   [(LOONGEMU_PLIC_MAX_SOURCES + 31) / 32];
    uint32_t threshold[LOONGEMU_PLIC_MAX_CONTEXTS];
} LoongemuPlic;

/* Returns 1 on success, 0 for invalid counts. */
int loongemu_plic_init(LoongemuPlic *plic, uint32_t num_sources,
                       uint32_t num_contexts);

/* Sets the state of an interrupt input line (level-sensitive). */
void loongemu_plic_set_irq(LoongemuPlic *plic, uint32_t source, int level);

/*
 * True when `context` has a claimable interrupt: pending, enabled and with a
 * priority strictly above the context threshold.
 */
int loongemu_plic_pending(const LoongemuPlic *plic, uint32_t context);

/* MMIO accessors; `offset` is relative to the PLIC base. */
uint32_t loongemu_plic_read32(LoongemuPlic *plic, uint32_t offset,
                              int *error);
void loongemu_plic_write32(LoongemuPlic *plic, uint32_t offset,
                           uint32_t value, int *error);

/* Registers the PLIC at `base` on the MMIO bus (32-bit register accesses). */
int loongemu_plic_attach(LoongemuMmioBus *bus, LoongemuPlic *plic,
                         uint64_t base);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_RISCV_PLIC_H */
