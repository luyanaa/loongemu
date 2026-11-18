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

static IR1_INST ir1_list[16];
static struct separated_data ir1_sd;
static uint8_t ir1_dt_area[8192] __attribute__((aligned(16)));

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
#define TB_DECODE_MAX_INSNS 8
#define TB_MAX_X86_INSN_LEN 15

int tb_harness_translate_at(uint64_t pc, uint8_t *code, uint64_t limit)
{
    TranslationBlock tb;
    int off = 0, n = 0;
    int rc;

    memset(&tb, 0, sizeof(tb));
    memset(ir1_list, 0, sizeof(ir1_list));
    memset(&ir1_sd, 0, sizeof(ir1_sd));

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

    /*
     * A direct branch emits a link slot -- `b 0` -- that only the runtime TB
     * linker ever fills, followed by the epilogue jump.  There is no linker
     * here, so the slot is turned into a nop and execution falls through to
     * the epilogue instead of spinning on itself.  jmp_target_arg holds the
     * slot's byte offset once assembly has resolved the label.
     *
     * A successor that this block does not have -- the call block exits
     * through one path only -- leaves the label undisposed, and label_dispose
     * stores -1.  As unsigned, `-1 + 4 <= rc` holds, so an unchecked write
     * lands one byte before the block and rewrites the high byte of its first
     * instruction: the call's return address then materialises from garbage,
     * `ret` pops it, and execution continues at an address nothing was
     * translated for.  Both the signed bound and the `b 0` check below keep
     * the patch inside the block and on the slot it is meant for.
     */
    for (int i = 0; i < 2; i++) {
        int64_t off = (int64_t)tb.jmp_target_arg[i];

        if (off < 0 || off + 4 > (int64_t)rc) {
            continue;
        }
        if (*(uint32_t *)(code + off) != 0x50000000u) {   /* b 0 */
            continue;
        }
        *(uint32_t *)(code + off) = 0x03400000u;           /* andi r0, r0, 0 */
    }
    return rc;
}
