#include <stdint.h>

#include "ir2.h"

#if defined(LOONGEMU_IR2_PEEPHOLE) && LOONGEMU_IR2_PEEPHOLE

static IR2_INST *ir2_peephole_next(IR2_INST *inst_array, const IR2_INST *inst)
{
    return inst->_next < 0 ? NULL : &inst_array[inst->_next];
}

static IR2_OPCODE ir2_peephole_opcode(const IR2_INST *inst)
{
    return (IR2_OPCODE)inst->_opcode;
}

static bool ir2_peephole_opnd_equal(const IR2_OPND *left,
                                    const IR2_OPND *right)
{
    return left->_type == right->_type && left->val == right->val;
}

static bool ir2_peephole_is_branch(IR2_OPCODE opcode)
{
    return (opcode >= LISA_BEQZ && opcode <= LISA_BCNEZ) ||
           (opcode >= LISA_B && opcode <= LISA_BGEU);
}

static bool ir2_peephole_is_branch3(IR2_OPCODE opcode)
{
    return opcode >= LISA_BEQ && opcode <= LISA_BGEU;
}

static bool ir2_peephole_is_branch2(IR2_OPCODE opcode)
{
    return opcode >= LISA_BEQZ && opcode <= LISA_BNEZ;
}

static bool ir2_peephole_is_f_branch(IR2_OPCODE opcode)
{
    return opcode >= LISA_BCEQZ && opcode <= LISA_BCNEZ;
}

static bool ir2_peephole_is_boundary(IR2_OPCODE opcode)
{
    return ir2_opcode_is_fault_boundary(opcode) ||
           ir2_opcode_may_raise_fp_trap(opcode) ||
           ir2_opcode_clobbers_fcsr(opcode);
}

static bool ir2_peephole_is_redundant_move(const IR2_INST *inst)
{
    IR2_OPCODE opcode = ir2_peephole_opcode(inst);

    if (opcode == LISA_MOV64) {
        return inst->op_count >= 2 &&
               ir2_peephole_opnd_equal(&inst->_opnd[0], &inst->_opnd[1]);
    }
    if (opcode == LISA_OR) {
        return inst->op_count >= 3 &&
               ir2_peephole_opnd_equal(&inst->_opnd[0], &inst->_opnd[1]) &&
               inst->_opnd[2]._type == IR2_OPND_GPR &&
               inst->_opnd[2]._reg_num == 0;
    }
    if (opcode == LISA_FMOV_S || opcode == LISA_FMOV_D) {
        return inst->op_count >= 2 &&
               ir2_peephole_opnd_equal(&inst->_opnd[0], &inst->_opnd[1]);
    }
    return false;
}

static bool ir2_peephole_is_self_addi_d(const IR2_INST *inst)
{
    return ir2_peephole_opcode(inst) == LISA_ADDI_D && inst->op_count >= 3 &&
           inst->_opnd[0]._type == IR2_OPND_GPR &&
           ir2_peephole_opnd_equal(&inst->_opnd[0], &inst->_opnd[1]) &&
           inst->_opnd[2]._type == IR2_OPND_IMM;
}

static IR2_OPND *ir2_peephole_branch_target(IR2_INST *inst)
{
    IR2_OPCODE opcode = ir2_peephole_opcode(inst);

    if (opcode == LISA_BL) {
        return NULL;
    }
    if (opcode == LISA_B) {
        return &inst->_opnd[0];
    }
    if (ir2_peephole_is_branch3(opcode)) {
        return &inst->_opnd[2];
    }
    if (ir2_peephole_is_branch2(opcode) ||
        ir2_peephole_is_f_branch(opcode)) {
        return &inst->_opnd[1];
    }
    return NULL;
}

static bool ir2_peephole_branch_falls_through(IR2_INST *inst_array,
                                              IR2_INST *inst)
{
    IR2_OPND *target = ir2_peephole_branch_target(inst);

    if (target == NULL) {
        return false;
    }
    if (target->_type == IR2_OPND_IMM) {
        return target->_imm32 == 1;
    }
    if (target->_type != IR2_OPND_LABEL) {
        return false;
    }

    int label_id = target->_label_id;
    for (IR2_INST *next = ir2_peephole_next(inst_array, inst);
         next != NULL && ir2_peephole_opcode(next) == LISA_LABEL;
         next = ir2_peephole_next(inst_array, next)) {
        if (next->_opnd[0]._type == IR2_OPND_LABEL &&
            next->_opnd[0]._label_id == label_id) {
            return true;
        }
    }
    return false;
}

static bool ir2_peephole_is_env_ld_d(const IR2_INST *inst,
                                     const IR2_OPND *env_base)
{
    return ir2_peephole_opcode(inst) == LISA_LD_D && inst->op_count >= 3 &&
           ir2_peephole_opnd_equal(&inst->_opnd[1], env_base) &&
           inst->_opnd[2]._type == IR2_OPND_IMM;
}

static bool ir2_peephole_is_env_st_d(const IR2_INST *inst,
                                     const IR2_OPND *env_base)
{
    return ir2_peephole_opcode(inst) == LISA_ST_D && inst->op_count >= 3 &&
           ir2_peephole_opnd_equal(&inst->_opnd[1], env_base) &&
           inst->_opnd[2]._type == IR2_OPND_IMM;
}

static bool ir2_peephole_same_env_field(const IR2_INST *left,
                                        const IR2_INST *right)
{
    return ir2_peephole_opnd_equal(&left->_opnd[1], &right->_opnd[1]) &&
           ir2_peephole_opnd_equal(&left->_opnd[2], &right->_opnd[2]);
}

static void ir2_peephole_remove(IR2_INST *inst_array, IR2_INST **first,
                                IR2_INST **last, int *real_ir2_inst_num,
                                IR2_INST *inst)
{
    IR2_INST *next = ir2_peephole_next(inst_array, inst);
    IR2_INST *prev = inst->_prev < 0 ? NULL : &inst_array[inst->_prev];

    if (prev != NULL) {
        prev->_next = next != NULL ? next->_id : -1;
    } else {
        *first = next;
    }
    if (next != NULL) {
        next->_prev = prev != NULL ? prev->_id : -1;
    } else {
        *last = prev;
    }
    if (ir2_peephole_opcode(inst) >= LISA_GR2SCR) {
        (*real_ir2_inst_num)--;
    }
    inst->_prev = -1;
    inst->_next = -1;
}

int ir2_peephole_optimize(IR2_INST *inst_array, IR2_INST **first,
                          IR2_INST **last, int *real_ir2_inst_num,
                          IR2_OPND env_base)
{
    if (inst_array == NULL || first == NULL || last == NULL ||
        real_ir2_inst_num == NULL || *first == NULL) {
        return 0;
    }

    int removed = 0;
    uint32_t original_offset = 0;
    for (IR2_INST *node = *first; node != NULL;
         node = ir2_peephole_next(inst_array, node)) {
        node->_addr = original_offset;
        IR2_OPCODE opcode = ir2_peephole_opcode(node);
        if (opcode != LISA_LABEL && opcode != LISA_X86_INST) {
            original_offset += 4;
        }
    }
    IR2_INST *inst = *first;

    while (inst != NULL) {
        IR2_INST *next = ir2_peephole_next(inst_array, inst);
        IR2_OPCODE opcode = ir2_peephole_opcode(inst);

        if (ir2_peephole_is_branch(opcode) &&
            ir2_peephole_branch_falls_through(inst_array, inst)) {
            ir2_peephole_remove(inst_array, first, last,
                                real_ir2_inst_num, inst);
            removed++;
            inst = next;
            continue;
        }
        if (ir2_peephole_is_redundant_move(inst)) {
            ir2_peephole_remove(inst_array, first, last,
                                real_ir2_inst_num, inst);
            removed++;
            inst = next;
            continue;
        }

        if (ir2_peephole_is_self_addi_d(inst) && next != NULL &&
            !ir2_peephole_is_boundary(opcode) &&
            !ir2_peephole_is_boundary(ir2_peephole_opcode(next)) &&
            ir2_peephole_is_self_addi_d(next) &&
            ir2_peephole_opnd_equal(&inst->_opnd[0], &next->_opnd[0])) {
            int64_t sum = inst->_opnd[2]._imm32;
            bool merged = false;
            while (next != NULL &&
                   !ir2_peephole_is_boundary(ir2_peephole_opcode(next)) &&
                   ir2_peephole_is_self_addi_d(next) &&
                   ir2_peephole_opnd_equal(&inst->_opnd[0], &next->_opnd[0])) {
                int64_t next_imm = next->_opnd[2]._imm32;
                if (sum + next_imm < -2048 || sum + next_imm > 2047) {
                    break;
                }
                sum += next_imm;
                IR2_INST *after = ir2_peephole_next(inst_array, next);
                ir2_peephole_remove(inst_array, first, last,
                                    real_ir2_inst_num, next);
                removed++;
                merged = true;
                next = after;
            }
            if (merged) {
                inst->_opnd[2]._imm32 = (int32)sum;
            }
        }

        /* Env fields are accessed through the caller's trusted CPU-state
         * base. Keep this local: no instruction (and thus no unrelated fault,
         * FP-trap, or FCSR boundary) may lie between the equivalent accesses.
         * Retain the first access so its precise fault PC remains unchanged. */
        if (env_base._type == IR2_OPND_GPR && next != NULL) {
            if (ir2_peephole_is_env_ld_d(inst, &env_base) &&
                ir2_peephole_is_env_ld_d(next, &env_base) &&
                ir2_peephole_same_env_field(inst, next) &&
                ir2_peephole_opnd_equal(&inst->_opnd[0], &next->_opnd[0])) {
                IR2_INST *after = ir2_peephole_next(inst_array, next);
                ir2_peephole_remove(inst_array, first, last,
                                    real_ir2_inst_num, next);
                removed++;
                next = after;
            } else if (ir2_peephole_is_env_st_d(inst, &env_base) &&
                       ir2_peephole_is_env_st_d(next, &env_base) &&
                       ir2_peephole_same_env_field(inst, next) &&
                       ir2_peephole_opnd_equal(&inst->_opnd[0],
                                               &next->_opnd[0])) {
                IR2_INST *after = ir2_peephole_next(inst_array, next);
                ir2_peephole_remove(inst_array, first, last,
                                    real_ir2_inst_num, next);
                removed++;
                next = after;
            }
        }

        inst = next;
    }
    return removed;
}

#else
typedef int ir2_peephole_disabled_translation_unit;
#endif
