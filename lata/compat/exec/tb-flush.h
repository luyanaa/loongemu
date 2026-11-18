#ifndef LATA_TB_FLUSH_H
#define LATA_TB_FLUSH_H
#include "qemu/osdep.h"
#include "qemu/bswap.h"
typedef struct CPUState CPUState;
void tb_flush(CPUState *cpu);
#endif
