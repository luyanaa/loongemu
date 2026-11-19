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
static uint8_t ir1_dt_area[32768] __attribute__((aligned(16)));

static const int control_flow_ops[] = {
    dt_X86_INS_JMP, dt_X86_INS_CALL, dt_X86_INS_RET, dt_X86_INS_JAE,
    dt_X86_INS_JA, dt_X86_INS_JBE, dt_X86_INS_JB, dt_X86_INS_JCXZ,
    dt_X86_INS_JECXZ, dt_X86_INS_JE, dt_X86_INS_JGE, dt_X86_INS_JG,
    dt_X86_INS_JLE, dt_X86_INS_JL, dt_X86_INS_JNE, dt_X86_INS_JNO,
    dt_X86_INS_JNP, dt_X86_INS_JNS, dt_X86_INS_JO, dt_X86_INS_JP,
    dt_X86_INS_JS,
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
        uint8_t insn_bytes[TB_MAX_X86_INSN_LEN * TB_DECODE_MAX_INSNS] = { 0 };

        /*
         * The harness decodes from the flat image directly.  With paging on,
         * cpu_ldub_code() -- the translator's own fetch -- goes through the
         * walker, and both agree because the harness maps the image into the
         * same physical memory the guest's tables describe.
         */
        for (unsigned b = 0; b < sizeof(insn_bytes); b++) {
            insn_bytes[b] = latx_bare_ldub_code(pc + b);
        }

        while (n < TB_DECODE_MAX_INSNS &&
               off + TB_MAX_X86_INSN_LEN <= (int)sizeof(insn_bytes)) {
            ADDRX next = ir1_disasm(&ir1_list[n], insn_bytes + off,
                                    (ADDRX)(pc + off), n, ir1_dt_area);

            if (next <= (ADDRX)(pc + off)) {
                break;
            }
            off = (int)(next - pc);
            /*
             * Fill each instruction's eflag use/def bitmap.  Normally the
             * flag reduction pass does this over the whole TB; without it
             * every _eflag_def is zero, ir1_need_calculate_any_flag()
             * answers "no" for every instruction, no arithmetic updates the
             * LBT flag state, and a following conditional branch reads stale
             * flags.
             */
            flag_gen(&ir1_list[n]);
            n++;
            if (is_control_flow(&ir1_list[n - 1])) {
                break;
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
            out_map->spans[i].x86_pc = (uint64_t)tcg_ctx->gen_insn_data[i * 2];
            out_map->spans[i].native_end_off = tcg_ctx->gen_insn_end_off[i];
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

uint64_t tb_harness_resolve_x86_pc(const TbInsnMap *map, uint64_t tb_code,
                                   uint64_t era, uint64_t fallback_pc)
{
    uint64_t off;

    if (map == 0 || map->count == 0 || era < tb_code) {
        return fallback_pc;
    }
    off = era - tb_code;
    for (uint16_t i = 0; i < map->count; i++) {
        if (off < (uint64_t)map->spans[i].native_end_off) {
            return map->spans[i].x86_pc;
        }
    }
    return map->spans[map->count - 1].x86_pc;
}
