#ifndef LATR_TRANSLATE_H
#define LATR_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>
#include "ir1.h"
#include "ir2.h"

typedef struct LatrTranslateContext {
    const RiscvIR1 *ir1;
    IR2_OPND target_label;
    bool has_target_label;
} LatrTranslateContext;

bool latr_translate_ir1(LatrTranslateContext *ctx);
bool latr_translate_csr(const RiscvIR1 *ir1);
bool latr_translate_privileged(const RiscvIR1 *ir1);
void latr_emit_instret(void);
void latr_emit_store(IR2_OPND address, IR2_OPND value, unsigned size);
/* Environment accesses preserve correctness for fields outside signed 12-bit offsets. */
void latr_env_load_d(IR2_OPND dst, int offset);
void latr_env_store_d(IR2_OPND value, int offset);
/* Emit cause 2 with instruction bits in the privilege-appropriate trap-value CSR. */
void latr_raise_illegal_instruction(const RiscvIR1 *ir1);
bool latr_translate_fp(const RiscvIR1 *ir1);

#endif
