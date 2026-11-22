/*
 * RVV 1.0 operand legality.
 *
 * The vector register operands of an instruction are a register *group*, and
 * the group shape (EMUL) follows from SEW/LMUL and the instruction's own EEW.
 * Reading `reg + idx` past v31 must therefore never happen: a group that would
 * run off the end of the register file is an illegal instruction, not an
 * access to a different architectural register.
 *
 * The rules transcribed here are QEMU's (target/riscv/insn_trans/trans_rvv
 * .c.inc, `vext_check_*`). Keeping them identical is what makes the QEMU-based
 * differential test meaningful, so every helper below cites its counterpart.
 *
 * Timing: SEW and LMUL are pinned per translation block (LatrTbKey.vspec), so
 * this check runs at translation time and turns the offending instruction into
 * an illegal-instruction trap. `vl` and `vstart` are runtime values and are
 * validated by emitted compare gates instead; see latr_translate_rvv().
 */
#ifndef LATR_RVV_LEGALITY_H
#define LATR_RVV_LEGALITY_H

#include <stdbool.h>
#include <stdint.h>

#include "ir1.h"

typedef struct LatrVectorState {
    uint64_t vtype; /* architectural vtype, vill cleared (bits [9:0] used) */
    uint8_t vill;   /* non-zero: every vector instruction is illegal */
    uint8_t elen;   /* widest supported element width in bits (64 for LATR) */
    uint8_t xlen;   /* XLEN (64 for LATR) */
} LatrVectorState;

typedef enum LatrRvvLegality {
    LATR_RVV_LEGAL = 0,
    LATR_RVV_ILLEGAL_VILL,   /* vtype is illegal; QEMU vext_check_isa_ill */
    LATR_RVV_ILLEGAL_EMUL,   /* EMUL outside 1/8..8 */
    LATR_RVV_ILLEGAL_ALIGN,  /* group start not a multiple of EMUL */
    LATR_RVV_ILLEGAL_OVERLAP,/* destination overlaps a source it may not */
    LATR_RVV_ILLEGAL_MASK,   /* masked destination overlaps v0 */
    LATR_RVV_ILLEGAL_NF,     /* nf * EMUL > 8 or segment runs past v31 */
    LATR_RVV_ILLEGAL_ELEN,   /* width beyond ELEN/SEW support */
    LATR_RVV_ILLEGAL_INDEX,  /* EEW index legality (EEW=64 with XLEN=32) */
    LATR_RVV_ILLEGAL_COUNT
} LatrRvvLegality;

/*
 * Returns LATR_RVV_LEGAL when the instruction may execute with `state`, or the
 * first violated rule. Instructions outside the RVV opcode range (including
 * the not-yet-implemented families) are reported legal; decoding already
 * rejects anything LATR cannot execute.
 */
LatrRvvLegality latr_rvv_check_operands(const RiscvIR1 *insn,
                                        const LatrVectorState *state);

/* True when the instruction's execution depends on the rounding-mode CSR. */
bool latr_rvv_uses_frm(unsigned opcode);

/*
 * Whole-register transfer class.
 *
 * vmv<n>r.v, vl<n>re<eew>.v and vs<n>r.v move nf complete registers and ignore
 * vl/vtype entirely. `nf` is encoded in the instruction (rs1 holds nf-1 as a
 * unary value), so it comes from the opcode rather than from arg.nf, which the
 * decoder leaves at zero for the @r2 / @r2rd argument shapes.
 */
typedef enum LatrRvvWholeKind {
    LATR_RVV_WHOLE_NONE = 0,
    LATR_RVV_WHOLE_MOVE,  /* vmv<n>r.v:  vd, vs2 */
    LATR_RVV_WHOLE_LOAD,  /* vl<n>re<eew>.v: vd, (rs1) */
    LATR_RVV_WHOLE_STORE  /* vs<n>r.v:  vd(rs2 field), (rs1) */
} LatrRvvWholeKind;

LatrRvvWholeKind latr_rvv_whole_kind(unsigned opcode);

/* Registers in the transfer (1, 2, 4 or 8); 0 when not a whole-register op. */
unsigned latr_rvv_whole_nf(unsigned opcode);

/* log2 of the element width for whole-register loads/stores (0..3). */
unsigned latr_rvv_whole_eew(unsigned opcode);

/* True for the unit-stride fault-only-first loads (vle<eew>ff.v). */
bool latr_rvv_is_ldff(unsigned opcode);

const char *latr_rvv_legality_name(LatrRvvLegality why);

#endif
