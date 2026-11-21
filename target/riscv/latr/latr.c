#include "latr.h"
#include "translate.h"

static __thread TRANSLATION_DATA tr_data_real;
static __thread ENV lsenv_real;
__thread TRANSLATION_DATA *tr_data;
__thread ENV *lsenv;
bool latr_dump;

const int latr_la_map[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};
const int latr_la_fmap[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

uint16_t latr_guest_lduw(uint64_t address)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

uint32_t latr_guest_ldl(uint64_t address)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void latr_tr_data_init(CPUArchState *env)
{
    lsenv = &lsenv_real;
    lsenv_real.cpu_state = env;
    lsenv_real.tr_data = &tr_data_real;
    tr_data = &tr_data_real;
    env_ir2_opnd = ir2_opnd_new(IR2_OPND_GPR, la_s8);
}

void tr_init(void *tb)
{
    TRANSLATION_DATA *data = lsenv->tr_data;
    data->curr_tb = tb;
    ra_free_all();
    if (data->ir2_inst_array == NULL) {
        data->ir2_inst_array = mm_calloc(400, sizeof(IR2_INST));
        lsassertm(data->ir2_inst_array != NULL, "cannot allocate LATR IR2 array");
        data->ir2_inst_num_max = 400;
    }
    data->ir2_inst_num_current = 0;
    data->real_ir2_inst_num = 0;
    data->first_ir2 = NULL;
    data->last_ir2 = NULL;
    data->label_num = 0;
}

void tr_fini(void)
{
    TRANSLATION_DATA *data = lsenv->tr_data;
    ra_free_all();
    data->ir2_inst_num_current = 0;
    data->real_ir2_inst_num = 0;
    data->first_ir2 = NULL;
    data->last_ir2 = NULL;
    data->label_num = 0;
}

IR2_OPND alloc_gpr_src(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V source register %d", reg);
    if (reg == 0) {
        return zero_ir2_opnd;
    }
    IR2_OPND value = ra_alloc_itemp();
    la_ld_d(value, env_ir2_opnd, env_offset_gpr(reg));
    return value;
}

IR2_OPND alloc_gpr_dst(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V destination register %d", reg);
    return reg == 0 ? zero_ir2_opnd : ra_alloc_itemp();
}

void store_gpr_dst(int reg, IR2_OPND value)
{
    if (reg != 0) {
        la_st_d(value, env_ir2_opnd, env_offset_gpr(reg));
    }
}

void free_alloc_gpr(IR2_OPND value)
{
    if (ir2_opnd_is_itemp(&value)) {
        ra_free_itemp(value._reg_num);
    }
}

IR2_OPND alloc_fpr_src(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V FP source register %d", reg);
    IR2_OPND value = ra_alloc_ftemp();
    la_fld_d(value, env_ir2_opnd, env_offset_fpr(reg));
    return value;
}

IR2_OPND alloc_fpr_dst(int reg)
{
    lsassertm(reg >= 0 && reg < 32, "invalid RISC-V FP destination register %d", reg);
    return ra_alloc_ftemp();
}

void store_fpr_dst(int reg, IR2_OPND value)
{
    la_fst_d(value, env_ir2_opnd, env_offset_fpr(reg));
}

void free_alloc_fpr(IR2_OPND value)
{
    if (ir2_opnd_is_ftemp(&value)) {
        ra_free_ftemp(value._reg_num);
    }
}

static int find_pc(const RiscvIR1 *insns, int count, uint64_t pc)
{
    for (int i = 0; i <= count; i++) {
        uint64_t candidate = i == count ? insns[count - 1].pc + insns[count - 1].raw_len : insns[i].pc;
        if (candidate == pc) {
            return i;
        }
    }
    return -1;
}

static bool has_direct_target(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_JAL:
    case LATR_RV_BEQ:
    case LATR_RV_BNE:
    case LATR_RV_BLT:
    case LATR_RV_BGE:
    case LATR_RV_BLTU:
    case LATR_RV_BGEU:
        return true;
    default:
        return false;
    }
}

static void store_pc_constant(uint64_t pc)
{
    IR2_OPND value = ra_alloc_itemp();
    li_d(value, (int64_t)pc);
    la_st_d(value, env_ir2_opnd, env_offset_pc());
    ra_free_temp(value);
}

bool tr_ir2_generate(TranslationBlock *tb)
{
    if (tb->icount == 0 || tb->icount > TCG_MAX_INSNS) {
        return false;
    }

    RiscvIR1 insns[TCG_MAX_INSNS];
    IR2_OPND labels[TCG_MAX_INSNS + 1];
    struct {
        uint64_t pc;
        IR2_OPND label;
    } exits[TCG_MAX_INSNS];
    int exit_count = 0;
    int count = tb->icount;

    uint64_t pc = tb->pc;
    for (int i = 0; i < count; i++) {
        uint16_t half = latr_guest_lduw(pc);
        uint32_t raw = (half & 3) == 3 ? latr_guest_ldl(pc) : half;
        if (!latr_decode(pc, raw, &insns[i])) {
            return false;
        }
        pc += insns[i].raw_len;
    }

    for (int i = 0; i <= count; i++) {
        labels[i] = ir2_opnd_new_type(IR2_OPND_LABEL);
    }

    for (int i = 0; i < count; i++) {
        if (!has_direct_target(insns[i].opcode)) {
            continue;
        }
        uint64_t target = insns[i].pc + (int64_t)insns[i].arg.imm;
        int target_index = find_pc(insns, count, target);
        if (target_index > i) {
            continue;
        }
        int j;
        for (j = 0; j < exit_count; j++) {
            if (exits[j].pc == target) {
                break;
            }
        }
        if (j == exit_count) {
            exits[j].pc = target;
            exits[j].label = ir2_opnd_new_type(IR2_OPND_LABEL);
            exit_count++;
        }
    }

    for (int i = 0; i < count; i++) {
        LatrTranslateContext ctx = { .ir1 = &insns[i] };
        if (has_direct_target(insns[i].opcode)) {
            uint64_t target = insns[i].pc + (int64_t)insns[i].arg.imm;
            int target_index = find_pc(insns, count, target);
            if (target_index > i) {
                ctx.target_label = labels[target_index];
            } else {
                int j;
                for (j = 0; j < exit_count; j++) {
                    if (exits[j].pc == target) {
                        break;
                    }
                }
                ctx.target_label = exits[j].label;
            }
            ctx.has_target_label = true;
        }
        la_label(labels[i]);
        if (!latr_translate_ir1(&ctx)) {
            return false;
        }
    }

    la_label(labels[count]);
    store_pc_constant(pc);
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);

    for (int i = 0; i < exit_count; i++) {
        la_label(exits[i].label);
        store_pc_constant(exits[i].pc);
        la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    }
    return true;
}

void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu)
{
    (void)cpu;
    int limit = max_insns == NULL ? TCG_MAX_INSNS : *max_insns;
    if (limit < 0) {
        limit = 0;
    } else if (limit > TCG_MAX_INSNS) {
        limit = TCG_MAX_INSNS;
    }

    uint64_t pc = tb->pc;
    int count = 0;
    while (count < limit) {
        uint16_t half = latr_guest_lduw(pc);
        uint32_t raw = (half & 3) == 3 ? latr_guest_ldl(pc) : half;
        RiscvIR1 ir1;
        if (!latr_decode(pc, raw, &ir1)) {
            break;
        }
        pc += ir1.raw_len;
        count++;
    }
    tb->icount = count;
    tb->size = (uint16_t)(pc - tb->pc);
    if (max_insns != NULL) {
        *max_insns = count;
    }
}

void label_dispose(void)
{
    int label_count = lsenv->tr_data->label_num;
    int positions[label_count + 1];
    for (int i = 0; i <= label_count; i++) {
        positions[i] = -1;
    }

    int ir2_index = 0;
    for (IR2_INST *inst = lsenv->tr_data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        if (inst->_opcode == LISA_LABEL) {
            int id = ir2_opnd_label_id(&inst->_opnd[0]);
            lsassertm(id > 0 && id <= label_count, "invalid LATR label %d", id);
            lsassertm(positions[id] == -1, "duplicate LATR label %d", id);
            positions[id] = ir2_index;
        } else {
            ir2_index++;
        }
    }

    ir2_index = 0;
    for (IR2_INST *inst = lsenv->tr_data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        IR2_OPND *target = NULL;
        IR2_OPCODE opcode = ir2_opcode(inst);
        if (opcode == LISA_B || opcode == LISA_BL) {
            target = &inst->_opnd[0];
        } else if (ir2_opcode_is_branch_with_3opnds(opcode)) {
            target = &inst->_opnd[2];
        } else if (ir2_opcode_is_branch_with_2opnds(opcode) || ir2_opcode_is_f_branch(opcode)) {
            target = &inst->_opnd[1];
        }
        if (target != NULL && ir2_opnd_is_label(target)) {
            int id = ir2_opnd_label_id(target);
            lsassertm(id > 0 && id <= label_count && positions[id] >= 0,
                      "unresolved LATR label %d", id);
            ir2_opnd_convert_label_to_imm(target, positions[id] - ir2_index);
        }
        if (opcode != LISA_LABEL) {
            ir2_index++;
        }
    }
}

int tr_ir2_assemble(const void *code_start_addr)
{
    label_dispose();
    uint32_t *code = (uint32_t *)code_start_addr;
    int count = 0;
    for (IR2_INST *inst = lsenv->tr_data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        if (ir2_opcode(inst) == LISA_LABEL) {
            continue;
        }
        code[count++] = ir2_assemble(inst);
    }
    return count;
}
