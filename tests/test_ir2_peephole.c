#include <assert.h>
#include <string.h>

#include "ir2.h"
#include "loongemu-perf.h"

static IR2_INST insts[32];
static int inst_count;
static int real_ir2_inst_num;
static IR2_INST *first;
static IR2_INST *last;

static IR2_OPND reg_opnd(int reg)
{
    IR2_OPND opnd = {0};
    opnd._type = IR2_OPND_GPR;
    opnd._reg_num = reg;
    return opnd;
}

static IR2_OPND imm_opnd(int value)
{
    IR2_OPND opnd = {0};
    opnd._type = IR2_OPND_IMM;
    opnd._imm32 = value;
    return opnd;
}

static IR2_OPND label_opnd(int label)
{
    IR2_OPND opnd = {0};
    opnd._type = IR2_OPND_LABEL;
    opnd._label_id = label;
    return opnd;
}

static void reset_stream(void)
{
    memset(insts, 0, sizeof(insts));
    inst_count = 0;
    real_ir2_inst_num = 0;
    first = NULL;
    last = NULL;
}

static IR2_INST *emit(IR2_OPCODE opcode, int op_count,
                      IR2_OPND op0, IR2_OPND op1, IR2_OPND op2)
{
    assert(inst_count < (int)(sizeof(insts) / sizeof(insts[0])));
    IR2_INST *inst = &insts[inst_count];
    inst->_id = inst_count;
    inst->_opcode = (int16)opcode;
    inst->op_count = op_count;
    inst->_opnd[0] = op0;
    inst->_opnd[1] = op1;
    inst->_opnd[2] = op2;
    inst->_prev = last != NULL ? last->_id : -1;
    inst->_next = -1;
    if (last != NULL) {
        last->_next = inst->_id;
    } else {
        first = inst;
    }
    last = inst;
    if (opcode >= LISA_GR2SCR) {
        real_ir2_inst_num++;
    }
    inst_count++;
    return inst;
}

static IR2_INST *next_inst(const IR2_INST *inst)
{
    return inst->_next < 0 ? NULL : &insts[inst->_next];
}

static unsigned count_opcode(IR2_OPCODE opcode, int first_gpr)
{
    unsigned count = 0;

    for (IR2_INST *inst = first; inst != NULL; inst = next_inst(inst)) {
        if (inst->_opcode == (int16)opcode &&
            (first_gpr < 0 || inst->_opnd[0]._reg_num == first_gpr)) {
            count++;
        }
    }
    return count;
}

static void test_side_effect_metadata(void)
{
    assert(ir2_opcode_may_raise_fp_trap(LISA_FADD_S));
    assert(ir2_opcode_may_raise_fp_trap(LISA_FCVT_D_S));
    assert(!ir2_opcode_may_raise_fp_trap(LISA_ADD_D));
    assert(ir2_opcode_is_fault_boundary(LISA_LD_D));
    assert(ir2_opcode_is_fault_boundary(LISA_ST_D));
    assert(!ir2_opcode_is_fault_boundary(LISA_ADDI_D));
    assert(ir2_opcode_clobbers_fcsr(LISA_FCMP_COND_D));
    assert(ir2_opcode_clobbers_fcsr(LISA_MOVGR2FCSR));
    assert(!ir2_opcode_clobbers_fcsr(LISA_MOVFCSR2GR));
}

static void test_perf_counter(void)
{
    loongemu_perf_reset();
    loongemu_perf_record_tb(16, 4);
    assert(loongemu_perf_tb_count() == 1);
    assert(loongemu_perf_host_insns() == 16);
    assert(loongemu_perf_guest_insns() == 4);
    assert(loongemu_perf_host_insns_per_guest_insn() == 4.0);

    /* Guest memory-access events are counted per class and reset with the
     * rest of the perf state. */
    loongemu_perf_count(LOONGEMU_PERF_TLB_HIT, 3);
    loongemu_perf_count(LOONGEMU_PERF_TLB_HIT, 2);
    loongemu_perf_count(LOONGEMU_PERF_MMIO_ACCESS, 1);
    assert(loongemu_perf_read(LOONGEMU_PERF_TLB_HIT) == 5);
    assert(loongemu_perf_read(LOONGEMU_PERF_TLB_MISS) == 0);
    assert(loongemu_perf_read(LOONGEMU_PERF_MMIO_ACCESS) == 1);
    assert(loongemu_perf_read(LOONGEMU_PERF_COUNTERS) == 0);
    loongemu_perf_count(LOONGEMU_PERF_COUNTERS, 1);
    assert(loongemu_perf_read(LOONGEMU_PERF_TLB_HIT) == 5);
    loongemu_perf_reset();
    assert(loongemu_perf_read(LOONGEMU_PERF_TLB_HIT) == 0);
    assert(loongemu_perf_read(LOONGEMU_PERF_MMIO_ACCESS) == 0);
}

static void test_peephole(void)
{
    IR2_OPND none = {0};
    IR2_OPND zero = reg_opnd(0);
    IR2_OPND env = reg_opnd(20);

    reset_stream();
    emit(LISA_OR, 3, reg_opnd(5), reg_opnd(5), zero);
    emit(LISA_MOV64, 2, reg_opnd(4), reg_opnd(4), none);
    emit(LISA_ADDI_D, 3, reg_opnd(6), reg_opnd(6), imm_opnd(100));
    emit(LISA_ADDI_D, 3, reg_opnd(6), reg_opnd(6), imm_opnd(200));
    emit(LISA_ADDI_D, 3, reg_opnd(7), reg_opnd(7), imm_opnd(1));
    emit(LISA_LD_D, 3, reg_opnd(9), env, imm_opnd(16));
    emit(LISA_ADDI_D, 3, reg_opnd(7), reg_opnd(7), imm_opnd(2));
    emit(LISA_B, 1, label_opnd(1), none, none);
    emit(LISA_LABEL, 1, label_opnd(1), none, none);
    emit(LISA_BL, 1, label_opnd(2), none, none);
    emit(LISA_LABEL, 1, label_opnd(2), none, none);
    emit(LISA_LD_D, 3, reg_opnd(10), env, imm_opnd(8));
    emit(LISA_LD_D, 3, reg_opnd(10), env, imm_opnd(8));
    emit(LISA_ST_D, 3, reg_opnd(11), env, imm_opnd(24));
    emit(LISA_ST_D, 3, reg_opnd(11), env, imm_opnd(24));

    assert(ir2_peephole_optimize(insts, &first, &last,
                                 &real_ir2_inst_num, env) == 6);
    assert(count_opcode(LISA_OR, -1) == 0);
    assert(count_opcode(LISA_MOV64, -1) == 0);
    assert(count_opcode(LISA_ADDI_D, 6) == 1);
    assert(count_opcode(LISA_ADDI_D, 7) == 2);
    assert(count_opcode(LISA_LD_D, 9) == 1);
    assert(count_opcode(LISA_LD_D, 10) == 1);
    assert(count_opcode(LISA_ST_D, 11) == 1);
    assert(count_opcode(LISA_B, -1) == 0);
    assert(count_opcode(LISA_BL, -1) == 1);

    for (IR2_INST *inst = first; inst != NULL; inst = next_inst(inst)) {
        if (inst->_opcode == LISA_ADDI_D &&
            inst->_opnd[0]._reg_num == 6) {
            assert(inst->_opnd[2]._imm32 == 300);
        }
    }
}

int main(void)
{
    test_side_effect_metadata();
    test_perf_counter();
    test_peephole();
    return 0;
}
