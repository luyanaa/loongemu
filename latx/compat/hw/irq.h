#ifndef LATX_BARE_HW_IRQ_H
#define LATX_BARE_HW_IRQ_H
/* The bare core never wires device IRQ lines; the type only has to exist for
 * the copied helper prototypes. */
#include "qemu/osdep.h"
typedef struct IRQState *qemu_irq;
#endif
