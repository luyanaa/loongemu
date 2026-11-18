#ifndef LOONGEMU_LOONGBT_IR2_OPCODES_H
#define LOONGEMU_LOONGBT_IR2_OPCODES_H
/* The LATX generator emits ir2-opcode.h as an enumerator list, not a header.
 * Wrapping it here gives the portable core a stable name for every opcode.
 * Build with -Ibaremetal/gen/latx/include. */
typedef enum LoongbtIr2Opcode {
#include "ir2-opcode.h"
} LoongbtIr2Opcode;
#endif
