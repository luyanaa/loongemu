#ifndef LATR_LATA_COMPAT_H
#define LATR_LATA_COMPAT_H

#include "latr.h"

/*
 * `target/arm/lata/ir2/ir2.c` is compiled into the LATR core as well. Its dump
 * path tags every line with an ir1 id kept in the LATA translation context, and
 * it is the only LataContext member the shared code touches. This shim exists so
 * that the shared file keeps building without pulling the ARM `lata.h` (which
 * this header shadows by name) and without giving LATR a real ARM context.
 *
 * The counter only groups IR2 dump output; it carries no architectural state.
 */
typedef struct LataContext {
    int ir2_dump_ir1_id;
} LataContext;

LataContext *lata_context_current(void);

#endif
