/*
 * Shared bring-up plumbing for the bare harnesses: decode the next block and
 * make its exits usable without a runtime TB linker.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "ir1.h"
#include "ir2.h"
#include "translate.h"
#include "flag-reduction.h"
#include "latx-x86-paging.h"

#include "tb_harness.h"

static IR1_INST ir1_list[TB_HARNESS_MAX_INSNS];
static struct separated_data ir1_sd;
static uint8_t ir1_dt_area[131072] __attribute__((aligned(16)));

static const int control_flow_ops[] = {
    dt_X86_INS_JMP, dt_X86_INS_LJMP, dt_X86_INS_CALL, dt_X86_INS_RET, dt_X86_INS_RETF,
    dt_X86_INS_RETFQ, dt_X86_INS_SYSCALL, dt_X86_INS_SYSRET,
    dt_X86_INS_IRET, dt_X86_INS_IRETD,
    dt_X86_INS_IRETQ, dt_X86_INS_HLT, dt_X86_INS_INT, dt_X86_INS_INT3,
    dt_X86_INS_UD2, dt_X86_INS_VMCALL, dt_X86_INS_VMMCALL,
    dt_X86_INS_LOOP, dt_X86_INS_LOOPE, dt_X86_INS_LOOPNE,
    dt_X86_INS_JAE, dt_X86_INS_JA, dt_X86_INS_JBE, dt_X86_INS_JB,
    dt_X86_INS_JCXZ, dt_X86_INS_JECXZ, dt_X86_INS_JRCXZ, dt_X86_INS_JE,
    dt_X86_INS_JGE, dt_X86_INS_JG, dt_X86_INS_JLE, dt_X86_INS_JL,
    dt_X86_INS_JNE, dt_X86_INS_JNO, dt_X86_INS_JNP, dt_X86_INS_JNS,
    dt_X86_INS_JO, dt_X86_INS_JP, dt_X86_INS_JS,
};

static int is_control_flow(const IR1_INST *ir1)
{
    int op = ir1_opcode(ir1);

    for (unsigned i = 0; i < sizeof(control_flow_ops) / sizeof(int); i++) {
        if (op == control_flow_ops[i]) {
            return 1;
        }
    }
    return 0;
}

static uint8_t tb_harness_stack_access_kind(int opcode)
{
    switch (opcode) {
    case dt_X86_INS_PUSH:
        return TB_HARNESS_STACK_ACCESS_PUSH;
    case dt_X86_INS_POP:
        return TB_HARNESS_STACK_ACCESS_POP;
    case dt_X86_INS_CALL:
        return TB_HARNESS_STACK_ACCESS_PUSH;
    default:
        return TB_HARNESS_STACK_ACCESS_NONE;
    }
}

static void tb_harness_record_fault_ea(TbInsnMap *map, TbInsnSpan *span,
                                       IR1_INST *ir1)
{
    uint8_t stack_kind = tb_harness_stack_access_kind(ir1_opcode(ir1));
    struct la_dt_insn *info = ir1->info;
    int mem_index = -1;
    int mem_count = 0;

    if (stack_kind == TB_HARNESS_STACK_ACCESS_NONE ||
        span->ea_seg_class != IR1_EA_SEG_UNKNOWN || info == NULL ||
        (ir1_addr_size(ir1) != 32 && ir1_addr_size(ir1) != 64) ||
        map->fault_ea_count >= TB_HARNESS_MAX_INSNS) {
        return;
    }
    for (int i = 0; i < info->x86.op_count; i++) {
        if (info->x86.operands[i].type == dt_X86_OP_MEM) {
            mem_index = i;
            mem_count++;
        }
    }
    if (mem_count != 1) {
        return;
    }

    IR1_OPND *mem_opnd = ir1_get_opnd(ir1, mem_index);
    if (mem_opnd == NULL) {
        return;
    }

    int access_size = ir1_opnd_size(mem_opnd) / 8;
    int address_size = ir1_addr_size(ir1);
    int64_t displacement = mem_opnd->mem.disp;
    int base_reg = TB_HARNESS_FAULT_REG_NONE;
    int index_reg = TB_HARNESS_FAULT_REG_NONE;
    int scale = 1;
    uint8_t ea_class = IR1_EA_SEG_NOT_SS;

    if (access_size <= 0 || access_size > UINT8_MAX ||
        displacement < INT32_MIN || displacement > INT32_MAX) {
        return;
    }
    if (ir1_opnd_has_base(mem_opnd)) {
        dt_x86_reg reg = ir1_opnd_base_reg(mem_opnd);
        if (reg == dt_X86_REG_EIP
#ifdef TARGET_X86_64
            || reg == dt_X86_REG_RIP
#endif
        ) {
            base_reg = TB_HARNESS_FAULT_REG_RIP;
        } else {
            base_reg = ir1_opnd_base_reg_num(mem_opnd);
            if (base_reg < 0 || base_reg >= 16) {
                return;
            }
            if (base_reg == 4 || base_reg == 5) {
                ea_class = IR1_EA_SEG_SS;
            }
        }
    }
    if (ir1_opnd_has_index(mem_opnd)) {
        index_reg = ir1_opnd_index_reg_num(mem_opnd);
        scale = ir1_opnd_scale(mem_opnd);
        if (index_reg < 0 || index_reg >= 16 ||
            (scale != 1 && scale != 2 && scale != 4 && scale != 8)) {
            return;
        }
    }
    if (ir1->ea_segment_prefix == dt_X86_PREFIX_FS ||
        ir1->ea_segment_prefix == dt_X86_PREFIX_GS) {
        ea_class = IR1_EA_SEG_NOT_SS;
    }
    /* UNKNOWN here must be a real stack/data class conflict. */
    if (ea_class != IR1_EA_SEG_NOT_SS) {
        return;
    }

    TbFaultEa *ea = &map->fault_eas[map->fault_ea_count];
    memset(ea, 0, sizeof(*ea));
    ea->displacement = (int32_t)displacement;
    ea->base = (uint8_t)base_reg;
    ea->index = (uint8_t)index_reg;
    ea->scale = (uint8_t)scale;
    ea->address_size = (uint8_t)(address_size / 8);
    ea->size = (uint8_t)access_size;
    ea->segment_prefix = ir1->ea_segment_prefix;
    ea->ea_seg_class = ea_class;
    span->fault_ea_index = (uint8_t)map->fault_ea_count;
    span->stack_access_kind = stack_kind;
    span->stack_access_size = (uint8_t)access_size;
    map->fault_ea_count++;
}

/*
 * Translate the block starting at pc into the code cache and return the
 * number of bytes emitted.  No exit patch is needed beyond the link slots
 * below: with CPU_LOG_TB_NOCHAIN set the translator itself emits the jump to
 * the epilogue, which is the entry the harness calls through.
 */
#define TB_DECODE_MAX_INSNS TB_HARNESS_MAX_INSNS
#define TB_MAX_X86_INSN_LEN 15

int tb_harness_translate_at_with_map(uint64_t pc, uint8_t *code,
                                     uint64_t limit, TbInsnMap *out_map)
{
    TranslationBlock tb;
    int off = 0, n = 0;
    int rc;
    CPUX86State *cpu_env =
        (lsenv != 0) ? (CPUX86State *)lsenv->cpu_state : 0;


    memset(&tb, 0, sizeof(tb));
    memset(ir1_list, 0, sizeof(ir1_list));
    memset(&ir1_sd, 0, sizeof(ir1_sd));
    if (out_map != 0) {
        memset(out_map, 0, sizeof(*out_map));
    }

    {
        /*
         * Decode from a local copy: ir1_disasm reads a maximum instruction
         * (15 bytes) plus lookahead, and handing it a pointer into the guest
         * image makes the bounds of that read depend on where in the image
         * the block sits.
         *
         * The copy has to be big enough that every instruction it may decode
         * has all 15 of its bytes present.  With a 32-byte copy, a block whose
         * seventh instruction starts at offset 31 decodes that instruction
         * from a zero-filled tail -- silently, as a different instruction with
         * a different length -- and the rest of the block is shifted.  That is
         * how `mov cr3, eax` became a one-operand instruction and stopped
         * writing CR3.
         */
        CPUX86State *xenv = (lsenv != 0) ? (CPUX86State *)lsenv->cpu_state : 0;
        uint64_t fetch_base = (xenv != 0 && xenv->sys.codemode != LATX_DT_X64)
                                  ? xenv->segs[R_CS].base : 0ULL;
        uint8_t insn_bytes[TB_MAX_X86_INSN_LEN * TB_DECODE_MAX_INSNS];
        int fetched = 0;

        while (n < TB_DECODE_MAX_INSNS &&
               off + TB_MAX_X86_INSN_LEN <= (int)sizeof(insn_bytes)) {
            int need = off + TB_MAX_X86_INSN_LEN;
            int valid_fetched = -1;
            int faulted = 0;
            while (fetched < need) {
                if (xenv != 0) {
                    xenv->exception_index = -1;
                    insn_bytes[fetched] =
                        latx_x86_fetch_byte(xenv, fetch_base + pc + (uint64_t)fetched);
                    if (xenv->exception_index == EXCP0E_PAGE) {
                        if (fetched == off) {
                            if (n == 0) {
                                return -1;
                            }
                            xenv->exception_index = -1;
                            faulted = 1;
                            break;
                        }
                        /* Lookahead hit an unmapped page; pad tail for disasm only. */
                        xenv->exception_index = -1;
                        valid_fetched = fetched;
                        for (int p = fetched; p < need; p++) {
                            insn_bytes[p] = 0x90;
                        }
                        break;
                    }
                } else {
                    insn_bytes[fetched] =
                        latx_bare_ldub_code(fetch_base + pc + (uint64_t)fetched);
                }
                fetched++;
            }
            if (faulted) {
                break;
            }
            ADDRX next = ir1_disasm(&ir1_list[n], insn_bytes + off,
                                    (ADDRX)(pc + off), n, ir1_dt_area);

            if (next <= (ADDRX)(pc + off)) {
                break;
            }
            if (valid_fetched >= 0 && (int)(next - pc) > valid_fetched) {
                if (n == 0) {
                    (void)latx_x86_fetch_byte(xenv, fetch_base + pc + (uint64_t)valid_fetched);
                    return -1;
                }
                break;
            }
            off = (int)(next - pc);
            n++;
            /*
             * Capture before translation: some translators rewrite decoded
             * fields such as addr_size while lowering an instruction.
             */
            if (out_map != 0) {
                TbInsnSpan *span = &out_map->spans[n - 1];

                span->ea_seg_class = ir1_ea_seg_class(&ir1_list[n - 1]);
                span->x86_size = (uint8_t)ir1_list[n - 1].info->size;
                span->fault_ea_index = TB_HARNESS_FAULT_EA_NONE;
                if (cpu_env != 0 &&
                    cpu_env->sys.codemode == LATX_DT_X64) {
                    tb_harness_record_fault_ea(
                        out_map, span, &ir1_list[n - 1]);
                }
            }
            if (is_control_flow(&ir1_list[n - 1])) {
                break;
            }
            if (valid_fetched >= 0 && off >= valid_fetched) {
                break;
            }
        }
        {
            uint8_t pending_use = __ALL_EFLAGS;
            for (int i = n - 1; i >= 0; i--) {
                flag_reduction(&ir1_list[i], &pending_use);
            }
        }
    }
    if (n == 0) {
        return -1;
    }

    ir1_sd.ir1 = ir1_list;
    ir1_sd.is_first_tb = 1;
    ir1_sd.next_pc = pc + off;
    tb.pc = pc;
    tb.size = (uint16_t)off;
    tb.icount = (uint16_t)n;
    tb.tc.ptr = code;
    tb.s_data = &ir1_sd;
    /*
     * Never link: there is no runtime TB linker here, and the translator's
     * goto_tb stub is a nop followed by a link slot that only the linker
     * would fill.  With both successors marked unlinkable the exit goes to
     * the epilogue instead.
     */
    tb.canlink[0] = 0;
    tb.canlink[1] = 0;

    rc = tr_translate_tb(&tb);
    if (rc <= 0 || (uint64_t)rc > limit) {
        return -1;
    }

    if (out_map != 0) {
        out_map->count = (uint16_t)n;
        for (int i = 0; i < n; i++) {
            TbInsnSpan *span = &out_map->spans[i];

            span->x86_pc = (uint64_t)tcg_ctx->gen_insn_data[i * 2];
            span->native_end_off = tcg_ctx->gen_insn_end_off[i];
            span->ea_seg_class = ir1_ea_seg_class(&ir1_list[i]);
            span->x86_size = (uint8_t)ir1_list[i].info->size;
        }
        out_map->jmp_slot_off[0] = -1;
        out_map->jmp_slot_off[1] = -1;
        out_map->jmp_target_pc[0] = 0;
        out_map->jmp_target_pc[1] = 0;
    }

    /*
     * A direct branch emits a link slot -- `b 0` -- followed by the epilogue
     * jump.  We turn the slot into `nop` (`andi r0, r0, 0`) initially so an
     * unlinked successor falls through to the epilogue, and record the slot
     * offset and successor x86 PC in `out_map` so the runtime loop can patch
     * it into a direct LoongArch `b offs26` once the target TB is translated.
     */
    for (int i = 0; i < 2; i++) {
        int64_t off = (int64_t)tb.jmp_target_arg[i];

        if (off < 0 || off + 4 > (int64_t)rc) {
            continue;
        }
        if (*(uint32_t *)(code + off) != 0x50000000u) {   /* b 0 */
            continue;
        }
        if (out_map != 0) {
            out_map->jmp_slot_off[i] = (int32_t)off;
            out_map->jmp_target_pc[i] = (uint64_t)((int64_t)tb.pc + tb.lazypc[i]);
        }
        *(uint32_t *)(code + off) = 0x03400000u;           /* andi r0, r0, 0 */
    }
    return rc;
}

int tb_harness_translate_at(uint64_t pc, uint8_t *code, uint64_t limit)
{
    return tb_harness_translate_at_with_map(pc, code, limit, 0);
}

int tb_harness_resolve_x86_fault(const TbInsnMap *map, uint64_t tb_code,
                                 uint64_t era, uint64_t fallback_pc,
                                 uint64_t *out_x86_pc,
                                 uint8_t *out_ea_seg_class)
{
    uint64_t off;

    if (out_x86_pc != 0) {
        *out_x86_pc = fallback_pc;
    }
    if (out_ea_seg_class != 0) {
        *out_ea_seg_class = IR1_EA_SEG_UNKNOWN;
    }
    if (map == 0 || map->count == 0 || era < tb_code) {
        return 0;
    }
    off = era - tb_code;
    for (uint16_t i = 0; i < map->count; i++) {
        if (off < (uint64_t)map->spans[i].native_end_off) {
            if (out_x86_pc != 0) {
                *out_x86_pc = map->spans[i].x86_pc;
            }
            if (out_ea_seg_class != 0) {
                *out_ea_seg_class = map->spans[i].ea_seg_class;
            }
            return 1;
        }
    }
    return 0;
}

static int tb_harness_fault_range_contains(uint64_t address, uint64_t start,
                                           uint8_t size)
{
    return size != 0 && address >= start && address - start < size;
}

static uint64_t tb_harness_fault_ea_address(const TbFaultEa *ea,
                                            const TbInsnSpan *span,
                                            const uint64_t gpr[16],
                                            uint64_t rsp_override,
                                            int use_rsp_override,
                                            uint64_t fs_base,
                                            uint64_t gs_base)
{
    uint64_t base = 0;
    uint64_t index = 0;
    uint64_t offset;

    if (ea->base == TB_HARNESS_FAULT_REG_RIP) {
        base = span->x86_pc + span->x86_size;
    } else if (ea->base == 4 && use_rsp_override) {
        base = rsp_override;
    } else if (ea->base != TB_HARNESS_FAULT_REG_NONE) {
        if (ea->base >= 16) {
            return UINT64_MAX;
        }
        base = gpr[ea->base];
    }
    if (ea->index != TB_HARNESS_FAULT_REG_NONE) {
        if (ea->index >= 16) {
            return UINT64_MAX;
        }
        index = gpr[ea->index] * ea->scale;
    }
    offset = base + index + (uint64_t)(int64_t)ea->displacement;
    if (ea->address_size == 4) {
        offset = (uint32_t)offset;
    } else if (ea->address_size != 8) {
        return UINT64_MAX;
    }
    if (ea->segment_prefix == dt_X86_PREFIX_FS) {
        offset += fs_base;
    } else if (ea->segment_prefix == dt_X86_PREFIX_GS) {
        offset += gs_base;
    } else if (ea->segment_prefix != 0) {
        return UINT64_MAX;
    }
    return offset;
}

int tb_harness_refine_x86_fault_ea(const TbInsnMap *map, uint64_t tb_code,
                                   uint64_t era, uint64_t badv,
                                   const uint64_t gpr[16],
                                   uint64_t fs_base, uint64_t gs_base,
                                   uint8_t *out_ea_seg_class)
{
    if (out_ea_seg_class != 0) {
        *out_ea_seg_class = IR1_EA_SEG_UNKNOWN;
    }
    if (map == 0 || map->count == 0 || gpr == 0 || era < tb_code) {
        return 0;
    }

    uint64_t off = era - tb_code;
    for (uint16_t i = 0; i < map->count; i++) {
        const TbInsnSpan *span = &map->spans[i];
        if (off >= (uint64_t)span->native_end_off) {
            continue;
        }
        if (span->ea_seg_class != IR1_EA_SEG_UNKNOWN ||
            span->fault_ea_index == TB_HARNESS_FAULT_EA_NONE ||
            span->fault_ea_index >= map->fault_ea_count ||
            span->stack_access_size == 0 ||
            (span->stack_access_kind != TB_HARNESS_STACK_ACCESS_PUSH &&
             span->stack_access_kind != TB_HARNESS_STACK_ACCESS_POP)) {
            return 0;
        }

        const TbFaultEa *ea = &map->fault_eas[span->fault_ea_index];
        if (ea->ea_seg_class != IR1_EA_SEG_NOT_SS || ea->size == 0 ||
            span->stack_access_size != ea->size ||
            (ea->address_size != 4 && ea->address_size != 8) ||
            (ea->segment_prefix != 0 &&
             ea->segment_prefix != dt_X86_PREFIX_FS &&
             ea->segment_prefix != dt_X86_PREFIX_GS) ||
            (ea->base != TB_HARNESS_FAULT_REG_NONE &&
             ea->base != TB_HARNESS_FAULT_REG_RIP && ea->base >= 16) ||
            (ea->index != TB_HARNESS_FAULT_REG_NONE && ea->index >= 16) ||
            (ea->index != TB_HARNESS_FAULT_REG_NONE &&
             ea->scale != 1 && ea->scale != 2 &&
             ea->scale != 4 && ea->scale != 8)) {
            return 0;
        }

        /*
         * The fault can occur on either side of the implicit RSP adjustment.
         * For an explicit [RSP] EA, also try the adjacent architectural value.
         */
        uint64_t rsp = gpr[4];
        uint64_t shifted_rsp = rsp + span->stack_access_size;
        uint64_t ea_addr = tb_harness_fault_ea_address(
            ea, span, gpr, rsp, 0, fs_base, gs_base);
        int data_match = tb_harness_fault_range_contains(
            badv, ea_addr, ea->size);

        if (ea->base == 4) {
            uint64_t alternate_ea = tb_harness_fault_ea_address(
                ea, span, gpr, shifted_rsp, 1, fs_base, gs_base);
            data_match |= tb_harness_fault_range_contains(
                badv, alternate_ea, ea->size);
        }

        /* Covers the pre-adjust and post-adjust implicit stack access. */
        int stack_match =
            tb_harness_fault_range_contains(
                badv, rsp, span->stack_access_size) ||
            tb_harness_fault_range_contains(
                badv, rsp - span->stack_access_size,
                span->stack_access_size);
        if (data_match == stack_match) {
            return 0;
        }
        if (out_ea_seg_class != 0) {
            *out_ea_seg_class =
                data_match ? ea->ea_seg_class : IR1_EA_SEG_SS;
        }
        return 1;
    }
    return 0;
}

uint64_t tb_harness_resolve_x86_span(const TbInsnMap *map, uint64_t tb_code,
                                     uint64_t era, uint64_t fallback_pc,
                                     uint8_t *out_ea_seg_class)
{
    uint64_t off;

    if (out_ea_seg_class != 0) {
        *out_ea_seg_class = IR1_EA_SEG_UNKNOWN;
    }
    if (map == 0 || map->count == 0 || era < tb_code) {
        return fallback_pc;
    }
    off = era - tb_code;
    for (uint16_t i = 0; i < map->count; i++) {
        if (off < (uint64_t)map->spans[i].native_end_off) {
            if (out_ea_seg_class != 0) {
                *out_ea_seg_class = map->spans[i].ea_seg_class;
            }
            return map->spans[i].x86_pc;
        }
    }
    return map->spans[map->count - 1].x86_pc;
}

uint64_t tb_harness_resolve_x86_pc(const TbInsnMap *map, uint64_t tb_code,
                                   uint64_t era, uint64_t fallback_pc)
{
    return tb_harness_resolve_x86_span(map, tb_code, era, fallback_pc, 0);
}

