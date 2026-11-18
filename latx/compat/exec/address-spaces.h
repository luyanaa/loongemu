#ifndef LATX_BARE_EXEC_ADDRESS_SPACES_H
#define LATX_BARE_EXEC_ADDRESS_SPACES_H
/* Port I/O through the QEMU address-space API: the bare build routes these to
 * platform hooks instead of a memory-region tree. */
#include "qemu/osdep.h"

typedef struct AddressSpace { int unused; } AddressSpace;
extern AddressSpace address_space_io;
extern AddressSpace address_space_memory;

uint8_t  address_space_ldub(AddressSpace *as, uint64_t addr, MemTxAttrs attrs, int *err);
uint16_t address_space_lduw(AddressSpace *as, uint64_t addr, MemTxAttrs attrs, int *err);
uint32_t address_space_ldl(AddressSpace *as, uint64_t addr, MemTxAttrs attrs, int *err);
void address_space_stb(AddressSpace *as, uint64_t addr, uint8_t v, MemTxAttrs attrs, int *err);
void address_space_stw(AddressSpace *as, uint64_t addr, uint16_t v, MemTxAttrs attrs, int *err);
void address_space_stl(AddressSpace *as, uint64_t addr, uint32_t v, MemTxAttrs attrs, int *err);
#endif
