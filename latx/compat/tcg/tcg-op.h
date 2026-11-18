#ifndef LATX_BARE_TCG_OP_H
#define LATX_BARE_TCG_OP_H
#include "tcg/tcg.h"
/* The retained pipeline emits IR2 directly; the TCG op builders only needed to
 * exist for the QEMU-era prototypes that upstream headers still declare. */
void tcg_gen_op1(TCGOpcode, TCGArg);
void tcg_gen_op2(TCGOpcode, TCGArg, TCGArg);
void tcg_gen_op3(TCGOpcode, TCGArg, TCGArg, TCGArg);
void tcg_gen_op4(TCGOpcode, TCGArg, TCGArg, TCGArg, TCGArg);
void tcg_gen_op5(TCGOpcode, TCGArg, TCGArg, TCGArg, TCGArg, TCGArg);
void tcg_gen_op6(TCGOpcode, TCGArg, TCGArg, TCGArg, TCGArg, TCGArg, TCGArg);
#endif
