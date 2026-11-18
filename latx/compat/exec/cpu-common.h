#ifndef LATX_BARE_EXEC_CPU_COMMON_H
#define LATX_BARE_EXEC_CPU_COMMON_H
/* Physical-RAM bookkeeping is owned by the platform layer, not by QEMU's
 * memory core; only the declarations the copied helpers reference are here. */
#include "qemu/osdep.h"
typedef struct RAMBlock { uint8_t *host; uint64_t max_length; } RAMBlock;
uint8_t *qemu_map_ram_ptr(RAMBlock *rb, uint64_t addr);
void *qemu_get_ram_ptr(uint64_t addr);
#endif
