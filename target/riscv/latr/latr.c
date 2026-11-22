#include "latr.h"
#include "translate.h"
#include "mmu.h"
#include "loongemu/riscv_sbi.h"
#include "loongemu/mmio_bus.h"

/* One context per vCPU. With LATR_ENABLE_TLS each host thread sees its own
 * bootstrap; without it (freestanding build) a single hart is bound at a
 * time, which is all the bare-metal harness needs. */
static LATR_TLS LatrTranslatorContext latr_bootstrap_context;
static LATR_TLS bool latr_bootstrap_ready;
LATR_TLS LatrTranslatorContext *latr_active_context;
LATR_TLS TRANSLATION_DATA *tr_data;
LATR_TLS ENV *lsenv;
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

static CPUArchState *latr_cpu_env(void)
{
    return (CPUArchState *)lsenv->cpu_state;
}

/* Fetches one 16-bit half through the page table and physical map. */
static bool latr_fetch16(uint64_t va, uint16_t *out, int *cause)
{
    CPUArchState *env = latr_cpu_env();
    uint64_t host = 0;
    uint64_t phys = 0;
    int fault = latr_translate(env, va, LATR_ACCESS_EXEC, 2, &host, &phys);

    if (fault != 0) {
        *cause = fault;
        return false;
    }
    if (env->mem_mmio) {
        /* Execute-in-place through an MMIO bus (pflash) must not dereference
         * the device address directly. */
        uint64_t value = 0;
        env->mem_mmio = 0;
        if (env->mmio == 0 ||
            loongemu_mmio_bus_read(env->mmio, phys, 2, &value) != 0) {
            *cause = LATR_EXCP_INST_ACCESS_FAULT;
            return false;
        }
        *out = (uint16_t)value;
        return true;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)host;
    *out = (uint16_t)(p[0] | (uint16_t)p[1] << 8);
    return true;
}

static void latr_record_fetch_fault(uint64_t va, int cause)
{
    CPUArchState *env = latr_cpu_env();

    env->fetch_faulted = 1;
    env->fetch_fault_va = va;
    env->fetch_fault_cause = (uint64_t)cause;
}

uint16_t latr_guest_lduw(uint64_t address)
{
    CPUArchState *env = latr_cpu_env();
    uint16_t half = 0;

    int cause = 0;

    env->fetch_faulted = 0;
    if (!latr_fetch16(address, &half, &cause)) {
        latr_record_fetch_fault(address, cause);
    }
    return half;
}

/* A 32-bit instruction may straddle two pages: each half walks on its own. */
uint32_t latr_guest_ldl(uint64_t address)
{
    CPUArchState *env = latr_cpu_env();
    uint16_t lo = 0;
    uint16_t hi = 0;

    int cause = 0;

    env->fetch_faulted = 0;
    if (!latr_fetch16(address, &lo, &cause)) {
        latr_record_fetch_fault(address, cause);
        return 0;
    }
    if (!latr_fetch16(address + 2, &hi, &cause)) {
        latr_record_fetch_fault(address + 2, cause);
        return 0;
    }
    return (uint32_t)lo | (uint32_t)hi << 16;
}

LatrTranslatorContext *latr_context_create(CPUArchState *cpu,
                                           struct LatrTbCache *tb_cache,
                                           uint8_t *code_buf,
                                           uint32_t code_buf_size)
{
    LatrTranslatorContext *ctx = mm_calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->pvir_cache = latr_pvir_cache_create();
    if (ctx->pvir_cache == NULL) {
        mm_free(ctx);
        return NULL;
    }
    ctx->cpu = cpu;
    ctx->tb_cache = tb_cache;
    ctx->code_buf = code_buf;
    ctx->code_buf_size = code_buf_size;
    ctx->env.cpu_state = cpu;
    ctx->env.tr_data = &ctx->tr_data;
    return ctx;
}

void latr_context_destroy(LatrTranslatorContext *ctx)
{
    if (ctx == NULL || ctx == &latr_bootstrap_context) {
        return;
    }
    if (latr_active_context == ctx) {
        latr_active_context = NULL;
        tr_data = NULL;
        lsenv = NULL;
    }
    if (ctx->tr_data.ir2_inst_array != NULL) {
        mm_free(ctx->tr_data.ir2_inst_array);
    }
    latr_pvir_cache_destroy(ctx->pvir_cache);
    mm_free(ctx);
}

void latr_context_bind(LatrTranslatorContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    latr_active_context = ctx;
    lsenv = &ctx->env;
    tr_data = &ctx->tr_data;
    ctx->env.tr_data = &ctx->tr_data;
    ctx->env.cpu_state = ctx->cpu;
    env_ir2_opnd = ir2_opnd_new(IR2_OPND_GPR, la_s8);
}

LatrTranslatorContext *latr_context_current(void)
{
    return latr_active_context;
}

void latr_context_safepoint(LatrTranslatorContext *ctx)
{
    uint64_t generation;

    if (ctx == NULL) {
        return;
    }
    latr_tb_cache_safepoint(ctx->tb_cache);
    generation = ctx->tb_cache != NULL ? ctx->tb_cache->generation : 0;
    if (generation != ctx->pvir_generation) {
        latr_pvir_cache_reset(ctx->pvir_cache);
        ctx->pvir_generation = generation;
    }
}

void latr_tr_data_init(CPUArchState *env)
{
    LatrTranslatorContext *ctx = latr_active_context;

    if (ctx == NULL) {
        if (!latr_bootstrap_ready) {
            memset(&latr_bootstrap_context, 0,
                   sizeof(latr_bootstrap_context));
            latr_bootstrap_context.pvir_cache = latr_pvir_cache_create();
            latr_bootstrap_ready = true;
        }
        ctx = &latr_bootstrap_context;
        latr_context_bind(ctx);
    }
    ctx->cpu = env;
    lsenv->cpu_state = env;
    lsenv->tr_data = &ctx->tr_data;
    tr_data = &ctx->tr_data;
    env_ir2_opnd = ir2_opnd_new(IR2_OPND_GPR, la_s8);
}

void tr_init(void *tb)
{
    TRANSLATION_DATA *data = lsenv->tr_data;
    data->curr_tb = tb;
    ra_free_all();
    latr_pvir_cache_clear();
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

/*
 * VMM-side SBI entry: services an S-mode ECALL trap (mcause = 9 with
 * mstatus.MPP = S) and resumes the guest at mepc + 4. Returns false when the
 * pending trap is not an S-mode ECALL, so the caller can handle it normally.
 */
bool latr_service_sbi(CPUArchState *env, LoongemuSbi *sbi)
{
    unsigned cause = (unsigned)env->exception;
    unsigned mpp = (unsigned)((env->csr[0x300] >> 11) & 3);

    if (cause != 9 || mpp != LATR_RISCV_PRIV_S || sbi == NULL) {
        return false;
    }

    uint64_t args[6] = {
        env->gpr[10], env->gpr[11], env->gpr[12],
        env->gpr[13], env->gpr[14], env->gpr[15],
    };
    LoongemuSbiRet ret = loongemu_sbi_call(sbi, env->gpr[17], env->gpr[16],
                                           args);

    env->gpr[10] = (uint64_t)ret.error;
    env->gpr[11] = ret.value;
    env->pc = env->csr[0x341] + 4; /* mepc points at the ECALL */
    env->exception = 0;
    env->privilege = LATR_RISCV_PRIV_S;
    return true;
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

    TRANSLATION_DATA *data = lsenv->tr_data;

    uint64_t pc = tb->pc;
    for (int i = 0; i < count; i++) {
        uint16_t half = latr_guest_lduw(pc);
        uint32_t raw = (half & 3) == 3 ? latr_guest_ldl(pc) : half;
        data->tb_insn_pc[i] = pc;
        if (!latr_decode(pc, raw, &insns[i])) {
            if (latr_cpu_env()->fetch_faulted) {
                insns[i].opcode = LATR_RV_FETCH_FAULT;
                insns[i].fault_va = latr_cpu_env()->fetch_fault_va;
                insns[i].fault_cause = (int)latr_cpu_env()->fetch_fault_cause;
                insns[i].raw_len = 4;
            }
            data->tb_insn_len[i] = insns[i].raw_len;
            count = i + 1;
            pc += insns[i].raw_len;
            break;
        }
        data->tb_insn_len[i] = insns[i].raw_len;
        pc += insns[i].raw_len;
    }
    data->tb_insn_count = count;

    for (int i = 0; i <= count; i++) {
        labels[i] = ir2_opnd_new_type(IR2_OPND_LABEL);
        data->tb_insn_label[i] = ir2_opnd_label_id(&labels[i]);
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

static bool tb_terminator(LatrRiscvOpcode opcode)
{
    switch (opcode) {
    case LATR_RV_JAL:
    case LATR_RV_JALR:
    case LATR_RV_BEQ:
    case LATR_RV_BNE:
    case LATR_RV_BLT:
    case LATR_RV_BGE:
    case LATR_RV_BLTU:
    case LATR_RV_BGEU:
    case LATR_RV_MRET:
    case LATR_RV_SRET:
    case LATR_RV_ECALL:
    case LATR_RV_EBREAK:
    case LATR_RV_FENCE_I:
    case LATR_RV_VSETVLI:
    case LATR_RV_VSETVL:
    case LATR_RV_VSETIVLI:
        return true;
    default:
        return false;
    }
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
    bool allow_flow = (tb->cflags & LATR_TB_CFLAGS_ALLOW_FLOW) != 0;

    while (count < limit) {
        uint16_t half = latr_guest_lduw(pc);
        uint32_t raw = (half & 3) == 3 ? latr_guest_ldl(pc) : half;
        RiscvIR1 ir1;
        bool decoded = latr_decode(pc, raw, &ir1);
        uint32_t raw_len = decoded ? (uint32_t)ir1.raw_len : 2;

        if (!decoded || ir1.raw_len <= 0) {
            pc += 2;
            count++;
            break;
        }
        /* Only the first instruction may cross a 4 KiB page: later
         * instructions start a new TB at the boundary. */
        if (count > 0 &&
            ((pc ^ (pc + raw_len - 1)) & ~UINT64_C(0xfff)) != 0) {
            break;
        }
        pc += raw_len;
        count++;
        if (!allow_flow && tb_terminator(ir1.opcode)) {
            break;
        }
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
    TRANSLATION_DATA *data = lsenv->tr_data;
    uint32_t *code = (uint32_t *)code_start_addr;
    int count = 0;
    uint32_t span_off[LATR_TB_MAP_SPANS + 1];

    for (int i = 0; i <= data->tb_insn_count && i <= LATR_TB_MAP_SPANS; i++) {
        span_off[i] = 0;
    }
    for (IR2_INST *inst = data->first_ir2; inst != NULL; inst = ir2_next(inst)) {
        if (ir2_opcode(inst) == LISA_LABEL) {
            int id = ir2_opnd_label_id(&inst->_opnd[0]);
            for (int i = 0; i <= data->tb_insn_count; i++) {
                if (data->tb_insn_label[i] == id) {
                    span_off[i] = (uint32_t)(count * 4);
                }
            }
            continue;
        }
        code[count++] = ir2_assemble(inst);
    }

    latr_tb_insn_map_begin((void *)code_start_addr, (uint32_t)count * 4);
    for (int i = 0; i < data->tb_insn_count; i++) {
        latr_tb_insn_map_add(data->tb_insn_pc[i], span_off[i],
                             span_off[i + 1], data->tb_insn_len[i]);
    }
    return count;
}
