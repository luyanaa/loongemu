#ifndef LATM_LLSC_H
#define LATM_LLSC_H

#include <stdint.h>

#define LATM_LLSC_MAX_CPUS 4u

/*
 * Shared LL/SC reservation table.
 *
 * The MIPS reservation is per hart (CP0 LLbit + LLAddr, kept on the CPU state),
 * but the *coherence* rule is not: a plain store by any hart to a reserved
 * physical range must drop every other hart's reservation, or a lock built out
 * of LL/SC lets two harts into the critical section. The per-CPU flag cannot see
 * another CPU's store, so the table lives next to the board, shared by all harts.
 */
typedef struct LatmLlscEntry {
    uint64_t pa;   /* physical address of the reservation */
    uint64_t size; /* 4 or 8, the LL width */
    int valid;
} LatmLlscEntry;

typedef struct LatmLlscTable {
    LatmLlscEntry res[LATM_LLSC_MAX_CPUS];
} LatmLlscTable;

void latm_llsc_init(LatmLlscTable *t);
/* Records hart `cpu`'s reservation (an LL just completed). */
void latm_llsc_reserve(LatmLlscTable *t, unsigned cpu, uint64_t pa,
                       unsigned size);
/* Drops hart `cpu`'s reservation (an SC finished, successful or not). */
void latm_llsc_clear(LatmLlscTable *t, unsigned cpu);
/*
 * A store by `cpu` covering [pa, pa+size) drops every reservation that overlaps
 * it - including `cpu`'s own, because a plain store between LL and SC must make
 * the SC fail. Overlap is range intersection, not equality: an 8-byte store over
 * a 4-byte reservation invalidates it.
 */
void latm_llsc_store(LatmLlscTable *t, unsigned cpu, uint64_t pa, unsigned size);
/* True when hart `cpu` still holds a reservation covering exactly `pa`/`size`. */
int latm_llsc_valid(const LatmLlscTable *t, unsigned cpu, uint64_t pa,
                    unsigned size);

#endif
