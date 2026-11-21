#ifndef LATA_SVE_DECODE_H
#define LATA_SVE_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "ir1.h"
#include "ir1-arg.h"

typedef struct {
    AARCH64_INSN_TYPE_A64 insn_type;
    union a64_u arg;
} A64SVEIR1Instruction;

typedef enum {
    A64_SVE_DECODE_INVALID_ARGUMENT = -1,
    A64_SVE_DECODE_NO_MATCH,
    A64_SVE_DECODE_MATCH,
    /* Never route an unknown word in a known SVE prefix to legacy decoding. */
    A64_SVE_DECODE_UNSUPPORTED_ENCODING,
} A64SVEDecodeStatus;

/* Runtime access state is classified separately from opcode recognition. */
typedef enum {
    A64_SVE_CONTROL_UNMODELED,
    A64_SVE_CONTROL_ALLOWED,
    A64_SVE_CONTROL_DENIED,
} A64SVEControlState;

typedef enum {
    A64_SVE_ACCESS_FEATURE_ABSENT,
    A64_SVE_ACCESS_CONTROL_UNSUPPORTED,
    A64_SVE_ACCESS_ALLOWED,
    A64_SVE_ACCESS_TRAP_REQUIRED,
} A64SVEAccessDisposition;

/*
 * Decode supported SVE forms without consulting feature or access-control
 * state. A recognized word always produces its typed SVE IR1 opcode.
 */
A64SVEDecodeStatus a64_sve_decode(uint32_t insn,
                                  A64SVEIR1Instruction *decoded);

/*
 * Classify feature presence and a separately supplied architectural control
 * result. This is a pure classifier; it does not emit an architectural trap.
 */
A64SVEAccessDisposition
a64_sve_access_disposition(bool sve_implemented, A64SVEControlState state);

#endif
