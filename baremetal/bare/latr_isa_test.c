#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "exec/exec-all.h"
#include "latr.h"
#include "translate.h"
#include "fpu/softfloat.h"
#include "qemu/host-utils.h"
#include "riscv-env.h"
#include "mmu.h"
#include "latr-system.h"
#include "loongemu/phys_mem.h"
#include "loongemu/riscv_sbi.h"
#include "loongemu/riscv_plic.h"
#include "loongemu/vclock.h"
#include "loongemu/mmio_bus.h"
#include "loongemu/uart16550.h"
#include "loongemu-perf.h"
#include "qemu/cpu-features.h"
#include "tests/latr_isa_diff_vectors.h"

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)

/* Trap delivery redirects env.pc to mtvec; tests assert the saved PC. */
#define LATR_TEST_MEPC 0x341

void latx_bare_uart_putc(int c) { *UART0_THR = (uint8_t)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s != '\0') {
        latx_bare_uart_putc(*s++);
    }
}

FILE *stderr;

static CPUArchState env;
static CPUState cpu;
static TranslationBlock tb;
static uint8_t code_buf[262144] __attribute__((aligned(4096)));
static uint64_t data_word;
static uint64_t other_data_word;
static uint64_t cbo_buf[8] __attribute__((aligned(64)));
static int checks;
static int failures;
static uint64_t diff_div;
static uint64_t diff_rem;
static uint64_t diff_mulw;
static uint64_t diff_caddi;
static uint64_t diff_cbeqz_taken;
static uint64_t diff_cbeqz_not_taken;
static uint64_t diff_cj;
static uint64_t diff_divw;
static uint64_t diff_remw;
static uint64_t diff_divuw;
static uint64_t diff_remuw;
static uint64_t diff_fp_rmm;
static uint64_t diff_fp_rmm_dynamic;
static uint64_t diff_fp_div_rmm;
static uint64_t diff_fp_fma;
static uint64_t diff_rvv;

extern void latr_tr_data_init(CPUArchState *env);
extern void tr_init(void *tb);
extern void tr_fini(void);
extern int tr_ir2_assemble(const void *code_start_addr);
extern void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu);
extern bool tr_ir2_generate(TranslationBlock *tb);
extern void latr_enter_tb(void *env, void *code);
extern bool latr_tb_insn_map_resolve(uintptr_t host_pc, uint64_t *guest_pc,
                                     uint32_t *raw_len);
extern void latr_tb_insn_map_forget(void *code);
extern const LatrTbInsnSpan *latr_tb_recovery_resolve(uintptr_t host_pc,
                                                    const LatrTbInsnMap **map_out);
extern bool latr_tb_recovery_apply(const LatrTbInsnSpan *span,
                                   const LatrTbInsnMap *map,
                                   const uint64_t host_gprs[32],
                                   const uint64_t host_fprs[32],
                                   CPUArchState *env);
extern bool latr_service_sbi(CPUArchState *env, LoongemuSbi *sbi);
extern void latr_update_interrupts(CPUArchState *env);
extern bool latr_interrupt_pending(CPUArchState *env);
extern bool latr_deliver_interrupt(CPUArchState *env);
extern void latr_idle_advance(CPUArchState *env);
extern uint64_t latr_rvv_vsetvl(CPUArchState *env, uint64_t s1,
                                uint64_t vtype);
extern void latr_rvv_mv(CPUArchState *env);
extern void latr_rvv_mask_scan(CPUArchState *env);
extern uint64_t latr_rvv_mem(CPUArchState *env);
extern uint64_t latr_rvv_stride_mem(CPUArchState *env);


__asm__(
".text\n"
".globl latr_enter_tb\n"
"latr_enter_tb:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $s8, $sp, 8\n"
"  move   $s8, $a0\n"
"  jirl   $ra, $a1, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $s8, $sp, 8\n"
"  addi.d $sp, $sp, 16\n"
"  jirl   $zero, $ra, 0\n");


void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(latr_stack)\n  addi.d $sp, $sp, %pc_lo12(latr_stack)\n"
        "  lu12i.w $t0, 128\n  add.d $sp, $sp, $t0\n"
        "  bl kmain\n1: b 1b\n");
static uint8_t latr_stack[524288] __attribute__((aligned(16), used));

static void puthex(uint64_t value);
static void check(uint64_t got, uint64_t want, const char *name)
{
    checks++;
    if (got != want) {
        failures++;
        latx_bare_uart_puts("latr-isa: FAIL ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" got=0x");
        puthex(got);
        latx_bare_uart_puts(" want=0x");
        puthex(want);
        latx_bare_uart_puts("\n");
    }
}

static void puthex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        latx_bare_uart_putc(digits[(value >> shift) & 0xf]);
    }
}

static void putdec(unsigned value)
{
    char digits[10];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        latx_bare_uart_putc(digits[--count]);
    }
}

static void reset_guest_state(void)
{
    uint8_t *state = (uint8_t *)&env;
    for (size_t i = 0; i < sizeof(env); i++) {
        state[i] = 0;
    }
    /* Unit-harness convenience: tests that care about FS=Off clear this. */
    env.csr[0x300] = UINT64_C(3) << 13;
    /* Permissive PMP entry 0 (TOR, RWX) covering the physical address space;
     * PMP-specific tests overwrite it. */
    env.csr[0x3a0] = 0x0f;
    env.csr[0x3b0] = (UINT64_C(1) << 54) - 1;
    /* Unit harness runs with hardware A/D updates (Svadu). */
    env.csr[0x30a] = UINT64_C(1) << 61;
}

/* Code-symbol census over every block this harness executes. */
static unsigned code_symbol_blocks;
static unsigned code_symbol_blocks_with_syms;
static unsigned code_symbol_total;
static unsigned code_symbol_max;
static unsigned code_symbol_overflows;

/* Runs one TB against the current env; callers own env setup. */
static bool latr_execute_block(const void *guest, int insns)
{
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)guest;
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = LATR_TB_CFLAGS_ALLOW_FLOW; /* unit harness: multi-flow block */

    int max_insns = insns;
    target_disasm(&tb, &max_insns, &cpu);
    if (tb.icount != insns) {
        return false;
    }
    tr_init(&tb);
    bool ok = tr_ir2_generate(&tb);
    if (ok) {
        int words = tr_ir2_assemble(code_buf);
        if ((size_t)words * 4 > sizeof(code_buf)) {
            return false; /* TB buffer overflow must never corrupt memory */
        }
        /* Code-symbol accounting (persistent-code-cache P0): how many absolute
         * host addresses this block embeds, i.e. how many words a cache would
         * have to relocate instead of copying. */
        {
            int syms = latr_tb_code_symbol_count();

            code_symbol_blocks++;
            code_symbol_total += (unsigned)syms;
            if (syms > 0) {
                code_symbol_blocks_with_syms++;
            }
            if (code_symbol_max < (unsigned)syms) {
                code_symbol_max = (unsigned)syms;
            }
            if (latr_tb_code_symbols_overflowed()) {
                code_symbol_overflows++;
            }
        }
        __asm__ volatile("ibar 0" ::: "memory");
        latr_enter_tb(&env, code_buf);
    }
    tr_fini();
    return ok;
}

/* Translates `insns` at `guest` and keeps the generated LoongArch code in
 * code_buf, so the caller can enter the very same TB more than once with a
 * different env.  vstart is deliberately *not* part of the TB key, so this is
 * the only way to reproduce what happens when a restart lands on an
 * already-translated block. */
static bool latr_translate_block(const void *guest, int insns)
{
    int max_insns = insns;
    bool ok;

    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)guest;
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = LATR_TB_CFLAGS_ALLOW_FLOW;

    target_disasm(&tb, &max_insns, &cpu);
    if (tb.icount != insns) {
        return false;
    }
    tr_init(&tb);
    ok = tr_ir2_generate(&tb);
    if (ok) {
        int words = tr_ir2_assemble(code_buf);
        if ((size_t)words * 4 > sizeof(code_buf)) {
            tr_fini();
            return false;
        }
        __asm__ volatile("ibar 0" ::: "memory");
    }
    tr_fini();
    return ok;
}

/* Re-enters the TB most recently built by latr_translate_block(). */
static void latr_reenter_tb(void)
{
    latr_enter_tb(&env, code_buf);
}

static bool run_block_with_regs_and_fpr(const void *guest, int insns,
                                        int reg0, uint64_t value0,
                                        int reg1, uint64_t value1,
                                        int fpreg, uint64_t fpvalue)
{
    reset_guest_state();
    if (reg0 >= 0) {
        env.gpr[reg0] = value0;
    }
    if (reg1 >= 0) {
        env.gpr[reg1] = value1;
    }
    if (fpreg >= 0) {
        env.fpr[fpreg] = fpvalue;
    }
    return latr_execute_block(guest, insns);
}

static bool run_block_with_regs(const void *guest, int insns,
                                int reg0, uint64_t value0,
                                int reg1, uint64_t value1)
{
    return run_block_with_regs_and_fpr(guest, insns, reg0, value0,
                                       reg1, value1, -1, 0);
}

static bool run_block(const void *guest, int insns, int initial_reg,
                      uint64_t initial_value)
{
    return run_block_with_regs(guest, insns, initial_reg, initial_value, -1, 0);
}

static void test_integer_and_branch(void)
{
    static const uint32_t arithmetic[] = {
        0x00500093, /* addi x1, x0, 5 */
        0xffe08113, /* addi x2, x1, -2 */
        0x002081b3, /* add x3, x1, x2 */
        0x0001821b, /* addiw x4, x3, 0 */
    };
    latx_bare_uart_puts("latr-isa: arithmetic begin\n");
    check(run_block(arithmetic, 4, -1, 0), 1, "arithmetic block translated");
    check(env.gpr[3], 8, "ADD result");
    check(env.gpr[4], 8, "ADDIW sign-extended result");
    check(env.pc, (uint64_t)(uintptr_t)(arithmetic + 4), "arithmetic next PC");
    latx_bare_uart_puts("latr-isa: arithmetic done\n");

    static const uint32_t mulw[] = {
        LATR_DIFF_MULW_X3_X1_X2, /* mulw x3, x1, x2 */
    };
    check(run_block_with_regs(mulw, 1,
                              1, UINT64_C(0x7fffffff),
                              2, UINT64_C(0x7fffffff)),
          1, "MULW block translated");
    check(env.gpr[3], 1, "MULW retains only low 32 product bits");
    diff_mulw = env.gpr[3];

    static const uint32_t branch[] = {
        0x00100093, /* addi x1, x0, 1 */
        0x00108463, /* beq x1, x1, +8 */
        0x00500113, /* addi x2, x0, 5 (skipped) */
        0x00700113, /* addi x2, x0, 7 */
    };
    latx_bare_uart_puts("latr-isa: branch begin\n");
    check(run_block(branch, 4, -1, 0), 1, "branch block translated");
    check(env.gpr[2], 7, "BEQ target path");
    latx_bare_uart_puts("latr-isa: branch done\n");

    static const uint32_t backedge[] = {
        0x00100093, /* addi x1, x0, 1 */
        0xfe108ee3, /* beq x1, x1, -4 */
    };
    check(run_block(backedge, 2, -1, 0), 1,
          "backward branch block translated");
    check(env.pc, (uint64_t)(uintptr_t)backedge,
          "backward branch returns to dispatcher");
}

static void test_memory_and_compressed(void)
{
    static const uint32_t memory[] = {
        0x02a00093, /* addi x1, x0, 42 */
        0x00113023, /* sd x1, 0(x2) */
        0x00013183, /* ld x3, 0(x2) */
    };
    data_word = 0;
    latx_bare_uart_puts("latr-isa: memory begin\n");
    check(run_block(memory, 3, 2, (uint64_t)(uintptr_t)&data_word), 1,
          "memory block translated");
    check(data_word, 42, "SD wrote guest memory");
    check(env.gpr[3], 42, "LD read guest memory");
    latx_bare_uart_puts("latr-isa: memory done\n");

    static const uint32_t divzero[] = {
        LATR_DIFF_ADDI_X1_5,    /* addi x1, x0, 5 */
        LATR_DIFF_DIV_X3_X1_X2, /* div x3, x1, x2 */
        LATR_DIFF_REM_X4_X1_X2, /* rem x4, x1, x2 */
    };
    latx_bare_uart_puts("latr-isa: M begin\n");
    check(run_block(divzero, 3, -1, 0), 1, "M extension block translated");
    check(env.gpr[3], UINT64_MAX, "DIV by zero returns all ones");
    check(env.gpr[4], 5, "REM by zero returns dividend");
    diff_div = env.gpr[3];
    diff_rem = env.gpr[4];
    static const uint32_t word_divzero[] = {
        LATR_DIFF_ADDI_X1_5,     /* addi x1, x0, 5 */
        LATR_DIFF_DIVW_X3_X1_X2, /* divw x3, x1, x2 */
        LATR_DIFF_REMW_X4_X1_X2, /* remw x4, x1, x2 */
        LATR_DIFF_DIVUW_X5_X1_X2, /* divuw x5, x1, x2 */
        LATR_DIFF_REMUW_X6_X1_X2, /* remuw x6, x1, x2 */
    };
    check(run_block(word_divzero, 5, 2, UINT64_C(0x100000000)), 1,
          "word divide by low-32-zero block translated");
    check(env.gpr[3], UINT64_MAX, "DIVW sees low-word zero divisor");
    check(env.gpr[4], 5, "REMW by low-word zero returns dividend");
    check(env.gpr[5], UINT64_MAX, "DIVUW zero result is sign-extended");
    check(env.gpr[6], 5, "REMUW by low-word zero returns dividend");
    diff_divw = env.gpr[3];
    diff_remw = env.gpr[4];
    diff_divuw = env.gpr[5];
    diff_remuw = env.gpr[6];
    static const uint32_t signed_overflow[] = {
        0x0220c1b3, /* div x3, x1, x2 */
        0x0220e233, /* rem x4, x1, x2 */
    };
    check(run_block_with_regs(signed_overflow, 2, 1,
                              (uint64_t)INT64_MIN, 2, UINT64_MAX),
          1, "signed 64-bit division overflow block translated");
    check(env.gpr[3], (uint64_t)INT64_MIN, "DIV min/-1 returns min");
    check(env.gpr[4], 0, "REM min/-1 returns zero");

    static const uint32_t word_signed_overflow[] = {
        0x0220c1bb, /* divw x3, x1, x2 */
        0x0220e23b, /* remw x4, x1, x2 */
    };
    check(run_block_with_regs(word_signed_overflow, 2, 1,
                              UINT64_C(0x80000000), 2,
                              UINT64_C(0xffffffff)),
          1, "signed word division overflow block translated");
    check(env.gpr[3], UINT64_C(0xffffffff80000000),
          "DIVW min/-1 returns sign-extended min");
    check(env.gpr[4], 0, "REMW min/-1 returns zero");
    latx_bare_uart_puts("latr-isa: M done\n");

    static const uint32_t amoadd[] = {
        0x00500093, /* addi x1, x0, 5 */
        0x001131af, /* amoadd.d x3, x1, (x2) */
    };
    data_word = 37;
    latx_bare_uart_puts("latr-isa: AMO begin\n");
    check(run_block(amoadd, 2, 2, (uint64_t)(uintptr_t)&data_word), 1,
          "AMO block translated");
    check(env.gpr[3], 37, "AMO returns old value");
    check(data_word, 42, "AMO atomically updates memory");

    static const uint32_t lrsc[] = {
        0x1001322f, /* lr.d x4, (x2) */
        0x06300093, /* addi x1, x0, 99 */
        0x181132af, /* sc.d x5, x1, (x2) */
    };
    data_word = 42;
    check(run_block(lrsc, 3, 2, (uint64_t)(uintptr_t)&data_word), 1,
          "LR/SC block translated");
    check(env.gpr[4], 42, "LR returns reserved value");
    check(env.gpr[5], 0, "SC reports success");
    check(data_word, 99, "SC stores reserved value");

    static const uint32_t sc_zero_failed[] = {
        0x1001b22f, /* lr.d x4, (x3), reserve a different address */
        0x180132af, /* sc.d x5, x0, (x2) */
    };
    data_word = 42;
    other_data_word = 7;
    check(run_block_with_regs(sc_zero_failed, 2,
                              2, (uint64_t)(uintptr_t)&data_word,
                              3, (uint64_t)(uintptr_t)&other_data_word),
          1, "SC with x0 source block translated");
    check(env.gpr[5], 1, "failed SC with x0 source reports failure");
    check(data_word, 42, "failed SC with x0 source leaves memory unchanged");
    latx_bare_uart_puts("latr-isa: AMO done\n");

    static const uint8_t compressed[] = {
        (uint8_t)(LATR_DIFF_C_ADDI_X10_1 & 0xff),
        (uint8_t)(LATR_DIFF_C_ADDI_X10_1 >> 8),
        0x13, 0x01, 0x00, 0x00, /* addi x2, x0, 0 */
    };
    latx_bare_uart_puts("latr-isa: compressed begin\n");
    check(run_block(compressed, 2, 10, 9), 1,
          "compressed block translated");
    check(env.gpr[10], 10, "C.ADDI expanded and executed");
    diff_caddi = env.gpr[10];
    check(env.pc, (uint64_t)(uintptr_t)(compressed + sizeof(compressed)),
          "compressed next PC uses raw instruction length");
    latx_bare_uart_puts("latr-isa: compressed addi done\n");

    uint64_t compressed_data[3] = {0, 42, 0};
    static const uint16_t compressed_ldsd[] = {
        0x6404, /* c.ld x9, 8(x8) */
        0xe804, /* c.sd x9, 16(x8) */
    };
    check(run_block(compressed_ldsd, 2, 8,
                    (uint64_t)(uintptr_t)compressed_data),
          1, "compressed load/store block translated");
    check(env.gpr[9], 42, "C.LD loads compressed base register");
    check(compressed_data[2], 42, "C.SD stores compressed base register");
    check(env.pc, (uint64_t)(uintptr_t)(compressed_ldsd + 2),
          "compressed load/store advances by two bytes each");
    latx_bare_uart_puts("latr-isa: compressed load/store done\n");
    uint64_t compressed_fp_data[4] = {
        UINT64_C(0x3ff8000000000000), 0,
        UINT64_C(0x4002000000000000), 0,
    };
    static const uint16_t compressed_fld[] = { 0x2000 }; /* c.fld f8, 0(x8) */
    check(run_block(compressed_fld, 1, 8,
                    (uint64_t)(uintptr_t)compressed_fp_data),
          1, "compressed C.FLD block translated");
    check(env.fpr[8], compressed_fp_data[0], "C.FLD loads double register");
    check(env.pc, (uint64_t)(uintptr_t)(compressed_fld + 1),
          "C.FLD advances by two bytes");
    latx_bare_uart_puts("latr-isa: compressed fld done\n");

    static const uint16_t compressed_fsd[] = { 0xa400 }; /* c.fsd f8, 8(x8) */
    check(run_block_with_regs_and_fpr(
              compressed_fsd, 1, 8, (uint64_t)(uintptr_t)compressed_fp_data,
              -1, 0, 8, compressed_fp_data[0]),
          1, "compressed C.FSD block translated");
    check(compressed_fp_data[1], compressed_fp_data[0],
          "C.FSD stores double register");
    check(env.pc, (uint64_t)(uintptr_t)(compressed_fsd + 1),
          "C.FSD advances by two bytes");
    latx_bare_uart_puts("latr-isa: compressed fsd done\n");

    static const uint16_t compressed_fldsp[] = {
        0x20c2, /* c.fldsp f1, 16(x2) */
    };
    check(run_block(compressed_fldsp, 1, 2,
                    (uint64_t)(uintptr_t)compressed_fp_data),
          1, "compressed C.FLDSP block translated");
    check(env.fpr[1], compressed_fp_data[2], "C.FLDSP loads stack double");
    check(env.pc, (uint64_t)(uintptr_t)(compressed_fldsp + 1),
          "C.FLDSP advances by two bytes");
    latx_bare_uart_puts("latr-isa: compressed fldsp done\n");

    static const uint16_t compressed_fsdsp[] = {
        0xac06, /* c.fsdsp f1, 24(x2) */
    };
    check(run_block_with_regs_and_fpr(
              compressed_fsdsp, 1, 2, (uint64_t)(uintptr_t)compressed_fp_data,
              -1, 0, 1, compressed_fp_data[2]),
          1, "compressed C.FSDSP block translated");
    check(compressed_fp_data[3], compressed_fp_data[2],
          "C.FSDSP stores stack double");
    check(env.pc, (uint64_t)(uintptr_t)(compressed_fsdsp + 1),
          "C.FSDSP advances by two bytes");
    latx_bare_uart_puts("latr-isa: compressed fsdsp done\n");
    static const uint8_t compressed_branch[] = {
        0x11, 0xc0,             /* c.beqz x8, +4 */
        0xa5, 0x44,             /* c.li x9, 9 */
        0x85, 0x04,             /* c.addi x9, 1 */
        0x33, 0x85, 0x84, 0x00, /* add x10, x9, x8 */
    };
    check(run_block_with_regs(compressed_branch, 4, 8, 0, -1, 0),
          1, "compressed taken branch block translated");
    check(env.gpr[10], 1, "C.BEQZ takes forward target");
    diff_cbeqz_taken = env.gpr[10];
    check(env.pc,
          (uint64_t)(uintptr_t)(compressed_branch + sizeof(compressed_branch)),
          "compressed branch block advances mixed instruction widths");
    latx_bare_uart_puts("latr-isa: compressed branch taken done\n");
    check(run_block_with_regs(compressed_branch, 4, 8, 1, -1, 0),
          1, "compressed not-taken branch block translated");
    check(env.gpr[10], 11, "C.BEQZ falls through when nonzero");
    diff_cbeqz_not_taken = env.gpr[10];
    latx_bare_uart_puts("latr-isa: compressed branch not-taken done\n");
    static const uint8_t compressed_jump[] = {
        0x19, 0xa0,             /* c.j +6 */
        0x13, 0x05, 0x90, 0x00, /* addi x10, x0, 9; skipped */
        0x13, 0x05, 0x15, 0x00, /* addi x10, x10, 1 */
    };
    check(run_block(compressed_jump, 3, 10, 0), 1,
          "compressed jump block translated");
    check(env.gpr[10], 1, "C.J skips the next instruction");
    diff_cj = env.gpr[10];
    check(env.pc,
          (uint64_t)(uintptr_t)(compressed_jump + sizeof(compressed_jump)),
          "C.J block preserves mixed-width sequential PC");
    latx_bare_uart_puts("latr-isa: compressed jump done\n");
    latx_bare_uart_puts("latr-isa: compressed done\n");
}
static void test_fp_rmm(void)
{
    static const uint32_t rmm_static[] = {
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FMV_W_X_F2_X2,
        LATR_DIFF_FADD_S_RMM,
        LATR_DIFF_FMV_X_W_X3_F3,
        LATR_DIFF_CSRRS_X5_FFLAGS,
    };
    latx_bare_uart_puts("latr-isa: fp rmm static begin\n");
    check(run_block_with_regs(rmm_static,
                              sizeof(rmm_static) / sizeof(rmm_static[0]),
                              1, 0x3f800000, 2, 0x33800000),
          1, "static RMM block translated");
    check(env.gpr[3], 0x3f800001, "static RMM rounds halfway away");
    check(env.gpr[5], 1, "static RMM records inexact flag");
    check(env.fpr[3], 0xffffffff3f800001ULL,
          "single result is NaN-boxed in floating register");
    check(env.fcsr, 1, "static RMM accrues inexact flag");
    diff_fp_rmm = env.gpr[3];
    latx_bare_uart_puts("latr-isa: fp rmm static done\n");

    static const uint32_t rmm_dynamic[] = {
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FMV_W_X_F2_X2,
        LATR_DIFF_CSRRW_X0_FFLAGS_X0,
        LATR_DIFF_CSRRWI_X0_FRM_4,
        LATR_DIFF_FADD_S_DYN_F4,
        LATR_DIFF_FMV_X_W_X6_F4,
        LATR_DIFF_CSRRS_X7_FFLAGS,
        LATR_DIFF_CSRRW_X0_FFLAGS_X0,
        LATR_DIFF_FADD_S_DYN_F5,
        LATR_DIFF_CSRRS_X11_FFLAGS,
    };
    latx_bare_uart_puts("latr-isa: fp rmm dynamic begin\n");
    check(run_block_with_regs(rmm_dynamic,
                              sizeof(rmm_dynamic) / sizeof(rmm_dynamic[0]),
                              1, 0x3f800000, 2, 0x33800000),
          1, "dynamic RMM block translated");
    check(env.gpr[6], 0x3f800001, "dynamic RMM rounds halfway away");
    check(env.gpr[7], 1, "dynamic RMM records inexact flag");
    check(env.gpr[11], 0, "cleared host flags stay cleared after exact op");
    check(env.fcsr, 0x80, "RMM remains selected with flags cleared");
    diff_fp_rmm_dynamic = env.gpr[6];
    latx_bare_uart_puts("latr-isa: fp rmm dynamic done\n");
}
static void test_fp_rmm_conversion(void)
{
    static const uint32_t rmm_conversion[] = {
        LATR_DIFF_LUI_X1_40200,
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FCVT_W_S_RMM,
        LATR_DIFF_CSRRS_X5_FFLAGS,
    };
    latx_bare_uart_puts("latr-isa: conv begin\n");
    check(run_block(rmm_conversion,
                    sizeof(rmm_conversion) / sizeof(rmm_conversion[0]),
                    -1, 0),
          1, "RMM integer conversion block translated");
    latx_bare_uart_puts("latr-isa: conv block done\n");
    check(env.gpr[3], 3, "FCVT.W.S RMM rounds 2.5 away from zero");
    check(env.gpr[5], 1, "RMM integer conversion accrues NX");
    check(env.fcsr, 1, "RMM conversion preserves accrued flags");
    static const uint32_t x0_conversion[] = {
        LATR_DIFF_LUI_X1_40200,
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FCVT_W_S_RMM_X0,
    };
    check(run_block(x0_conversion,
                    sizeof(x0_conversion) / sizeof(x0_conversion[0]),
                    -1, 0),
          1, "RMM conversion to x0 block translated");
    check(env.gpr[0], 0, "RMM integer conversion preserves x0");
    check(env.fcsr, 1, "RMM conversion to x0 still accrues NX");
}

static void test_fp_double(void)
{
    static const uint32_t dadd[] = {
        LATR_DIFF_FMV_D_X_F1_X1,
        LATR_DIFF_FMV_D_X_F2_X2,
        LATR_DIFF_FADD_D_RNE,
        LATR_DIFF_FMV_X_D_X3_F3,
    };
    check(run_block_with_regs(dadd, sizeof(dadd) / sizeof(dadd[0]),
                              1, UINT64_C(0x3ff0000000000000),
                              2, UINT64_C(0x4000000000000000)),
          1, "double-precision arithmetic block translated");
    check(env.gpr[3], UINT64_C(0x4008000000000000),
          "FADD.D computes 1.0 plus 2.0");
    check(env.fpr[3], UINT64_C(0x4008000000000000),
          "FADD.D writes full-width result");
    check(env.fcsr, 0, "exact FADD.D leaves accrued flags clear");
}

static void test_fp_rmm_division(void)
{
    static const uint32_t rmm_div[] = {
        LATR_DIFF_FMV_D_X_F1_X1,
        LATR_DIFF_FMV_D_X_F2_X2,
        LATR_DIFF_FDIV_D_RMM,
        LATR_DIFF_FMV_X_D_X3_F3,
        LATR_DIFF_CSRRS_X5_FFLAGS,
    };
    check(run_block_with_regs(rmm_div,
                              sizeof(rmm_div) / sizeof(rmm_div[0]),
                              1, UINT64_C(0x3ff0000000000000),
                              2, UINT64_C(0x4008000000000000)),
          1, "RMM double-division block translated");
    check(env.gpr[3], UINT64_C(0x3fd5555555555555),
          "FDIV.D RMM rounds 1/3 to nearest representable result");
    check(env.gpr[5], 1, "RMM double division accrues NX");
    check(env.fcsr, 1, "RMM double division preserves accrued NX");
    diff_fp_div_rmm = env.gpr[3];
}
static void test_fp_fma(void)
{
    static const uint32_t fmadd[] = {
        LATR_DIFF_FMV_D_X_F1_X1,
        LATR_DIFF_FMV_D_X_F2_X2,
        LATR_DIFF_FADD_D_RNE,
        LATR_DIFF_FMADD_D_F4_F1_F2_F3,
        LATR_DIFF_FMV_X_D_X3_F4,
    };
    check(run_block_with_regs(fmadd, sizeof(fmadd) / sizeof(fmadd[0]),
                              1, UINT64_C(0x3ff0000000000000),
                              2, UINT64_C(0x4000000000000000)),
          1, "double fused multiply-add block translated");
    check(env.gpr[3], UINT64_C(0x4014000000000000),
          "FMADD.D computes 1.0 times 2.0 plus 3.0");
    check(env.fpr[4], UINT64_C(0x4014000000000000),
          "FMADD.D writes full-width fused result");
    check(env.fcsr, 0, "exact FMADD.D leaves accrued flags clear");
    diff_fp_fma = env.gpr[3];
    static const uint32_t invalid_rmm_fmadd_s[] = {
        LATR_DIFF_FMV_W_X_F2_X2,
        0xf00181d3, /* fmv.w.x f3, x3 */
        0x1820c243, /* fmadd.s f4, f1, f2, f3, rmm */
        LATR_DIFF_CSRRS_X5_FFLAGS,
    };
    check(run_block_with_regs_and_fpr(
              invalid_rmm_fmadd_s,
              sizeof(invalid_rmm_fmadd_s) / sizeof(invalid_rmm_fmadd_s[0]),
              2, 0, 3, 0x7fc00000, 1, UINT64_C(0xffffffff7f800000)),
          1, "RMM FMADD.S Inf-times-zero block translated");
    check(env.gpr[5], 0x10,
          "RMM FMADD.S Inf-times-zero raises NV with qNaN addend");
    check(env.fcsr, 0x10, "RMM FMADD.S accrues invalid-operation flag");
}

static void test_fp_nan_semantics(void)
{
    static const uint32_t qnan_min[] = {
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FMV_W_X_F2_X2,
        LATR_DIFF_FMIN_S_F3_F1_F2,
        LATR_DIFF_FMV_X_W_X3_F3,
        LATR_DIFF_CSRRS_X5_FFLAGS,
        LATR_DIFF_FCLASS_S_X4_F1,
    };
    check(run_block_with_regs(qnan_min,
                              sizeof(qnan_min) / sizeof(qnan_min[0]),
                              1, 0x7fc00000, 2, 0x3f800000),
          1, "quiet-NaN FMIN block translated");
    check(env.gpr[3], 0x3f800000, "FMIN returns numeric operand for qNaN");
    check(env.gpr[5], 0, "quiet NaN does not raise NV in FMIN");
    check(env.gpr[4], 0x200, "FCLASS marks quiet NaN");

    static const uint32_t snan_min[] = {
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FMV_W_X_F2_X2,
        LATR_DIFF_FMIN_S_F3_F1_F2,
        LATR_DIFF_FMV_X_W_X3_F3,
        LATR_DIFF_CSRRS_X5_FFLAGS,
        LATR_DIFF_FCLASS_S_X4_F1,
    };
    check(run_block_with_regs(snan_min,
                              sizeof(snan_min) / sizeof(snan_min[0]),
                              1, 0x7f800001, 2, 0x3f800000),
          1, "signaling-NaN FMIN block translated");
    check(env.gpr[3], 0x3f800000, "FMIN returns number for sNaN");
    check(env.gpr[5], 0x10, "signaling NaN raises NV in FMIN");
    check(env.gpr[4], 0x100, "FCLASS marks signaling NaN");
    static const uint32_t signed_zero_minmax[] = {
        LATR_DIFF_FMV_W_X_F1_X1,
        LATR_DIFF_FMV_W_X_F2_X2,
        LATR_DIFF_FMIN_S_F3_F1_F2,
        LATR_DIFF_FMV_X_W_X3_F3,
        LATR_DIFF_FMAX_S_F3_F1_F2,
        LATR_DIFF_FMV_X_W_X6_F3,
        LATR_DIFF_FCLASS_S_X4_F1,
    };
    check(run_block_with_regs(signed_zero_minmax,
                              sizeof(signed_zero_minmax) /
                                  sizeof(signed_zero_minmax[0]),
                              1, 0x80000000, 2, 0),
          1, "signed-zero min/max block translated");
    check(env.gpr[3], UINT64_C(0xffffffff80000000),
          "FMIN preserves negative zero");
    check(env.gpr[6], 0, "FMAX preserves positive zero");
    check(env.gpr[4], 0x8, "FCLASS identifies negative zero");
    check(run_block_with_regs(signed_zero_minmax,
                              sizeof(signed_zero_minmax) /
                                  sizeof(signed_zero_minmax[0]),
                              1, 0x80000000, 2, 0x80000000),
          1, "two-negative-zero min/max block translated");
    check(env.gpr[3], UINT64_C(0xffffffff80000000),
          "FMIN preserves -0 when both operands are -0");
    check(env.gpr[6], UINT64_C(0xffffffff80000000),
          "FMAX preserves -0 when both operands are -0");

    static const uint32_t fcvt_wu_x0_negative[] = {
        0xc0108053, /* fcvt.wu x0, f1, rne */
        LATR_DIFF_CSRRS_X5_FFLAGS,
    };
    check(run_block_with_regs_and_fpr(
              fcvt_wu_x0_negative,
              sizeof(fcvt_wu_x0_negative) /
                  sizeof(fcvt_wu_x0_negative[0]),
              -1, 0, -1, 0, 1, UINT64_C(0xffffffffbf800000)),
          1, "FCVT.WU.S x0 negative-input block translated");
    check(env.gpr[5], 0x10, "FCVT.WU.S x0 still raises NV");
    check(env.fcsr, 0x10, "FCVT.WU.S x0 accrues invalid-operation flag");
    check(env.gpr[0], 0, "FCVT.WU.S x0 preserves the zero register");

    static const uint32_t fcvt_lu_x0_negative[] = {
        0xc2308053, /* fcvt.lu.d x0, f1, rne */
        LATR_DIFF_CSRRS_X5_FFLAGS,
    };
    check(run_block_with_regs_and_fpr(
              fcvt_lu_x0_negative,
              sizeof(fcvt_lu_x0_negative) /
                  sizeof(fcvt_lu_x0_negative[0]),
              -1, 0, -1, 0, 1, UINT64_C(0xbff0000000000000)),
          1, "FCVT.LU.D x0 negative-input block translated");
    check(env.gpr[5], 0x10, "FCVT.LU.D x0 still raises NV");
    check(env.fcsr, 0x10, "FCVT.LU.D x0 accrues invalid-operation flag");
    check(env.gpr[0], 0, "FCVT.LU.D x0 preserves the zero register");
}

static void test_fp_reserved_rounding(void)
{
    static const uint32_t reserved_static[] = {0x0020d1d3};
    check(run_block(reserved_static, 1, -1, 0), 1,
          "reserved static rounding-mode block translated");
    check(env.exception, 2, "reserved static rm raises illegal instruction");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)reserved_static,
          "reserved static rm records faulting PC");
    check(env.badaddr, reserved_static[0],
          "reserved static rm records faulting instruction");

    static const uint32_t reserved_dynamic[] = {
        0x0022d073, /* csrrwi x0, frm, 5 */
        0x0020f1d3, /* fadd.s f3, f1, f2, dyn */
    };
    check(run_block(reserved_dynamic, 2, -1, 0), 1,
          "reserved dynamic rounding-mode block translated");
    check(env.exception, 2, "reserved dynamic frm raises illegal instruction");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)(reserved_dynamic + 1),
          "reserved dynamic frm records faulting PC");
    check(env.badaddr, reserved_dynamic[1],
          "reserved dynamic frm records faulting instruction");
}





static void test_csr(void)
{
    static const uint32_t custom_csr[] = {
        0x00f00093, /* addi x1, x0, 15 */
        0x800091f3, /* csrrw x3, 0x800, x1 */
        0x03000113, /* addi x2, x0, 48 */
        0x80012273, /* csrrs x4, 0x800, x2 */
        0x00c00313, /* addi x6, x0, 12 */
        0x800332f3, /* csrrc x5, 0x800, x6 */
    };
    check(run_block(custom_csr, 6, -1, 0), 1,
          "custom CSR block translated");
    check(env.gpr[3], 0, "CSRRW returns old CSR value");
    check(env.gpr[4], 15, "CSRRS returns old CSR value");
    check(env.gpr[5], 63, "CSRRC returns old CSR value");
    check(env.csr[0x800], 51, "CSR set and clear update storage");

    static const uint32_t csr_immediate[] = {
        0x8000d073, /* csrrwi x0, 0x800, 1 */
        0x800261f3, /* csrrsi x3, 0x800, 4 */
        0x8000f273, /* csrrci x4, 0x800, 1 */
    };
    check(run_block(csr_immediate, 3, -1, 0), 1,
          "immediate CSR block translated");
    check(env.gpr[3], 1, "CSRRSI returns old CSR value");
    check(env.gpr[4], 5, "CSRRCI returns old CSR value");
    check(env.csr[0x800], 4, "CSR immediate set and clear work");

    static const uint32_t fcsr_aliases[] = {
        0x01f00093, /* addi x1, x0, 31 */
        0x001091f3, /* csrrw x3, fflags, x1 */
        0x00225273, /* csrrwi x4, frm, 4 */
        0x003022f3, /* csrrs x5, fcsr, x0 */
    };
    check(run_block(fcsr_aliases, 4, -1, 0), 1,
          "floating CSR alias block translated");
    check(env.gpr[3], 0, "FFLAGS write returns old flags");
    check(env.gpr[4], 0, "FRM write returns old rounding mode");
    check(env.fcsr, 0x9f, "FFLAGS and FRM alias FCSR state");
    check(env.gpr[5], 0x9f, "FCSR read returns both aliases");
    static const uint32_t unsupported_user_csr[] = {0x004021f3};
    check(run_block(unsupported_user_csr, 1, -1, 0), 1,
          "unsupported user CSR block translated to trap");
    check(env.exception, 2, "unsupported user CSR raises illegal instruction");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)unsupported_user_csr,
          "unsupported CSR trap records faulting PC");
    check(env.badaddr, unsupported_user_csr[0],
          "unsupported CSR trap records instruction bits");

    static const uint32_t privileged_csr[] = {0x30009073};
    check(run_block_with_regs(privileged_csr, 1, 1, 1, -1, 0), 1,
          "privileged CSR block translated to trap");
    check(env.exception, 2, "user access to machine CSR is illegal");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)privileged_csr,
          "privileged CSR trap records faulting PC");
    check(env.badaddr, privileged_csr[0],
          "privileged CSR trap records instruction bits");
}


static void test_hpm_stateen(void)
{
    static const uint32_t block[] = {
        0x323091f3, /* csrrw x3, mhpmevent3, x1 */
        0x32302273, /* csrrs x4, mhpmevent3, x0 */
        0xb0309073, /* csrrw x0, mhpmcounter3, x1 */
        0xb03022f3, /* csrrs x5, mhpmcounter3, x0 */
        0xc0302373, /* csrrs x6, hpmcounter3, x0 */
        0x30c09073, /* csrrw x0, mstateen0, x1 */
        0x30c023f3, /* csrrs x7, mstateen0, x0 */
        0x32009073, /* csrrw x0, mcountinhibit, x1 */
        0x32002473, /* csrrs x8, mcountinhibit, x0 */
        0x30d094f3, /* csrrw x9, mstateen1, x1 */
        0x30d02573, /* csrrs x10, mstateen1, x0 */
        0x33f09073, /* csrrw x0, mhpmevent31, x1 */
        0x33f025f3, /* csrrs x11, mhpmevent31, x0 */
        0xb1f09073, /* csrrw x0, mhpmcounter31, x1 */
        0xc1f02673, /* csrrs x12, hpmcounter31, x0 */
    };
    uint64_t all = UINT64_MAX;

    reset_guest_state();
    env.privilege = LATR_RISCV_PRIV_M;
    env.gpr[1] = all;
    check(latr_execute_block(block, 15), 1,
          "HPM/stateen block translated");
    check(env.gpr[3], 0, "mhpmevent3 write returns reset value");
    check(env.gpr[4], all, "mhpmevent3 stores the written value");
    check(env.gpr[5], all, "mhpmcounter3 stores the written value");
    check(env.gpr[6], all, "hpmcounter3 aliases mhpmcounter3");
    check(env.gpr[7], UINT64_C(0xc000000000000002),
          "mstateen0 keeps only supported gating bits");
    check(env.gpr[8], UINT64_C(0xfffffffd),
          "mcountinhibit keeps counter inhibit bits except TIME");
    check(env.gpr[9], 0, "mstateen1 write returns reset value");
    check(env.gpr[10], UINT64_C(0x8000000000000000),
          "mstateen1 keeps only the STATEEN bit");
    check(env.gpr[11], all, "mhpmevent31 stores the written value");
    check(env.gpr[12], all, "hpmcounter31 aliases mhpmcounter31");

    static const uint32_t illegal_write[] = {0xc0309073};
    reset_guest_state();
    env.privilege = LATR_RISCV_PRIV_M;
    env.gpr[1] = 1;
    check(latr_execute_block(illegal_write, 1), 1,
          "hpmcounter write block translated to trap");
    check(env.exception, 2, "hpmcounter is read-only");

    static const uint32_t read_hpm[] = {0xc03022f3};
    reset_guest_state();
    env.privilege = 0;
    env.csr[0x306] = UINT64_C(1) << 3;
    env.csr[0x106] = UINT64_C(1) << 3;
    env.csr[0xb03] = 0x123;
    check(latr_execute_block(read_hpm, 1), 1,
          "U-mode hpmcounter read translated");
    check(env.gpr[5], 0x123, "mcounteren bit 3 permits hpmcounter3 in U-mode");

    reset_guest_state();
    env.privilege = 0;
    env.csr[0x106] = UINT64_C(1) << 3;
    env.csr[0xb03] = 0x456;
    check(latr_execute_block(read_hpm, 1), 1,
          "blocked U-mode hpmcounter read translated to trap");
    check(env.exception, 2,
          "mcounteren bit 3 clear traps U-mode hpmcounter3");
}

static void test_system_traps(void)
{
    static const uint32_t ecall[] = {0x00000073};
    check(run_block(ecall, 1, -1, 0), 1, "ECALL block translated");
    check(env.exception, 8, "ECALL records user exception");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)ecall, "ECALL records faulting PC");
    check(env.badaddr, 0, "ECALL leaves trap value zero");

    static const uint32_t ebreak[] = {0x00100073};
    check(run_block(ebreak, 1, -1, 0), 1, "EBREAK block translated");
    check(env.exception, 3, "EBREAK records breakpoint exception");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)ebreak, "EBREAK records faulting PC");
    check(env.badaddr, (uint64_t)(uintptr_t)ebreak,
          "EBREAK records trap value");

    static const uint16_t c_ebreak = 0x9002;
    check(run_block(&c_ebreak, 1, -1, 0), 1,
          "C.EBREAK block translated");
    check(env.exception, 3, "C.EBREAK records breakpoint exception");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)&c_ebreak,
          "C.EBREAK records faulting PC");
    check(env.badaddr, (uint64_t)(uintptr_t)&c_ebreak,
          "C.EBREAK records trap value");
    static uint32_t fence_i[] = {
        0x00112023, /* sw x1, 0(x2) */
        0x0000100f, /* fence.i */
        0x00100193, /* addi x3, x0, 1; replaced before it executes */
    };
    check(run_block_with_regs(fence_i, 3,
                              1, 0x00700193,
                              2, (uint64_t)(uintptr_t)&fence_i[2]),
          1, "FENCE.I self-modifying block translated");
    check(fence_i[2], 0x00700193, "preceding store changed guest code");
    check(env.gpr[3], 0, "FENCE.I exits before stale translated code");
    check(env.pc, (uint64_t)(uintptr_t)&fence_i[2],
          "FENCE.I advances PC to next instruction");
    check(run_block(fence_i + 2, 1, -1, 0), 1,
          "post-FENCE.I code re-decodes");
    check(env.gpr[3], 7, "post-FENCE.I translation sees modified code");
}

static void test_illegal_decode(void)
{
    static const uint32_t illegal[] = {
        0x00500093, /* addi x1, x0, 5 */
        0xffffffff, /* undecodable encoding */
        0x00700113, /* addi x2, x0, 7; must not execute */
    };
    check(run_block(illegal, 2, -1, 0), 1,
          "block with undecodable instruction translates");
    check(env.gpr[1], 5, "instruction before illegal decode executes");
    check(env.exception, 2, "undecodable instruction raises illegal trap");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)(illegal + 1),
          "illegal decode records faulting PC");
    check(env.badaddr, 0xffffffff, "illegal decode records instruction bits");
    check(env.gpr[2], 0, "illegal decode stops the block");
}

static void test_atomics(void)
{
    data_word = UINT64_C(0x1122334455667788);
    static const uint32_t lr_sc[] = {
        0x100332af, /* lr.d x5, (x6) */
        0x03300493, /* addi x9, x0, 0x33 */
        0x189333af, /* sc.d x7, x9, (x6) */
        0x189335af, /* sc.d x11, x9, (x6) */
    };
    check(run_block(lr_sc, 4, 6, (uint64_t)(uintptr_t)&data_word), 1,
          "LR/SC block translated");
    check(env.gpr[5], UINT64_C(0x1122334455667788),
          "LR.D returns memory value");
    check(env.gpr[7], 0, "SC.D succeeds after LR.D");
    check(data_word, 0x33, "successful SC.D stores the value");
    check(env.gpr[11], 1, "SC.D fails once the reservation is consumed");

    data_word = UINT64_C(0x1122334455667788);
    static const uint32_t lr_store_sc[] = {
        0x100332af, /* lr.d x5, (x6) */
        0x05500413, /* addi x8, x0, 0x55 */
        0x00833023, /* sd x8, 0(x6) */
        0x03300493, /* addi x9, x0, 0x33 */
        0x189333af, /* sc.d x7, x9, (x6) */
    };
    check(run_block(lr_store_sc, 5, 6, (uint64_t)(uintptr_t)&data_word), 1,
          "LR/store/SC block translated");
    check(env.gpr[5], UINT64_C(0x1122334455667788),
          "LR.D before intervening store returns memory value");
    check(env.gpr[7], 1, "intervening store invalidates the reservation");
    check(data_word, 0x55, "failed SC.D leaves the store value");

    data_word = UINT64_C(0x1122334455667788);
    static const uint32_t sc_without_lr[] = {
        0x03300493, /* addi x9, x0, 0x33 */
        0x189333af, /* sc.d x7, x9, (x6) */
    };
    check(run_block(sc_without_lr, 2, 6, (uint64_t)(uintptr_t)&data_word), 1,
          "SC without LR block translated");
    check(env.gpr[7], 1, "SC.D without a reservation fails");
    check(data_word, UINT64_C(0x1122334455667788),
          "failed SC.D without reservation leaves memory unchanged");

    data_word = UINT64_C(0xffffffff80000000);
    static const uint32_t lr_w_sc_w[] = {
        0x100322af, /* lr.w x5, (x6) */
        0x04400493, /* addi x9, x0, 0x44 */
        0x189323af, /* sc.w x7, x9, (x6) */
    };
    check(run_block(lr_w_sc_w, 3, 6, (uint64_t)(uintptr_t)&data_word), 1,
          "word LR/SC block translated");
    check(env.gpr[5], UINT64_C(0xffffffff80000000),
          "LR.W sign-extends the loaded word");
    check(env.gpr[7], 0, "SC.W succeeds after LR.W");
    check(data_word, UINT64_C(0xffffffff00000044),
          "SC.W replaces only the low word");

    data_word = 100;
    static const uint32_t amo_add_d[] = {
        0x008332af, /* amoadd.d x5, x8, (x6) */
    };
    check(run_block_with_regs(amo_add_d, 1, 6, (uint64_t)(uintptr_t)&data_word,
                              8, 7),
          1, "AMOADD.D block translated");
    check(env.gpr[5], 100, "AMOADD.D returns the old value");
    check(data_word, 107, "AMOADD.D stores old plus addend");

    data_word = UINT64_C(0xffffffff);
    static const uint32_t amo_add_w[] = {
        0x008322af, /* amoadd.w x5, x8, (x6) */
    };
    check(run_block_with_regs(amo_add_w, 1, 6, (uint64_t)(uintptr_t)&data_word,
                              8, 1),
          1, "AMOADD.W block translated");
    check(env.gpr[5], UINT64_MAX,
          "AMOADD.W sign-extends the 32-bit old value");
    check(data_word, 0, "AMOADD.W stores the wrapped low word");

    data_word = 0;
    static const uint32_t lr_only[] = {
        0x100332af, /* lr.d x5, (x6) */
    };
    check(run_block(lr_only, 1, 6, (uint64_t)(uintptr_t)&data_word), 1,
          "standalone LR.D block translated");
    check(env.reservation_valid, 1, "LR.D arms the reservation");

    static const uint32_t lr_then_ecall[] = {
        0x100332af, /* lr.d x5, (x6) */
        0x00000073, /* ecall */
    };
    check(run_block(lr_then_ecall, 2, 6, (uint64_t)(uintptr_t)&data_word), 1,
          "LR then ECALL block translated");
    check(env.exception, 8, "ECALL after LR.D traps");
    check(env.reservation_valid, 0, "taking a trap drops the reservation");

    /* Atomics must be naturally aligned: cause 4 for loads, 6 for stores. */
    uint64_t unaligned = (uint64_t)(uintptr_t)&data_word + 4;
    check(run_block(lr_only, 1, 6, unaligned), 1,
          "misaligned LR.D block translated");
    check(env.exception, 4, "misaligned LR.D raises load-misaligned");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)lr_only,
          "misaligned LR.D saves the faulting PC");
    check(env.badaddr, unaligned, "misaligned LR.D reports the address");
    check(env.gpr[5], 0, "misaligned LR.D leaves the destination alone");
    check(env.reservation_valid, 0,
          "misaligned LR.D does not arm a reservation");

    static const uint32_t sc_only[] = {
        0x189333af, /* sc.d x7, x9, (x6) */
    };
    check(run_block(sc_only, 1, 6, unaligned), 1,
          "misaligned SC.D block translated");
    check(env.exception, 6, "misaligned SC.D raises store-misaligned");
    check(env.badaddr, unaligned, "misaligned SC.D reports the address");
    check(env.gpr[7], 0, "misaligned SC.D leaves the result alone");

    uint64_t odd = (uint64_t)(uintptr_t)&data_word + 1;
    check(run_block(amo_add_d, 1, 6, odd), 1,
          "misaligned AMOADD.D block translated");
    check(env.exception, 6, "misaligned AMOADD.D raises store-misaligned");
    check(env.badaddr, odd, "misaligned AMOADD.D reports the address");
    check(data_word, 0, "misaligned AMOADD.D leaves memory unchanged");

    /* Writes through the unified guest interface must join the same
     * reservation domain as scalar stores. */
    data_word = UINT64_C(0x1122334455667788);
    other_data_word = 0;
    reset_guest_state();
    env.gpr[6] = (uint64_t)(uintptr_t)&data_word;
    check(latr_execute_block(lr_only, 1), 1,
          "LR before guest_store translates");
    check(env.reservation_valid, 1, "LR arms the reservation");
    check(latr_guest_store(&env, (uint64_t)(uintptr_t)&other_data_word, 8,
                           UINT64_C(0x55)), 0,
          "guest_store joins the reservation domain");
    check(latr_execute_block(sc_only, 1), 1,
          "SC after guest_store translates");
    check(env.gpr[7], 1, "guest_store invalidates the reservation");
    check(data_word, UINT64_C(0x1122334455667788),
          "failed SC after guest_store leaves memory unchanged");
    check(other_data_word, UINT64_C(0x55), "guest_store wrote its target");

    /* The vector store path funnels through guest_store as well. */
    data_word = UINT64_C(0x1122334455667788);
    other_data_word = 0;
    reset_guest_state();
    env.gpr[6] = (uint64_t)(uintptr_t)&data_word;
    check(latr_execute_block(lr_only, 1), 1,
          "LR before RVV store translates");
    env.vtype = 0x18; /* SEW=64, LMUL=1 */
    env.vl = 1;
    env.vstart = 0;
    env.rvv_args[0] = (uint64_t)(uintptr_t)&other_data_word;
    env.rvv_args[1] = 8; /* vd */
    env.rvv_args[2] = 3; /* EEW=64 */
    env.rvv_args[3] = 0; /* store */
    env.rvv_args[4] = 1; /* unmasked */
    env.rvv_args[5] = 0;
    for (int i = 0; i < 8; i++) {
        env.vreg[8][i] = (uint8_t)(0x60 + i);
    }
    check(latr_rvv_mem(&env), 0, "RVV store after LR runs");
    check(latr_execute_block(sc_only, 1), 1,
          "SC after RVV store translates");
    check(env.gpr[7], 1, "RVV store invalidates the reservation");
    check(data_word, UINT64_C(0x1122334455667788),
          "failed SC after RVV store leaves memory unchanged");
    check(other_data_word, UINT64_C(0x6766656463626160),
          "RVV store wrote its target");

    /* Non-CPU writers use the explicit notification entry point. */
    data_word = UINT64_C(0x1122334455667788);
    reset_guest_state();
    env.gpr[6] = (uint64_t)(uintptr_t)&data_word;
    check(latr_execute_block(lr_only, 1), 1,
          "LR before device notification translates");
    latr_reservation_notify_write((uint64_t)(uintptr_t)&other_data_word, 8);
    check(latr_execute_block(sc_only, 1), 1,
          "SC after device notification translates");
    check(env.gpr[7], 1, "device notification invalidates the reservation");
    check(data_word, UINT64_C(0x1122334455667788),
          "failed SC after device notification leaves memory unchanged");

    /* A notification raised while a writer holds the lock must return without
     * spinning; the holder's own release advances the epoch. */
    data_word = UINT64_C(0x1122334455667788);
    reset_guest_state();
    env.gpr[6] = (uint64_t)(uintptr_t)&data_word;
    uint64_t held_epoch = latr_guest_store_lock;
    check(latr_execute_block(lr_only, 1), 1,
          "LR before nested notification translates");
    check(env.reservation_valid, 1, "LR arms before the nested notification");
    latr_guest_store_lock = held_epoch + 1; /* writer in flight */
    latr_reservation_notify_write((uint64_t)(uintptr_t)&other_data_word, 8);
    check(latr_guest_store_lock, held_epoch + 1,
          "notification leaves a held lock untouched");
    latr_guest_store_lock = held_epoch + 2; /* writer release */
    check(latr_execute_block(sc_only, 1), 1,
          "SC after nested notification translates");
    check(env.gpr[7], 1,
          "writer release after nested notification invalidates the reservation");
    check(data_word, UINT64_C(0x1122334455667788),
          "failed SC after nested notification leaves memory unchanged");
}

static void test_bitmanip(void)
{
    /* Each vector and expected value matches the golden reference run. */
    static const uint32_t orc_b[] = {0x2870d293};   /* orc.b x5, x1 */
    check(run_block(orc_b, 1, 1, 0x010100ff), 1, "ORC.B block translated");
    check(env.gpr[5], 0xffff00ff, "ORC.B expands per-byte non-zero");

    static const uint32_t clz[] = {0x60009293};     /* clz x5, x1 */
    check(run_block(clz, 1, 1, 1), 1, "CLZ block translated");
    check(env.gpr[5], 63, "CLZ counts leading zeros");

    static const uint32_t ctz[] = {0x60109293};     /* ctz x5, x1 */
    check(run_block(ctz, 1, 1, 0x100), 1, "CTZ block translated");
    check(env.gpr[5], 8, "CTZ counts trailing zeros");

    static const uint32_t cpop[] = {0x60209293};    /* cpop x5, x1 */
    check(run_block(cpop, 1, 1, 0xf0f0), 1, "CPOP block translated");
    check(env.gpr[5], 8, "CPOP counts set bits");

    static const uint32_t rolw[] = {0x602092bb};    /* rolw x5, x1, x2 */
    check(run_block_with_regs(rolw, 1, 1, 0x80000000, 2, 1), 1,
          "ROLW block translated");
    check(env.gpr[5], 1, "ROLW rotates within the low word");

    static const uint32_t sext_b[] = {0x60409293};  /* sext.b x5, x1 */
    check(run_block(sext_b, 1, 1, 0x80), 1, "SEXT.B block translated");
    check(env.gpr[5], UINT64_C(0xffffffffffffff80),
          "SEXT.B sign-extends the low byte");

    static const uint32_t zext_h[] = {0x0800c2bb};  /* zext.h x5, x1 */
    check(run_block(zext_h, 1, 1, UINT64_C(0xffff0000ffff)), 1,
          "ZEXT.H block translated");
    check(env.gpr[5], 0xffff, "ZEXT.H keeps only the low half");

    static const uint32_t rev8[] = {0x6b80d293};    /* rev8 x5, x1 */
    check(run_block(rev8, 1, 1, UINT64_C(0x1122334455667788)), 1,
          "REV8 block translated");
    check(env.gpr[5], UINT64_C(0x8877665544332211),
          "REV8 reverses all eight bytes");

    static const uint32_t min_insn[] = {0x0a20c2b3}; /* min x5, x1, x2 */
    static const uint32_t maxu_insn[] = {0x0a20f2b3}; /* maxu x5, x1, x2 */
    check(run_block_with_regs(min_insn, 1, 1, UINT64_MAX, 2, 1), 1,
          "MIN block translated");
    check(env.gpr[5], UINT64_MAX, "MIN compares signed values");
    check(run_block_with_regs(maxu_insn, 1, 1, UINT64_MAX, 2, 1), 1,
          "MAXU block translated");
    check(env.gpr[5], UINT64_MAX, "MAXU compares unsigned values");

    static const uint32_t bexti[] = {0x4850d293};   /* bexti x5, x1, 5 */
    check(run_block(bexti, 1, 1, 0x20), 1, "BEXTI block translated");
    check(env.gpr[5], 1, "BEXTI extracts the addressed bit");

    static const uint32_t bseti[] = {0x28309293};   /* bseti x5, x1, 3 */
    check(run_block(bseti, 1, 1, 0), 1, "BSETI block translated");
    check(env.gpr[5], 8, "BSETI sets the addressed bit");

    static const uint32_t binvi[] = {0x68009293};   /* binvi x5, x1, 0 */
    check(run_block(binvi, 1, 1, 1), 1, "BINVI block translated");
    check(env.gpr[5], 0, "BINVI inverts the addressed bit");

    static const uint32_t sh1add[] = {0x2020a2b3};  /* sh1add x5, x1, x2 */
    check(run_block_with_regs(sh1add, 1, 1, 3, 2, 5), 1,
          "SH1ADD block translated");
    check(env.gpr[5], 11, "SH1ADD computes rs1*2 + rs2");

    static const uint32_t add_uw[] = {0x082082bb};  /* add.uw x5, x1, x2 */
    check(run_block_with_regs(add_uw, 1, 1, UINT64_C(0xffffffff), 2, 1), 1,
          "ADD.UW block translated");
    check(env.gpr[5], UINT64_C(0x100000000),
          "ADD.UW zero-extends the low word before adding");

    static const uint32_t slli_uw[] = {0x0840929b}; /* slli.uw x5, x1, 4 */
    check(run_block(slli_uw, 1, 1, UINT64_C(0xffffffff)), 1,
          "SLLI.UW block translated");
    check(env.gpr[5], UINT64_C(0xffffffff0),
          "SLLI.UW shifts a zero-extended word");

    static const uint32_t roriw[] = {0x6040d29b};   /* roriw x5, x1, 4 */
    check(run_block(roriw, 1, 1, 0xf), 1, "RORIW block translated");
    check(env.gpr[5], UINT64_C(0xfffffffff0000000),
          "RORIW sign-extends the rotated word");
}

static void test_privileged(void)
{
    static const uint32_t mret[] = {0x30200073};
    reset_guest_state();
    env.csr[0x300] = UINT64_C(1) << 11;    /* MPP = S */
    env.csr[0x341] = UINT64_C(0x80001234); /* mepc */
    env.privilege = 3;
    check(latr_execute_block(mret, 1), 1, "MRET block translated");
    check(env.privilege, 1, "MRET enters S-mode");
    check((env.csr[0x300] >> 11) & 3, 0, "MRET clears MPP");
    check((env.csr[0x300] >> 7) & 1, 1, "MRET restores MIE from MPIE");
    check(env.pc, UINT64_C(0x80001234), "MRET jumps to mepc");

    static const uint32_t sret[] = {0x10200073};
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x141] = UINT64_C(0x80005678); /* sepc */
    check(latr_execute_block(sret, 1), 1, "SRET block translated");
    check(env.privilege, 0, "SRET enters U-mode");
    check(env.pc, UINT64_C(0x80005678), "SRET jumps to sepc");

    reset_guest_state();
    check(latr_execute_block(sret, 1), 1, "U-mode SRET translates to a trap");
    check(env.exception, 2, "U-mode SRET raises illegal instruction");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)sret,
          "U-mode SRET saves the faulting PC");

    static const uint32_t wfi[] = {0x10500073};
    check(run_block(wfi, 1, -1, 0), 1,
          "WFI in U-mode block translated");
    check(env.exception, 0, "WFI in U-mode retires without a trap");
    check(env.pc, (uint64_t)(uintptr_t)(wfi + 1), "WFI falls through");
    reset_guest_state();
    env.csr[0x300] = UINT64_C(1) << 21; /* TW */
    check(latr_execute_block(wfi, 1), 1, "WFI with TW translates to a trap");
    check(env.exception, 2, "WFI with TW in U-mode is illegal");

    static const uint32_t sfence[] = {0x12000073};
    reset_guest_state();
    check(latr_execute_block(sfence, 1), 1,
          "U-mode SFENCE.VMA translates to a trap");
    check(env.exception, 2, "SFENCE.VMA in U-mode is illegal");
    reset_guest_state();
    env.privilege = 1;
    check(latr_execute_block(sfence, 1), 1,
          "S-mode SFENCE.VMA block translated");
    check(env.exception, 0, "SFENCE.VMA in S-mode retires");
    check(env.pc, (uint64_t)(uintptr_t)(sfence + 1),
          "SFENCE.VMA falls through");

    static const uint32_t cbo_zero[] = {0x0043200f};
    for (int i = 0; i < 8; i++) {
        cbo_buf[i] = UINT64_MAX;
    }
    check(run_block(cbo_zero, 1, 6, (uint64_t)(uintptr_t)cbo_buf), 1,
          "CBO.ZERO in U-mode translates to a trap");
    check(env.exception, 2, "CBO.ZERO without CBOZE is illegal in U-mode");
    check(cbo_buf[0], UINT64_MAX, "illegal CBO.ZERO leaves memory intact");

    reset_guest_state();
    env.privilege = 3;
    env.gpr[6] = (uint64_t)(uintptr_t)&cbo_buf[1]; /* 64-byte block unaligned */
    check(latr_execute_block(cbo_zero, 1), 1,
          "M-mode CBO.ZERO block translated");
    check(env.exception, 0, "M-mode CBO.ZERO retires");
    uint64_t nonzero = 0;
    for (int i = 0; i < 8; i++) {
        nonzero |= cbo_buf[i];
    }
    check(nonzero, 0, "CBO.ZERO clears the aligned 64-byte block");
}

static void test_mmu_csr_state(void)
{
    static const uint32_t satp_write_read[] = {
        0x18009073, /* csrrw x0, satp, x1 */
        0x180022f3, /* csrr x5, satp */
    };
    uint64_t satp = (UINT64_C(8) << 60) | (UINT64_C(0x1234) << 44) |
                    UINT64_C(0x56789);
    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = satp;
    check(latr_execute_block(satp_write_read, 2), 1,
          "satp write/read block translated");
    check(env.csr[0x180], satp, "satp keeps MODE/ASID/PPN");
    check(env.gpr[5], satp, "satp read-back matches the write");

    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = (UINT64_C(11) << 60) | UINT64_C(0xabc);
    check(latr_execute_block(satp_write_read, 2), 1,
          "unsupported satp mode block translated");
    check(env.csr[0x180], 0,
          "unsupported satp mode leaves satp unchanged");

    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = UINT64_C(0x999);
    check(latr_execute_block(satp_write_read, 2), 1,
          "Bare-mode satp block translated");
    check(env.csr[0x180], UINT64_C(0x999), "Bare-mode satp is accepted");

    reset_guest_state();
    env.csr[0x300] = UINT64_C(1) << 20; /* TVM */
    env.privilege = 1;
    env.gpr[1] = 0;
    check(latr_execute_block(satp_write_read, 2), 1,
          "TVM satp block translates to a trap");
    check(env.exception, 2, "TVM traps S-mode satp access");
    check(env.csr[0x180], 0, "TVM-blocked satp write is not applied");

    static const uint32_t pmpcfg_write_read[] = {
        0x3a009073, /* csrrw x0, pmpcfg0, x1 */
        0x3a0022f3, /* csrr x5, pmpcfg0 */
    };
    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = UINT64_C(0x0018020f); /* entry1 = 0x02: W without R */
    check(latr_execute_block(pmpcfg_write_read, 2), 1,
          "pmpcfg0 block translated");
    check(env.gpr[5], UINT64_C(0x0018000f),
          "pmpcfg0 drops the reserved W-without-R entry");

    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = UINT64_MAX;
    check(latr_execute_block(pmpcfg_write_read, 2), 1,
          "pmpcfg0 all-ones block translated");
    check(env.gpr[5], UINT64_MAX, "pmpcfg0 accepts R/W entries");

    static const uint32_t pmpaddr_write_read[] = {
        0x3b009073, /* csrrw x0, pmpaddr0, x1 */
        0x3b0022f3, /* csrr x5, pmpaddr0 */
    };
    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = UINT64_C(0x123456789);
    check(latr_execute_block(pmpaddr_write_read, 2), 1,
          "pmpaddr0 block translated");
    check(env.gpr[5], UINT64_C(0x123456789), "pmpaddr0 round-trips");

    static const uint32_t pmpcfg1_probe[] = {
        0x3a105073, /* csrrwi x0, 0x3a1, 0 */
    };
    reset_guest_state();
    env.privilege = 3;
    check(latr_execute_block(pmpcfg1_probe, 1), 1,
          "pmpcfg1 access translates to a trap");
    check(env.exception, 2, "RV64 has no pmpcfg1");
}

static void test_counters(void)
{
    static const uint32_t counters[] = {
        0x00500093, /* addi x1, x0, 5 */
        0xc02022f3, /* csrr x5, instret */
        0xc0002373, /* csrr x6, cycle */
        0xc01023f3, /* csrr x7, time */
        0xc0102473, /* csrr x8, time */
    };
    reset_guest_state();
    env.privilege = 3;
    check(latr_execute_block(counters, 5), 1, "counter CSR block translated");
    check(env.gpr[5], 1, "INSTRET counts previously retired instructions");
    /* mcycle increments when an instruction starts, so the reading
     * instruction is already counted; INSTRET counts retired history. */
    check(env.gpr[6], 3, "CYCLE counts the reading instruction too");
    check(env.gpr[8] >= env.gpr[7], 1, "TIME is monotonic within a block");

    static const uint32_t set_stimecmp[] = {
        0x14d09073, /* csrrw x0, 0x14d, x1 */
    };
    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = 1234;
    check(latr_execute_block(set_stimecmp, 1), 1,
          "STIMECMP write block translated");
    check(env.csr[0x14d], 1234, "STIMECMP stores the written value");
    check(env.stimecmp_update_pending, 1,
          "STIMECMP write requests a timer reprogram");
}

static uint64_t pte_table[5][512] __attribute__((aligned(4096)));

static int pte_tables_used = 1; /* table 0 is the root */

static void clear_page_tables(void)
{
    for (size_t i = 0; i < sizeof(pte_table) / sizeof(pte_table[0]); i++) {
        for (int j = 0; j < 512; j++) {
            pte_table[i][j] = 0;
        }
    }
    pte_tables_used = 1;
}

static uint64_t *alloc_pte_table(void)
{
    if (pte_tables_used >= (int)(sizeof(pte_table) / sizeof(pte_table[0]))) {
        return NULL;
    }
    return pte_table[pte_tables_used++];
}

/* Leaf PTE of `va` under the tables map_page() builds, so a test can edit a
 * page table behind the walker's back. NULL when no leaf exists. */
static uint64_t *leaf_pte(unsigned mode, uint64_t va)
{
    int levels = mode == 8 ? 3 : mode == 9 ? 4 : 5;
    uint64_t *table = pte_table[0];

    for (int level = levels - 1; level > 0; level--) {
        uint64_t vpn = (va >> (12 + 9 * level)) & 0x1ff;

        if ((table[vpn] & LATR_PTE_V) == 0) {
            return NULL;
        }
        table = (uint64_t *)(uintptr_t)(((table[vpn] >> LATR_PTE_PPN_SHIFT) &
                                        LATR_PTE_PPN_MASK)
                                       << 12);
    }
    return &table[(va >> 12) & 0x1ff];
}

/* Maps one 4 KiB page under Sv39/Sv48/Sv57, allocating sub-tables. */
static void map_page(unsigned mode, uint64_t va, uint64_t pa, uint64_t flags)
{
    int levels = mode == 8 ? 3 : mode == 9 ? 4 : 5;
    uint64_t *table = pte_table[0];

    for (int level = levels - 1; level > 0; level--) {
        uint64_t vpn = (va >> (12 + 9 * level)) & 0x1ff;
        if (table[vpn] == 0) {
            uint64_t *next = alloc_pte_table();
            if (next == NULL) {
                return;
            }
            table[vpn] = (((uint64_t)(uintptr_t)next >> 12)
                          << LATR_PTE_PPN_SHIFT) | LATR_PTE_V;
        }
        table = (uint64_t *)(uintptr_t)(((table[vpn] >> 10) & LATR_PTE_PPN_MASK)
                                        << 12);
    }
    table[(va >> 12) & 0x1ff] = ((pa >> 12) << LATR_PTE_PPN_SHIFT) | flags;
}

/* Maps a host range identity so paged tests can still fetch their own code. */
static void map_range_identity(uint64_t start, uint64_t size)
{
    uint64_t page = start & ~UINT64_C(0xfff);
    uint64_t end = (start + size + 0xfff) & ~UINT64_C(0xfff);

    for (; page < end; page += 4096) {
        map_page(8, page, page,
                 LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_X |
                 LATR_PTE_A | LATR_PTE_D);
    }
}


static uint32_t fetch_code_page[1024] __attribute__((aligned(4096)));
static uint8_t fetch_page_a[4096] __attribute__((aligned(4096)));
static uint8_t fetch_page_b[4096] __attribute__((aligned(4096)));

static void test_mmu_fetch(void)
{
    uint64_t sant = (UINT64_C(8) << 60) |
                    ((uint64_t)(uintptr_t)pte_table[0] >> 12);

    /* Non-identity code mapping: VA 0x10000 -> the host code page. */
    clear_page_tables();
    fetch_code_page[0] = 0x00500093; /* addi x1, x0, 5 */
    map_page(8, UINT64_C(0x10000), (uint64_t)(uintptr_t)fetch_code_page,
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_X);
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    check(latr_execute_block((void *)(uintptr_t)UINT64_C(0x10000), 1), 1,
          "non-identity code page translates");
    check(env.gpr[1], 5, "execution reads code through the page table");
    check(env.pc, UINT64_C(0x10004), "non-identity fetch advances the VA");

    /* A 32-bit instruction split across two pages. */
    clear_page_tables();
    fetch_page_a[4094] = 0x93; /* low half of addi x1, x0, 5 */
    fetch_page_a[4095] = 0x00;
    fetch_page_b[0] = 0x50; /* high half */
    fetch_page_b[1] = 0x00;
    map_page(8, UINT64_C(0x8000), (uint64_t)(uintptr_t)fetch_page_a,
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_X);
    map_page(8, UINT64_C(0x9000), (uint64_t)(uintptr_t)fetch_page_b,
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_X);
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    check(latr_execute_block((void *)(uintptr_t)UINT64_C(0x8ffe), 1), 1,
          "cross-page 32-bit fetch translates");
    check(env.gpr[1], 5, "cross-page fetch assembles both halves");
    check(env.pc, UINT64_C(0x9002), "cross-page fetch advances into page two");

    /* Executing from a page without X traps with cause 12. */
    clear_page_tables();
    fetch_code_page[0] = 0x00500093;
    map_page(8, UINT64_C(0xa000), (uint64_t)(uintptr_t)fetch_code_page,
             LATR_PTE_V | LATR_PTE_R);
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    check(latr_execute_block((void *)(uintptr_t)UINT64_C(0xa000), 1), 1,
          "non-executable code page translates to a trap");
    check(env.exception, 12, "non-executable page raises a fetch page fault");
    check(env.badaddr, UINT64_C(0xa000), "fetch fault reports the VA");
    check(env.csr[LATR_TEST_MEPC], UINT64_C(0xa000),
          "fetch fault saves the faulting PC");
    check(env.gpr[1], 0, "fetch fault does not execute the instruction");
}

static uint64_t map_host_page[1024] __attribute__((aligned(4096)));
static uint64_t map_host_page2[1024] __attribute__((aligned(4096)));
static LoongemuMemMap test_mem_map;

static void test_phys_map(void)
{
    loongemu_mem_map_init(&test_mem_map);
    /* One RAM window, one ROM window and a reserved hole. */
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x1000), UINT64_C(0x1000),
                               (uint64_t)(uintptr_t)map_host_page,
                               LOONGEMU_MEM_RAM, "ram"),
          1, "RAM region registers");
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x2000), UINT64_C(0x1000),
                               (uint64_t)(uintptr_t)map_host_page + 0x1000,
                               LOONGEMU_MEM_ROM, "rom"),
          1, "ROM region registers");
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x3000), UINT64_C(0x1000),
                               UINT64_C(0x90000000), LOONGEMU_MEM_RESERVED,
                               "reserved"),
          1, "reserved region registers");
    /* The walker reads PTEs through the physical map; keep the test page
     * tables identity-mapped as RAM. */
    check(loongemu_mem_map_add(&test_mem_map,
                               (uint64_t)(uintptr_t)pte_table & ~UINT64_C(0xfff),
                               UINT64_C(0x5000),
                               (uint64_t)(uintptr_t)pte_table & ~UINT64_C(0xfff),
                               LOONGEMU_MEM_RAM, "pte"), 1,
          "page-table pages register as RAM");

    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &test_mem_map;

    /* Bare mode: the guest VA is the PA and the map supplies the host page. */
    uint64_t host = latr_resolve_guest_address(&env, UINT64_C(0x1010),
                                               LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, 0, "RAM access resolves without a fault");
    check(host, (uint64_t)(uintptr_t)map_host_page + 0x10,
          "RAM access lands on the mapped host page");
    check(env.mem_phys, UINT64_C(0x1010), "physical address is recorded");

    host = latr_resolve_guest_address(&env, UINT64_C(0x2010),
                                      LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, 0, "ROM read resolves");
    check(host, (uint64_t)(uintptr_t)map_host_page + 0x1010,
          "ROM read lands on the mapped host page");
    host = latr_resolve_guest_address(&env, UINT64_C(0x2010),
                                      LATR_ACCESS_STORE, 8);
    check(env.mem_fault_cause, LATR_EXCP_STORE_ACCESS_FAULT,
          "ROM writes raise a store access fault");

    (void)latr_resolve_guest_address(&env, UINT64_C(0x3000),
                                     LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, LATR_EXCP_LOAD_ACCESS_FAULT,
          "reserved host-protected holes never alias");

    (void)latr_resolve_guest_address(&env, UINT64_C(0x5000),
                                     LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, LATR_EXCP_LOAD_ACCESS_FAULT,
          "unmapped physical addresses fault");
    (void)latr_resolve_guest_address(&env, UINT64_C(0x3000),
                                     LATR_ACCESS_NONE, 8);
    check(env.mem_fault_cause, LATR_EXCP_LOAD_ACCESS_FAULT,
          "unknown access kinds fall back to a load access fault");

    /* Fetch through the map: the code page is RAM-mapped host memory. */
    clear_page_tables();
    map_page(8, UINT64_C(0x10000), UINT64_C(0x1000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_X);
    map_host_page[0] = 0x00500093; /* addi x1, x0, 5 */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    env.mem_map = &test_mem_map;
    check(latr_execute_block((void *)(uintptr_t)UINT64_C(0x10000), 1), 1,
          "mapped code page translates");
    check(env.gpr[1], 5, "fetch and data go through the physical map");

    /* A fetch into a reserved hole raises cause 1, not a page fault. */
    clear_page_tables();
    map_page(8, UINT64_C(0x10000), UINT64_C(0x3000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_X);
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    env.mem_map = &test_mem_map;
    check(latr_execute_block((void *)(uintptr_t)UINT64_C(0x10000), 1), 1,
          "reserved code page translates to a trap");
    check(env.exception, LATR_EXCP_INST_ACCESS_FAULT,
          "reserved code page raises an instruction access fault");
    check(env.badaddr, UINT64_C(0x10000), "access fault reports the VA");

    /* Without a map the bare harness keeps the identity behaviour. */
    reset_guest_state();
    check(latr_resolve_guest_address(&env, UINT64_C(0x1010),
                                     LATR_ACCESS_LOAD, 8),
          UINT64_C(0x1010), "no map keeps the identity translation");

    /*
     * The TLB caches the physical-map resolution of a page so a repeat access
     * skips the region lookup. Two things must still hold: the cached plan is
     * used for the right bytes, and a structural map change (generation bump)
     * forces a refill instead of handing back the old host page.
     */
    clear_page_tables();
    map_page(8, UINT64_C(0x10000), UINT64_C(0x1000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_X);
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    env.mem_map = &test_mem_map;
    check(latr_resolve_guest_address(&env, UINT64_C(0x10010),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page + 0x10,
          "first access fills the map plan");
    check(latr_resolve_guest_address(&env, UINT64_C(0x10018),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page + 0x18,
          "repeat access reuses the plan at the right offset");
    check(latr_resolve_guest_address(&env, UINT64_C(0x10008),
                                     LATR_ACCESS_STORE, 8),
          (uint64_t)(uintptr_t)map_host_page + 0x8,
          "store to the planned page resolves");
    check(env.mem_fault_cause, 0, "planned store does not fault");

    /* Move the RAM window to a different host page: the plan must not survive
     * the map change. */
    check(loongemu_mem_map_remove(&test_mem_map, UINT64_C(0x1000)), 1,
          "RAM window is removable for the remap test");
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x1000),
                               UINT64_C(0x1000),
                               (uint64_t)(uintptr_t)map_host_page2,
                               LOONGEMU_MEM_RAM, "ram2"),
          1, "RAM window re-registers at a new host page");
    check(latr_resolve_guest_address(&env, UINT64_C(0x10010),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x10,
          "generation bump refills the plan at the new host page");
    check(loongemu_mem_map_remove(&test_mem_map, UINT64_C(0x1000)), 1,
          "second RAM window is removable");
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x1000),
                               UINT64_C(0x1000),
                               (uint64_t)(uintptr_t)map_host_page,
                               LOONGEMU_MEM_RAM, "ram"),
          1, "original RAM window is restored");

    /* A region shorter than a page clips the plan: an access that runs past
     * the region end must fault even though the earlier one in the same page
     * succeeded. */
    clear_page_tables();
    map_page(8, UINT64_C(0x14000), UINT64_C(0x4000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W);
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x4000),
                               UINT64_C(0x100),
                               (uint64_t)(uintptr_t)map_host_page2,
                               LOONGEMU_MEM_RAM, "short"),
          1, "short RAM window registers");
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    env.mem_map = &test_mem_map;
    check(latr_resolve_guest_address(&env, UINT64_C(0x14080),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x80,
          "access inside the short window resolves");
    (void)latr_resolve_guest_address(&env, UINT64_C(0x140fc),
                                     LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, LATR_EXCP_LOAD_ACCESS_FAULT,
          "access running past the region end still faults");
    check(loongemu_mem_map_remove(&test_mem_map, UINT64_C(0x4000)), 1,
          "short RAM window is removed");

    /*
     * The fast path may skip pte_permits() only when the entry was validated
     * for the *same* privilege / SUM / MXR context and the same access kind,
     * and only while the leaf PTE still equals the snapshot. Each of those is
     * checked below by changing exactly one of them between two accesses that
     * the fast path would otherwise have served.
     */
    check(loongemu_mem_map_add(&test_mem_map, UINT64_C(0x6000),
                               UINT64_C(0x1000),
                               (uint64_t)(uintptr_t)map_host_page2,
                               LOONGEMU_MEM_RAM, "ctx-ram"),
          1, "context-test RAM window registers");
    clear_page_tables();
    map_page(8, UINT64_C(0x1c000), UINT64_C(0x6000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_U |
                 LATR_PTE_A | LATR_PTE_D);
    reset_guest_state();
    env.privilege = LATR_RISCV_PRIV_S;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    env.mem_map = &test_mem_map;
    /* S-mode touching a U page needs mstatus.SUM. */
    (void)latr_resolve_guest_address(&env, UINT64_C(0x1c010),
                                     LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, LATR_EXCP_LOAD_PAGE_FAULT,
          "S-mode load of a U page faults without SUM");
    env.csr[0x300] |= UINT64_C(1) << 18; /* SUM = 1 */
    check(latr_resolve_guest_address(&env, UINT64_C(0x1c010),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x10,
          "SUM=1 lets the same access through");
    check(latr_resolve_guest_address(&env, UINT64_C(0x1c018),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x18,
          "repeat access under the same context");
    env.csr[0x300] &= ~(UINT64_C(1) << 18); /* SUM = 0 */
    (void)latr_resolve_guest_address(&env, UINT64_C(0x1c010),
                                     LATR_ACCESS_LOAD, 8);
    check(env.mem_fault_cause, LATR_EXCP_LOAD_PAGE_FAULT,
          "clearing SUM invalidates the cached verdict");
    env.csr[0x300] |= UINT64_C(1) << 18;

    /* An unsynchronised page-table edit must be observed, not patched over. */
    {
        uint64_t *pte = leaf_pte(8, UINT64_C(0x1c020));

        check(pte != NULL, 1, "leaf PTE is locatable");
        if (pte != NULL) {
            uint64_t saved = *pte;

            *pte = saved & ~LATR_PTE_V;
            (void)latr_resolve_guest_address(&env, UINT64_C(0x1c020),
                                             LATR_ACCESS_LOAD, 8);
            check(env.mem_fault_cause, LATR_EXCP_LOAD_PAGE_FAULT,
                  "a leaf-V clear behind the walker's back faults");
            *pte = saved;
            check(latr_resolve_guest_address(&env, UINT64_C(0x1c020),
                                             LATR_ACCESS_LOAD, 8),
                  (uint64_t)(uintptr_t)map_host_page2 + 0x20,
                  "restoring the PTE makes the access work again");
        }
    }

    /* A store is a different access kind: it needs its own validation. */
    reset_guest_state();
    env.privilege = LATR_RISCV_PRIV_S;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    env.csr[0x300] |= UINT64_C(1) << 18;
    env.mem_map = &test_mem_map;
    check(latr_resolve_guest_address(&env, UINT64_C(0x1c030),
                                     LATR_ACCESS_LOAD, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x30, "load in the ctx page");
    check(latr_resolve_guest_address(&env, UINT64_C(0x1c030),
                                     LATR_ACCESS_STORE, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x30,
          "store to the same address validates its own kind");
    check(latr_resolve_guest_address(&env, UINT64_C(0x1c038),
                                     LATR_ACCESS_STORE, 8),
          (uint64_t)(uintptr_t)map_host_page2 + 0x38,
          "second store is served from the store bit");
    check(loongemu_mem_map_remove(&test_mem_map, UINT64_C(0x6000)), 1,
          "context-test RAM window is removed");
}

static LoongemuMmioBus test_bus;
static LoongemuMemMap test_mmio_map;
static int mmio_reads;
static int mmio_writes;
static uint64_t mmio_last_offset;
static uint64_t mmio_last_size;
static uint64_t mmio_last_value;

static uint64_t dev_reg_read(void *opaque, uint64_t offset, unsigned size)
{
    (void)opaque;
    mmio_reads++;
    mmio_last_offset = offset;
    mmio_last_size = size;
    return UINT64_C(0x1122334455667788);
}

static void dev_reg_write(void *opaque, uint64_t offset, uint64_t value,
                          unsigned size)
{
    (void)opaque;
    mmio_writes++;
    mmio_last_offset = offset;
    mmio_last_size = size;
    mmio_last_value = value;
}

static uint64_t dev_fetch_read(void *opaque, uint64_t offset, unsigned size)
{
    static const uint16_t insn[] = { 0x0093, 0x0050 }; /* addi x1, x0, 5 */
    (void)opaque;
    if (size != 2 || offset >= 4) {
        return 0;
    }
    return insn[offset >> 1];
}

static void map_host_range(LoongemuMemMap *map, const void *code, uint64_t size)
{
    uint64_t base = (uint64_t)(uintptr_t)code & ~UINT64_C(0xfff);
    loongemu_mem_map_add(map, base, 0x1000, base, LOONGEMU_MEM_RAM, "code");
    (void)size;
}

static void test_mmio_path(void)
{
    static const uint32_t io_access[] = {
        0x02a00093, /* addi x1, x0, 42 */
        0x00133023, /* sd x1, 0(x6) */
        0x00033183, /* ld x3, 0(x6) */
    };
    static const uint32_t io_sizes[] = {
        0x00033003, /* ld x0, 0(x6) */
    };
    static const uint32_t io_word[] = {
        0x00032203, /* lw x4, 0(x6) */
        0x00030283, /* lb x5, 0(x6) */
    };
    static const uint32_t io_atomic[] = {
        0x100332af, /* lr.d x5, (x6) */
    };
    static const uint32_t io_fetch[] = {
        0x00500093, /* addi x1, x0, 5 (served from MMIO) */
    };

    loongemu_mem_map_init(&test_mmio_map);
    loongemu_mmio_bus_init(&test_bus);
    map_host_range(&test_mmio_map, io_access, sizeof(io_access));
    map_host_range(&test_mmio_map, io_sizes, sizeof(io_sizes));
    map_host_range(&test_mmio_map, io_word, sizeof(io_word));
    map_host_range(&test_mmio_map, io_atomic, sizeof(io_atomic));
    map_host_range(&test_mmio_map, io_fetch, sizeof(io_fetch));
    check(loongemu_mem_map_add(&test_mmio_map, UINT64_C(0x5000), UINT64_C(0x100),
                               UINT64_C(0x5000), LOONGEMU_MEM_MMIO, "regs"),
          1, "MMIO region registers");
    check(loongemu_mmio_bus_add(&test_bus, UINT64_C(0x5000), UINT64_C(0x100),
                                dev_reg_read, dev_reg_write, NULL, "regs"),
          1, "device registers on the bus");
    check(loongemu_mem_map_add(&test_mmio_map, UINT64_C(0x6000), UINT64_C(0x100),
                               UINT64_C(0x6000), LOONGEMU_MEM_MMIO, "hole"),
          1, "unbacked MMIO region registers");

    /* Store then load through the bus. */
    mmio_reads = mmio_writes = 0;
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x5010);
    check(latr_execute_block(io_access, 3), 1, "MMIO access block runs");
    check(mmio_writes, 1, "store dispatches to the device");
    check(mmio_last_offset, 0x10, "store forwards the register offset");
    check(mmio_last_size, 8, "store forwards the access size");
    check(mmio_last_value, 42, "store forwards the value");
    check(mmio_reads, 1, "load dispatches to the device");
    check(env.gpr[3], UINT64_C(0x1122334455667788),
          "load returns the device value");

    /* Widths reach the device unchanged. */
    mmio_reads = 0;
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x5012);
    check(latr_execute_block(io_word, 2), 1, "narrow MMIO block runs");
    check(env.gpr[4], UINT64_C(0x55667788), "lw gets four bytes");
    check(env.gpr[5], UINT64_C(0xffffffffffffff88),
          "lb gets a sign-extended byte");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x5010);
    check(latr_execute_block(io_sizes, 1), 1, "ld MMIO block runs");
    check(mmio_last_size, 8, "ld asks for eight bytes");

    /* MMIO without a device is an access fault, not a host dereference. */
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x6010);
    check(latr_execute_block(io_sizes, 1), 1, "unbacked MMIO load runs");
    check(env.exception, LATR_EXCP_LOAD_ACCESS_FAULT,
          "unbacked MMIO load faults");
    check(env.badaddr, UINT64_C(0x6010), "access fault reports the VA");

    /* Atomics to MMIO are rejected with a store/AMO access fault. */
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x5010);
    check(latr_execute_block(io_atomic, 1), 1, "MMIO LR block runs");
    check(env.exception, LATR_EXCP_STORE_ACCESS_FAULT,
          "LR to MMIO raises a store access fault");
    check(env.badaddr, UINT64_C(0x5010), "MMIO atomic fault reports the VA");

    /* Execute-in-place through the bus (pflash style). */
    check(loongemu_mem_map_add(&test_mmio_map, UINT64_C(0x7000), UINT64_C(0x100),
                               UINT64_C(0x7000), LOONGEMU_MEM_MMIO, "flash"),
          1, "flash MMIO region registers");
    check(loongemu_mmio_bus_add(&test_bus, UINT64_C(0x7000), UINT64_C(0x100),
                                dev_fetch_read, NULL, NULL, "flash"),
          1, "flash device on the bus");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    check(latr_execute_block((void *)(uintptr_t)UINT64_C(0x7000), 1), 1,
          "execute-in-place block runs");
    check(env.gpr[1], 5, "fetch reached the device and executed");
}

#define CHECK_DEV(cond, name) check((cond) ? 1 : 0, 1, name)

static LoongemuUart16550 test_uart;
static LoongemuPlic test_device_plic;
static char uart_tx_buf[32];
static int uart_tx_len;

static void uart_tx_cb(void *opaque, uint8_t byte)
{
    (void)opaque;
    if (uart_tx_len < (int)sizeof(uart_tx_buf) - 1) {
        uart_tx_buf[uart_tx_len++] = (char)byte;
        uart_tx_buf[uart_tx_len] = 0;
    }
}

static void test_uart_plic_devices(void)
{
    static const uint32_t uart_putc[] = { 0x00730023 }; /* sb x7, 0(x6) */
    static const uint32_t uart_lsr[] = { 0x00534283 };  /* lbu x5, 5(x6) */
    static const uint32_t uart_set_ier[] = { 0x007300a3 }; /* sb x7, 1(x6) */
    static const uint32_t uart_get_rbr[] = { 0x00034283 }; /* lbu x5, 0(x6) */
    static const uint32_t plic_priority[] = { 0x00732223 }; /* sw x7, 4(x6) */
    static const uint32_t plic_word[] = { 0x00732023 };     /* sw x7, 0(x6) */
    static const uint32_t plic_claim[] = { 0x00432283 };    /* lw x5, 4(x6) */
    static const uint32_t plic_prio10[] = { 0x02732423 }; /* sw x7, 40(x6) */
    LoongemuUartPlicLine line = { &test_device_plic, 10 };

    loongemu_mem_map_init(&test_mmio_map);
    loongemu_mmio_bus_init(&test_bus);
    check(loongemu_mem_map_add(&test_mmio_map, UINT64_C(0x10000000),
                               UINT64_C(0x100), UINT64_C(0x10000000),
                               LOONGEMU_MEM_MMIO, "uart"),
          1, "UART region registers");
    check(loongemu_mem_map_add(&test_mmio_map, UINT64_C(0x0c000000),
                               UINT64_C(0x400000), UINT64_C(0x0c000000),
                               LOONGEMU_MEM_MMIO, "plic"),
          1, "PLIC region registers");
    map_host_range(&test_mmio_map, uart_putc, sizeof(uart_putc));
    map_host_range(&test_mmio_map, uart_lsr, sizeof(uart_lsr));
    map_host_range(&test_mmio_map, uart_set_ier, sizeof(uart_set_ier));
    map_host_range(&test_mmio_map, uart_get_rbr, sizeof(uart_get_rbr));
    map_host_range(&test_mmio_map, plic_priority, sizeof(plic_priority));
    map_host_range(&test_mmio_map, plic_word, sizeof(plic_word));
    map_host_range(&test_mmio_map, plic_claim, sizeof(plic_claim));

    check(loongemu_plic_init(&test_device_plic, 32, 1), 1,
          "device PLIC initialises");
    CHECK_DEV(loongemu_plic_attach(&test_bus, &test_device_plic,
                                   UINT64_C(0x0c000000)) == 1,
              "PLIC attaches to the bus");
    loongemu_uart16550_init(&test_uart, uart_tx_cb, loongemu_uart_plic_irq,
                            &line);
    uart_tx_len = 0;
    CHECK_DEV(loongemu_uart16550_attach(&test_bus, &test_uart,
                                        UINT64_C(0x10000000),
                                        UINT64_C(0x100)) == 1,
              "UART attaches to the bus");

    /* Guest writes a byte to the UART THR and reads LSR. */
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x10000000);
    env.gpr[7] = 'A';
    check(latr_execute_block(uart_putc, 1), 1, "guest THR write runs");
    check(uart_tx_len, 1, "guest THR write reaches the console");
    check(uart_tx_buf[0], 'A', "console receives the guest byte");
    env.gpr[6] = UINT64_C(0x10000000);
    check(latr_execute_block(uart_lsr, 1), 1, "guest LSR read runs");
    check(env.gpr[5], 0x60, "guest sees THRE/TEMT");

    /* Guest programs the PLIC registers through MMIO. */
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x0c000000);
    env.gpr[7] = 3;
    check(latr_execute_block(plic_priority, 1), 1, "guest priority write runs");
    check(test_device_plic.priority[1], 3, "PLIC priority updated");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x0c002000); /* context 0 enable word */
    env.gpr[7] = 1u << 10;
    check(latr_execute_block(plic_word, 1), 1, "guest enable write runs");
    check(test_device_plic.enable[0][0], 1u << 10, "PLIC enable updated");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x0c200000); /* context 0 threshold */
    env.gpr[7] = 0;
    check(latr_execute_block(plic_word, 1), 1, "guest threshold write runs");
    check(test_device_plic.threshold[0], 0, "PLIC threshold updated");

    /* Give the UART source a priority and enable it for context 0. */
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x0c000000);
    env.gpr[7] = 1;
    check(latr_execute_block(plic_prio10, 1), 1,
          "guest source-10 priority write runs");
    check(test_device_plic.priority[10], 1, "PLIC source 10 has a priority");

    /* Enable the UART receive interrupt, inject a byte, claim via guest. */
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x10000000);
    env.gpr[7] = 1; /* IER.RDA */
    check(latr_execute_block(uart_set_ier, 1), 1, "guest IER write runs");
    check(test_uart.ier, 1, "UART IER updated by the guest");
    loongemu_uart16550_receive(&test_uart, 'B');
    check(loongemu_plic_pending(&test_device_plic, 0), 1,
          "UART interrupt reaches the PLIC");
    env.plic = &test_device_plic;
    env.plic_context = 0;
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 11), 1ULL << 11,
          "PLIC pending raises MIP.MEIP");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x0c200000);
    check(latr_execute_block(plic_claim, 1), 1, "guest claim runs");
    check(env.gpr[5], 10, "guest claim returns the UART source");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x10000000);
    check(latr_execute_block(uart_get_rbr, 1), 1, "guest RBR read runs");
    check(env.gpr[5], 'B', "guest reads the received byte");
    check(loongemu_uart16550_irq_level(&test_uart), 0,
          "reading RBR drops the UART line");
    reset_guest_state();
    env.mem_map = &test_mmio_map;
    env.mmio = &test_bus;
    env.gpr[6] = UINT64_C(0x0c200000);
    env.gpr[7] = 10;
    check(latr_execute_block(plic_word, 1), 1, "guest complete runs");
    check(loongemu_plic_pending(&test_device_plic, 0), 0,
          "complete with the line low leaves nothing pending");
}

static void test_tb_insn_map(void)
{
    static const uint32_t block[] = {
        0x00500093, /* addi x1, x0, 5 */
        0x00108113, /* addi x2, x1, 1 */
        0x002081b3, /* add x3, x1, x2 */
    };
    reset_guest_state();
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)block;
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = LATR_TB_CFLAGS_ALLOW_FLOW; /* unit harness: multi-flow block */
    int max_insns = 3;
    target_disasm(&tb, &max_insns, &cpu);
    check(tb.icount, 3, "map test block decodes three instructions");
    tr_init(&tb);
    check(tr_ir2_generate(&tb), 1, "map test block translates");
    int words = tr_ir2_assemble(code_buf);
    tr_fini();
    check(words > 0, 1, "map test block assembles");

    uint64_t gpc = 0;
    uint32_t raw_len = 0;
    check(latr_tb_insn_map_resolve((uintptr_t)code_buf, &gpc, &raw_len), 1,
          "map resolves the block start");
    check(gpc, (uint64_t)(uintptr_t)block, "first span names the first PC");
    check(raw_len, 4, "first span records the raw instruction length");

    uint64_t seq_pc[8];
    uint32_t seq_len[8];
    int seq = 0;
    for (int i = 0; i < words; i++) {
        uint64_t cur_pc;
        uint32_t cur_len;
        if (!latr_tb_insn_map_resolve((uintptr_t)code_buf + 4 * i,
                                      &cur_pc, &cur_len)) {
            continue;
        }
        if (seq > 0 && seq_pc[seq - 1] == cur_pc) {
            continue;
        }
        if (seq < 8) {
            seq_pc[seq] = cur_pc;
            seq_len[seq] = cur_len;
            seq++;
        }
    }
    check(seq, 3, "map yields one span per instruction");
    for (int i = 0; i < seq && i < 3; i++) {
        check(seq_pc[i], (uint64_t)(uintptr_t)(block + i),
              "map spans follow guest PC order");
        check(seq_len[i], 4, "map spans record 32-bit lengths");
    }

    latr_tb_insn_map_forget(code_buf);
    check(latr_tb_insn_map_resolve((uintptr_t)code_buf, &gpc, &raw_len), 0,
          "forget drops the map entry");

    /* RVC: the same map must record 2-byte instruction lengths. */
    static const uint8_t mixed[] = {
        (uint8_t)(LATR_DIFF_C_ADDI_X10_1 & 0xff),
        (uint8_t)(LATR_DIFF_C_ADDI_X10_1 >> 8),
        0x93, 0x00, 0x50, 0x00, /* addi x1, x0, 5 */
    };
    reset_guest_state();
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)mixed;
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = LATR_TB_CFLAGS_ALLOW_FLOW; /* unit harness: multi-flow block */
    max_insns = 2;
    target_disasm(&tb, &max_insns, &cpu);
    check(tb.icount, 2, "mixed-width block decodes two instructions");
    tr_init(&tb);
    check(tr_ir2_generate(&tb), 1, "mixed-width block translates");
    words = tr_ir2_assemble(code_buf);
    tr_fini();

    seq = 0;
    for (int i = 0; i < words; i++) {
        uint64_t cur_pc;
        uint32_t cur_len;
        if (!latr_tb_insn_map_resolve((uintptr_t)code_buf + 4 * i,
                                      &cur_pc, &cur_len)) {
            continue;
        }
        if (seq > 0 && seq_pc[seq - 1] == cur_pc) {
            continue;
        }
        if (seq < 8) {
            seq_pc[seq] = cur_pc;
            seq_len[seq] = cur_len;
            seq++;
        }
    }
    check(seq, 2, "mixed-width map yields two spans");
    if (seq == 2) {
        check(seq_pc[0], (uint64_t)(uintptr_t)mixed, "RVC span starts the block");
        check(seq_len[0], 2, "RVC span records a 2-byte length");
        check(seq_pc[1], (uint64_t)(uintptr_t)(mixed + 2),
              "second span starts after the RVC instruction");
        check(seq_len[1], 4, "second span records a 4-byte length");
    }
    latr_tb_insn_map_forget(code_buf);
}
static void test_tb_recovery(void)
{
    /* addi x5 leaves x5's value deferred in a host temp; the ld that follows
     * is a GUEST_ACCESS span whose binding snapshot must carry x5's binding. */
    static const uint32_t block[] = {
        0x00700293, /* addi x5, x0, 7  */
        0x00033183, /* ld x3, 0(x6)    */
        0x00100113, /* addi x2, x0, 1  */
    };
    reset_guest_state();
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)block;
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = 0;
    int max_insns = 3;
    target_disasm(&tb, &max_insns, &cpu);
    check(tb.icount, 3, "recovery block decodes three instructions");
    tr_init(&tb);
    check(tr_ir2_generate(&tb), 1, "recovery block translates");
    int words = tr_ir2_assemble(code_buf);
    check(words > 0, 1, "recovery block assembles");

    /* First insn (pure addi) is a host-internal span. */
    const LatrTbInsnMap *map = NULL;
    const LatrTbInsnSpan *s0 = latr_tb_recovery_resolve(
        (uintptr_t)code_buf, &map);
    check(s0 != NULL, 1, "recovery resolves first insn span");
    if (s0 != NULL) {
        check(s0->site, LATR_RECOV_NATIVE_INTERNAL,
              "pure-addi span is native-internal");
        check(latr_tb_recovery_apply(s0, map, NULL, NULL, &env), 0,
              "native-internal span is not applied");
    }

    /* Walk the assembled words until the ld span turns up: its site must be
     * GUEST_ACCESS, retired_before must be 1, and it must carry x5's binding. */
    const LatrTbInsnSpan *sld = NULL;
    for (int i = 0; i < words; i++) {
        const LatrTbInsnSpan *s = latr_tb_recovery_resolve(
            (uintptr_t)code_buf + (uintptr_t)i * 4, NULL);
        if (s != NULL && s->site == LATR_RECOV_GUEST_ACCESS &&
            s->guest_pc == (uint64_t)(uintptr_t)&block[1]) {
            sld = s;
            break;
        }
    }
    check(sld != NULL, 1, "ld span is guest-access");
    if (sld != NULL) {
        check(sld->retired_before, 1, "ld span retires one insn before it");
        /* LATR flushes the regcache on every control-flow edge (la_* macros
         * -> latr_regcache_flush), so no GPR/FPR defer survives to a fault
         * span; bind_cnt is structurally 0 here. Assert the recovery
         * plumbing that is observable: guest-access site, apply success and
         * env.pc projection. Any bindings present must still apply. */
        uint64_t host_gprs[32] = {0};
        for (uint8_t i = 0; i < sld->bind_cnt; i++) {
            const LatrRecoveryBinding *b =
                &map->binds[sld->bind_first + i];
            if (!b->is_fpr && b->temp >= 0 && b->temp < 32) {
                host_gprs[b->temp] = UINT64_C(0xdeadbeefcafe) + i;
            }
        }
        env.gpr[5] = 0;
        check(latr_tb_recovery_apply(sld, map, host_gprs, NULL, &env), 1,
              "guest-access span applies");
        for (uint8_t i = 0; i < sld->bind_cnt; i++) {
            const LatrRecoveryBinding *b =
                &map->binds[sld->bind_first + i];
            if (!b->is_fpr) {
                check(env.gpr[b->guest_reg],
                      UINT64_C(0xdeadbeefcafe) + i,
                      "gpr binding applied to env.gpr");
            }
        }
        check(env.pc, (uint64_t)(uintptr_t)&block[1],
              "apply sets env.pc to the ld");
        check(sld->phase, LE_PHASE_BEFORE_INSN,
              "scalar load commits nothing before the fault");
    }
    latr_tb_insn_map_forget(code_buf);

    /*
     * Phase classification: a trap retires into the vector (AFTER_FALLTHROUGH),
     * while an RVV element store can leave a committed prefix (PARTIAL_EFFECT,
     * which the machine's host-fault path refuses to project).
     */
    {
        static const uint32_t ecall_block[] = { 0x00000073 }; /* ecall */
        const LatrTbInsnSpan *s;

        reset_guest_state();
        for (size_t i = 0; i < sizeof(tb); i++) {
            ((uint8_t *)&tb)[i] = 0;
        }
        cpu.env_ptr = &env;
        env.pc = (uint64_t)(uintptr_t)ecall_block;
        tb.pc = env.pc;
        tb.tc.ptr = code_buf;
        tb.cflags = 0;
        int once = 1;
        target_disasm(&tb, &once, &cpu);
        tr_init(&tb);
        check(tr_ir2_generate(&tb), 1, "ecall block translates");
        check(tr_ir2_assemble(code_buf) > 0, 1, "ecall block assembles");
        s = latr_tb_recovery_resolve((uintptr_t)code_buf, NULL);
        check(s != NULL, 1, "ecall span resolves");
        if (s != NULL) {
            check(s->site, LATR_RECOV_GUEST_CHECK, "ecall span is a guest check");
            check(s->phase, LE_PHASE_AFTER_FALLTHROUGH,
                  "ecall span retires into the vector");
        }
        latr_tb_insn_map_forget(code_buf);
    }
    {
        /* vse32.v v1, (x6): element store, commit order per element. */
        static const uint32_t vstore_block[] = { 0x020860a7 };
        const LatrTbInsnSpan *s;

        reset_guest_state();
        for (size_t i = 0; i < sizeof(tb); i++) {
            ((uint8_t *)&tb)[i] = 0;
        }
        cpu.env_ptr = &env;
        env.pc = (uint64_t)(uintptr_t)vstore_block;
        tb.pc = env.pc;
        tb.tc.ptr = code_buf;
        tb.cflags = 0;
        int once = 1;
        target_disasm(&tb, &once, &cpu);
        check(tb.icount, 1, "vector store decodes");
        tr_init(&tb);
        check(tr_ir2_generate(&tb), 1, "vector store translates");
        check(tr_ir2_assemble(code_buf) > 0, 1, "vector store assembles");
        s = latr_tb_recovery_resolve((uintptr_t)code_buf, NULL);
        check(s != NULL, 1, "vector store span resolves");
        if (s != NULL) {
            check(s->site, LATR_RECOV_GUEST_ACCESS,
                  "vector store is a guest access");
            check(s->phase, LE_PHASE_PARTIAL_EFFECT,
                  "vector store can commit a prefix");
        }
        latr_tb_insn_map_forget(code_buf);
    }
}

/*
 * Recovery writeback must be all-or-nothing: a span whose snapshot does not
 * cover its bindings, or whose rows are malformed, must leave the guest
 * register file exactly as it was. A half-applied replay would deliver a trap
 * over state the guest never had.
 */
static void test_recovery_atomic_apply(void)
{
    static LatrTbInsnMap map;
    LatrTbInsnSpan span;
    uint64_t host[32];
    uint64_t fhost[32];

    for (size_t i = 0; i < sizeof(map); i++) {
        ((uint8_t *)&map)[i] = 0;
    }
    memset(&span, 0, sizeof(span));
    memset(host, 0, sizeof(host));
    memset(fhost, 0, sizeof(fhost));
    host[3] = UINT64_C(0x7777);
    host[4] = UINT64_C(0x8888);
    fhost[7] = UINT64_C(0x9999);

    map.binds[0].guest_reg = 5;
    map.binds[0].temp = 3;
    map.binds[1].guest_reg = 6;
    map.binds[1].temp = 4;
    span.site = LATR_RECOV_GUEST_ACCESS;
    span.phase = LE_PHASE_BEFORE_INSN;
    span.guest_pc = UINT64_C(0x1234);
    span.bind_first = 0;
    span.bind_cnt = 2;

    reset_guest_state();
    env.gpr[5] = UINT64_C(0xaa);
    env.gpr[6] = UINT64_C(0xbb);
    env.pc = UINT64_C(0xdead);

    /* No snapshot: nothing may change, not even pc. */
    check(latr_tb_recovery_apply(&span, &map, NULL, NULL, &env), 0,
          "missing GPR snapshot is refused");
    check(env.gpr[5], UINT64_C(0xaa), "refused apply leaves gpr5 alone");
    check(env.gpr[6], UINT64_C(0xbb), "refused apply leaves gpr6 alone");
    check(env.pc, UINT64_C(0xdead), "refused apply leaves pc alone");

    /* A malformed row rejects the whole span. */
    map.binds[1].temp = 40; /* outside the host register file */
    check(latr_tb_recovery_apply(&span, &map, host, NULL, &env), 0,
          "out-of-range host register refuses the span");
    check(env.gpr[5], UINT64_C(0xaa), "bad row leaves the first binding unapplied");
    check(env.gpr[6], UINT64_C(0xbb), "bad row leaves the second binding unapplied");
    map.binds[1].temp = 4;

    /* Valid snapshot: both bindings land and pc points at the span. */
    check(latr_tb_recovery_apply(&span, &map, host, fhost, &env), 1,
          "valid snapshot applies");
    check(env.gpr[5], UINT64_C(0x7777), "first binding committed");
    check(env.gpr[6], UINT64_C(0x8888), "second binding committed");
    check(env.pc, UINT64_C(0x1234), "pc projection committed");

    /* An FPR binding without the FPR snapshot is refused the same way. */
    memset(&span, 0, sizeof(span));
    span.site = LATR_RECOV_GUEST_ACCESS;
    span.guest_pc = UINT64_C(0x4321);
    span.bind_first = 0;
    span.bind_cnt = 1;
    map.binds[0].guest_reg = 9;
    map.binds[0].temp = 7;
    map.binds[0].is_fpr = 1;
    env.fpr[9] = UINT64_C(0xcc);
    env.pc = UINT64_C(0xbeef);
    check(latr_tb_recovery_apply(&span, &map, host, NULL, &env), 0,
          "missing FPR snapshot is refused");
    check(env.fpr[9], UINT64_C(0xcc), "refused FPR apply leaves the register");
    check(latr_tb_recovery_apply(&span, &map, host, fhost, &env), 1,
          "FPR snapshot applies");
    check(env.fpr[9], UINT64_C(0x9999), "FPR binding committed");

    /* A row that points at a spill slot cannot be read from the host register
     * file, and LATR never emits one: refuse rather than read a register. */
    map.binds[0].is_fpr = 0;
    map.binds[0].is_spill = 1;
    map.binds[0].temp = 2; /* would be a valid host register */
    env.gpr[9] = UINT64_C(0xdd);
    env.pc = UINT64_C(0xf00d);
    check(latr_tb_recovery_apply(&span, &map, host, fhost, &env), 0,
          "spill-slot binding is refused");
    check(env.gpr[9], UINT64_C(0xdd), "refused spill leaves the register");
    check(env.pc, UINT64_C(0xf00d), "refused spill leaves pc");
    map.binds[0].is_spill = 0;
}

static void test_mmu_walk(void)
{
    clear_page_tables();

    reset_guest_state();
    env.privilege = 1; /* S-mode, so U pages need mstatus.SUM */
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    map_page(8, UINT64_C(0x1000), UINT64_C(0x2000),
                        LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_X);
    uint64_t pa = 0;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "Sv39 4K leaf resolves");
    check(pa, UINT64_C(0x2000), "Sv39 4K leaf physical address");
    check(pte_table[2][1] & LATR_PTE_A, LATR_PTE_A, "Sv39 walk sets A");
    check(latr_walk(&env, UINT64_C(0x1abc), LATR_ACCESS_STORE, &pa), 0,
          "Sv39 store within the page resolves");
    check(pa, UINT64_C(0x2abc), "Sv39 store keeps the page offset");
    check(pte_table[2][1] & LATR_PTE_D, LATR_PTE_D, "Sv39 store sets D");

    /* Svade: with ADUE=0, a clear A bit faults instead of being set. */
    env.csr[0x30a] = 0;
    pte_table[2][1] &= ~(LATR_PTE_A | LATR_PTE_D);
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "Svade faults on a clear A bit");
    check(pte_table[2][1] & LATR_PTE_A, 0, "Svade leaves A for software");
    env.csr[0x30a] = UINT64_C(1) << 61;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "Svadu resolves once ADUE is enabled");
    check(pte_table[2][1] & LATR_PTE_A, LATR_PTE_A, "Svadu sets A");

    /* Svpbmt reserved encoding faults unless PBMTE is enabled. */
    pte_table[2][1] |= UINT64_C(1) << 61;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "PBMT without PBMTE faults");
    env.csr[0x30a] |= UINT64_C(1) << 62;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "PBMTE allows PBMT pages");
    pte_table[2][1] &= ~(UINT64_C(1) << 61);
    env.csr[0x30a] &= ~(UINT64_C(1) << 62);

    check(latr_walk(&env, UINT64_C(0x2000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "unmapped Sv39 page faults");
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_EXEC, &pa), 0,
          "Sv39 execute checks X");

    /* Permission encoding: W without R is reserved and faults everywhere. */
    pte_table[2][1] = ((UINT64_C(0x2000) >> 12) << LATR_PTE_PPN_SHIFT) |
                      LATR_PTE_V | LATR_PTE_W;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "W-without-R PTE faults");

    pte_table[2][1] = ((UINT64_C(0x2000) >> 12) << LATR_PTE_PPN_SHIFT) |
                      LATR_PTE_V | LATR_PTE_R | LATR_PTE_A;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_STORE, &pa),
          LATR_EXCP_STORE_PAGE_FAULT, "read-only leaf rejects stores");
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "read-only leaf allows loads");

    /* User pages are inaccessible to S-mode unless SUM is set. */
    pte_table[2][1] = ((UINT64_C(0x2000) >> 12) << LATR_PTE_PPN_SHIFT) |
                      LATR_PTE_V | LATR_PTE_R | LATR_PTE_U | LATR_PTE_A;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "SUM=0 blocks S-mode user-page loads");
    env.csr[0x300] = UINT64_C(1) << 18;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "SUM=1 allows S-mode user-page loads");
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_EXEC, &pa),
          LATR_EXCP_INST_PAGE_FAULT, "S-mode never executes user pages");
    env.csr[0x300] = 0;
    env.privilege = 0;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "U-mode may use a user page");
    env.privilege = 1;

    /* MXR turns X-only leaves into readable ones. */
    pte_table[2][1] = ((UINT64_C(0x2000) >> 12) << LATR_PTE_PPN_SHIFT) |
                      LATR_PTE_V | LATR_PTE_X | LATR_PTE_A;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "X-only leaf rejects loads");
    env.csr[0x300] = UINT64_C(1) << 19;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "MXR allows loads from X-only leaves");
    env.csr[0x300] = 0;

    /* Superpages: 2 MiB leaf at level 1 of the VPN[2]=1 subtree. */
    clear_page_tables();
    pte_table[0][1] = (((uint64_t)(uintptr_t)pte_table[1] >> 12)
                       << LATR_PTE_PPN_SHIFT) | LATR_PTE_V;
    pte_table[1][0] = ((UINT64_C(0x80000000) >> 12) << LATR_PTE_PPN_SHIFT) |
                      LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_A |
                      LATR_PTE_D;
    check(latr_walk(&env, UINT64_C(0x40000000), LATR_ACCESS_LOAD, &pa), 0,
          "2 MiB Sv39 superpage resolves");
    check(pa, UINT64_C(0x80000000), "2 MiB superpage base");
    check(latr_walk(&env, UINT64_C(0x40123456), LATR_ACCESS_LOAD, &pa), 0,
          "2 MiB superpage offset resolves");
    check(pa, UINT64_C(0x80123456), "2 MiB superpage keeps VA offset");
    pte_table[1][0] |= UINT64_C(1) << 12; /* misaligned superpage PPN */
    check(latr_walk(&env, UINT64_C(0x40000000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "misaligned superpage faults");

    /* Sv48 (4 levels) and Sv57 (5 levels) reach the same leaf. */
    clear_page_tables();
    env.csr[0x180] = (UINT64_C(9) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    map_page(9, UINT64_C(0x1000), UINT64_C(0x3000),
                        LATR_PTE_V | LATR_PTE_R | LATR_PTE_A);
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "Sv48 leaf resolves");
    check(pa, UINT64_C(0x3000), "Sv48 leaf physical address");

    clear_page_tables();
    env.csr[0x180] = (UINT64_C(10) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    map_page(10, UINT64_C(0x1000), UINT64_C(0x4000),
                        LATR_PTE_V | LATR_PTE_R | LATR_PTE_A);
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "Sv57 leaf resolves");
    check(pa, UINT64_C(0x4000), "Sv57 leaf physical address");

    /* M-mode bypasses translation; Bare mode is identity. */
    env.privilege = 3;
    check(latr_walk(&env, UINT64_C(0x1234), LATR_ACCESS_LOAD, &pa), 0,
          "M-mode bypasses translation");
    check(pa, UINT64_C(0x1234), "M-mode address is identity-mapped");
    env.privilege = 1;
    env.csr[0x180] = 0;
    check(latr_walk(&env, UINT64_C(0x1234), LATR_ACCESS_LOAD, &pa), 0,
          "Bare mode is identity-mapped");
    check(pa, UINT64_C(0x1234), "Bare-mode address passes through");

    /* The runtime wrapper reports the fault cause in the environment. */
    clear_page_tables();
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    check(latr_resolve_guest_address(&env, UINT64_C(0x1000),
                                     LATR_ACCESS_LOAD, 8), 0,
          "resolve reports a fault as address zero");
    check(env.mem_fault_cause, LATR_EXCP_LOAD_PAGE_FAULT,
          "resolve stores the fault cause");
}

static void test_tlb(void)
{
    uint64_t pa = 0;
    uint64_t hits;

    clear_page_tables();
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    map_page(8, UINT64_C(0x1000), UINT64_C(0x2000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W |
             LATR_PTE_A | LATR_PTE_D);
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "TLB cold walk resolves");
    check(env.tlb_hits, 0, "cold walk misses the TLB");
    check(latr_walk(&env, UINT64_C(0x1234), LATR_ACCESS_LOAD, &pa), 0,
          "TLB hit resolves");
    check(pa, UINT64_C(0x2234), "TLB hit preserves the page offset");
    check(env.tlb_hits, 1, "second walk hits the TLB");

    /* A leaf edit without SFENCE is observed by PTE validation. */
    pte_table[2][1] = LATR_PTE_V | LATR_PTE_R | LATR_PTE_A;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_STORE, &pa),
          LATR_EXCP_STORE_PAGE_FAULT,
          "TLB validation observes a removed W bit");
    check(env.tlb_hits, 1, "invalidated TLB entry does not count as a hit");

    /* All-ASID SFENCE bypasses the cache, then caching resumes. */
    map_page(8, UINT64_C(0x1000), UINT64_C(0x3000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W |
             LATR_PTE_A | LATR_PTE_D);
    env.sfence_pending = 1;
    env.sfence_asid = UINT32_MAX;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "SFENCE-all walk resolves");
    check(env.tlb_hits, 1, "SFENCE-all bypasses the cached entry");
    env.sfence_pending = 0;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "walk after SFENCE resolves");
    check(env.tlb_hits, 2, "caching resumes after the flush");

    /* ASID-scoped SFENCE leaves other address spaces cached. */
    env.csr[0x180] = (UINT64_C(5) << 44) | (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "new ASID walk resolves");
    check(env.tlb_hits, 2, "new ASID misses the previous entry");
    env.sfence_pending = 1;
    env.sfence_asid = 6;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "other-ASID SFENCE walk resolves");
    check(env.tlb_hits, 3, "other-ASID SFENCE keeps the entry");
    env.sfence_asid = 5;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "matching-ASID SFENCE walk resolves");
    check(env.tlb_hits, 3, "matching-ASID SFENCE drops the entry");
    env.sfence_pending = 0;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "walk caches before a cleared-flag flush");
    check(env.tlb_hits, 4, "walk hits before the sequence bump");

    /* The TLB flush sequence survives the runtime clearing the flags. */
    env.tlb_flush_seq++;
    env.sfence_pending = 0;
    check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa), 0,
          "sequence-only flush walk resolves");
    check(env.tlb_hits, 4, "cleared-flag SFENCE still flushes the TLB");

    /* MXR and SUM are re-evaluated on every hit. */
    map_page(8, UINT64_C(0x4000), UINT64_C(0x5000),
             LATR_PTE_V | LATR_PTE_X | LATR_PTE_A | LATR_PTE_D);
    check(latr_walk(&env, UINT64_C(0x4000), LATR_ACCESS_EXEC, &pa), 0,
          "execute-only page caches for fetch");
    check(latr_walk(&env, UINT64_C(0x4000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT,
          "hit re-checks MXR and rejects a read");
    env.csr[0x300] |= UINT64_C(1) << 19; /* MXR */
    hits = env.tlb_hits;
    check(latr_walk(&env, UINT64_C(0x4000), LATR_ACCESS_LOAD, &pa), 0,
          "MXR makes the cached page readable");
    check(env.tlb_hits, hits + 1, "MXR-allowed load is a TLB hit");
    env.csr[0x300] &= ~(UINT64_C(1) << 19);

    map_page(8, UINT64_C(0x6000), UINT64_C(0x7000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_U |
             LATR_PTE_A | LATR_PTE_D);
    check(latr_walk(&env, UINT64_C(0x6000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_PAGE_FAULT, "SUM=0 rejects a user page");
    env.csr[0x300] |= UINT64_C(1) << 18; /* SUM */
    hits = env.tlb_hits;
    check(latr_walk(&env, UINT64_C(0x6000), LATR_ACCESS_LOAD, &pa), 0,
          "SUM=1 allows the user page");
    check(env.tlb_hits, hits, "SUM-allowed load fills the TLB");
    check(latr_walk(&env, UINT64_C(0x6000), LATR_ACCESS_LOAD, &pa), 0,
          "second SUM-allowed load resolves");
    check(env.tlb_hits, hits + 1, "SUM-allowed load hits the TLB");
    env.csr[0x300] &= ~(UINT64_C(1) << 18);

    /* A store hit still needs D; a miss lets the walker set it. */
    map_page(8, UINT64_C(0xa000), UINT64_C(0xb000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_A);
    check(latr_walk(&env, UINT64_C(0xa000), LATR_ACCESS_LOAD, &pa), 0,
          "A-only page caches for loads");
    check(latr_walk(&env, UINT64_C(0xa000), LATR_ACCESS_LOAD, &pa), 0,
          "A-only page hits for loads");
    hits = env.tlb_hits;
    check(latr_walk(&env, UINT64_C(0xa000), LATR_ACCESS_STORE, &pa), 0,
          "store falls back to set D");
    check(env.tlb_hits, hits, "D-missing store is not a hit");
    check(pte_table[2][10] & LATR_PTE_D, LATR_PTE_D,
          "store walk sets the D bit");
    check(latr_walk(&env, UINT64_C(0xa000), LATR_ACCESS_STORE, &pa), 0,
          "second store resolves");
    check(env.tlb_hits, hits + 1, "second store hits the TLB");

    /* PMP is re-checked even when the translation is cached. */
    map_page(8, UINT64_C(0x8000), UINT64_C(0x9000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W |
             LATR_PTE_A | LATR_PTE_D);
    check(latr_walk(&env, UINT64_C(0x8000), LATR_ACCESS_LOAD, &pa), 0,
          "PMP test page resolves");
    env.csr[0x3a0] = 0x0f | (UINT64_C(0x10) << 8) |
                     (UINT64_C(0x0f) << 16);
    env.csr[0x3b0] = UINT64_C(0x9000) >> 2;
    env.csr[0x3b1] = UINT64_C(0x9000) >> 2;
    env.csr[0x3b2] = (UINT64_C(1) << 54) - 1;
    hits = env.tlb_hits;
    check(latr_walk(&env, UINT64_C(0x8000), LATR_ACCESS_LOAD, &pa),
          LATR_EXCP_LOAD_ACCESS_FAULT,
          "PMP denies a cached translation");
    check(env.tlb_hits, hits, "PMP-denied lookup is not a hit");
}

static void test_mmu_data_path(void)
{
    uint64_t pa_page = (uint64_t)(uintptr_t)&data_word & ~UINT64_C(0xfff);
    uint64_t offset = (uint64_t)(uintptr_t)&data_word & 0xfff;
    uint64_t va = UINT64_C(0x4000) + offset;
    uint64_t sant = (UINT64_C(8) << 60) |
                    ((uint64_t)(uintptr_t)pte_table[0] >> 12);

    clear_page_tables();
    map_page(8, UINT64_C(0x4000), pa_page,
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W | LATR_PTE_A | LATR_PTE_D);

    static const uint32_t store_load[] = {
        0x02a00093, /* addi x1, x0, 42 */
        0x00133023, /* sd x1, 0(x6) */
        0x00033183, /* ld x3, 0(x6) */
    };
    map_range_identity((uint64_t)(uintptr_t)store_load, sizeof(store_load));
    data_word = 0;
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    env.gpr[6] = va;
    check(latr_execute_block(store_load, 3), 1,
          "Sv39 data block translated");
    check(data_word, 42, "Sv39 store reaches the mapped physical page");
    check(env.gpr[3], 42, "Sv39 load returns the mapped value");

    static const uint32_t lr_sc[] = {
        0x100332af, /* lr.d x5, (x6) */
        0x03300493, /* addi x9, x0, 0x33 */
        0x189333af, /* sc.d x7, x9, (x6) */
    };
    map_range_identity((uint64_t)(uintptr_t)lr_sc, sizeof(lr_sc));
    data_word = 7;
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    env.gpr[6] = va;
    check(latr_execute_block(lr_sc, 3), 1, "Sv39 LR/SC block translated");
    check(env.gpr[5], 7, "Sv39 LR returns the mapped value");
    check(env.gpr[7], 0, "Sv39 SC succeeds");
    check(data_word, 0x33, "Sv39 SC stores through the mapping");
    check(env.reservation_addr, pa_page + offset,
          "reservation records the physical address");

    static const uint32_t bad_load[] = {0x00033183}; /* ld x3, 0(x6) */
    map_range_identity((uint64_t)(uintptr_t)bad_load, sizeof(bad_load));
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    env.gpr[6] = UINT64_C(0x8000);
    check(latr_execute_block(bad_load, 1), 1, "unmapped load translated");
    check(env.exception, 13, "unmapped load raises a load page fault");
    check(env.badaddr, UINT64_C(0x8000), "load page fault reports the VA");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)bad_load,
          "load page fault saves the faulting PC");

    static const uint32_t bad_store[] = {0x00133023}; /* sd x1, 0(x6) */
    map_range_identity((uint64_t)(uintptr_t)bad_store, sizeof(bad_store));
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x180] = sant;
    env.gpr[6] = UINT64_C(0x8000);
    check(latr_execute_block(bad_store, 1), 1, "unmapped store translated");
    check(env.exception, 15, "unmapped store raises a store page fault");
    check(env.badaddr, UINT64_C(0x8000), "store page fault reports the VA");
}

static int64_t sbi_test_set_timer(void *opaque, uint64_t when)
{
    (void)opaque;
    (void)when;
    return LOONGEMU_SBI_SUCCESS;
}

static void test_sbi_service(void)
{
    static const uint32_t probe_spec[] = {
        0x00000813, /* addi x16, x0, 0 */
        0x01000893, /* addi x17, x0, 0x10 (BASE) */
        0x00000073, /* ecall */
    };
    static const LoongemuSbiCalls calls = {
        .set_timer = sbi_test_set_timer,
    };
    LoongemuSbi sbi = {
        .calls = &calls,
        .hart_count = 1,
        .impl_id = LOONGEMU_SBI_IMPL_ID,
        .impl_version = 1,
    };

    /* An S-mode ECALL traps to M-mode; the VMM services it via SBI. */
    reset_guest_state();
    env.privilege = 1;
    check(latr_execute_block(probe_spec, 3), 1, "S-mode ECALL block runs");
    check(env.exception, 9, "S-mode ECALL raises cause 9");
    check(((env.csr[0x300] >> 11) & 3), 1, "trap entry records MPP = S");
    check(latr_service_sbi(&env, &sbi), 1, "SBI services the ECALL trap");
    check(env.gpr[10], 0, "SBI returns SUCCESS in a0");
    check(env.gpr[11], LOONGEMU_SBI_SPEC_VERSION,
          "SBI returns the spec version in a1");
    check(env.pc, (uint64_t)(uintptr_t)(probe_spec + 2) + 4,
          "SBI resumes after the ECALL");
    check(env.exception, 0, "SBI clears the trap cause");
    check(env.privilege, 1, "SBI keeps the guest in S-mode");

    /* Unsupported EID still returns a proper error pair. */
    static const uint32_t probe_bad[] = {
        0x00000813, /* addi x16, x0, 0 */
        0x77700893, /* addi x17, x0, 0x777 */
        0x00000073, /* ecall */
    };
    reset_guest_state();
    env.privilege = 1;
    check(latr_execute_block(probe_bad, 3), 1, "unsupported EID block runs");
    check(latr_service_sbi(&env, &sbi), 1, "SBI services unknown EIDs");
    check(env.gpr[10], (uint64_t)LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "unknown EID reports NOT_SUPPORTED");

    /* A user-mode ECALL (cause 8) is not an SBI call. */
    reset_guest_state();
    check(latr_execute_block(probe_spec, 3), 1, "U-mode ECALL block runs");
    check(env.exception, 8, "U-mode ECALL raises cause 8");
    check(latr_service_sbi(&env, &sbi), 0,
          "SBI ignores user-mode ECALL traps");
    check(env.gpr[10], 0, "ignored ECALL leaves a0 untouched");
}

static LoongemuVClock test_vclock;
static LoongemuPlic test_plic;

static void test_interrupts(void)
{
    loongemu_vclock_init(&test_vclock, LOONGEMU_VCLOCK_VIRTUAL_STEP,
                         1000000000, 0, 0, 0, NULL, NULL);

    /* Sstc: MIP.STIP follows the comparator against the vclock. */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 3;
    env.csr[0x14d] = 100; /* stimecmp */
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 5), 0, "STIP clear before the deadline");
    loongemu_vclock_advance_ns(&test_vclock, 100);
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 5), 1ULL << 5,
          "STIP set once the deadline passes");

    /* Machine-level delivery (not delegated). */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 3;
    env.pc = 0x80000000;
    env.csr[0x305] = 0x80001000;          /* mtvec */
    env.csr[0x300] = 1ULL << 3;           /* mstatus.MIE */
    env.csr[0x304] = 1ULL << 5;           /* mie.STIE */
    env.csr[0x14d] = 0;                   /* always expired */
    check(latr_interrupt_pending(&env), 1, "STIP is enabled and pending");
    check(latr_deliver_interrupt(&env), 1, "machine timer interrupt taken");
    check(env.exception, (1ULL << 63) | 5, "mcause is a timer interrupt");
    check(env.csr[0x342], (1ULL << 63) | 5,
          "interrupt delivery writes mcause CSR");
    check(env.csr[0x341], 0x80000000, "mepc records the interrupted PC");
    check(env.pc, 0x80001000, "trap vectors to mtvec");
    check(env.privilege, 3, "machine traps stay in M-mode");
    check((env.csr[0x300] >> 7) & 1, 1, "MIE moves into MPIE");
    check((env.csr[0x300] >> 3) & 1, 0, "MIE is cleared on entry");
    check(latr_deliver_interrupt(&env), 0,
          "no re-entry while MIE is clear");

    /* Delegated supervisor timer interrupt. */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 1;
    env.pc = 0x80000200;
    env.csr[0x105] = 0x80002000;          /* stvec */
    env.csr[0x300] = 1ULL << 1;           /* sstatus.SIE */
    env.csr[0x303] = 1ULL << 5;           /* mideleg.STIP */
    env.csr[0x304] = 1ULL << 5;
    env.csr[0x14d] = 0;
    check(latr_deliver_interrupt(&env), 1, "delegated timer interrupt taken");
    check(env.exception, (1ULL << 63) | 5, "scause is a timer interrupt");
    check(env.csr[0x142], (1ULL << 63) | 5,
          "delegated delivery writes scause CSR");
    check(env.csr[0x141], 0x80000200, "sepc records the interrupted PC");
    check(env.pc, 0x80002000, "trap vectors to stvec");
    check(env.privilege, 1, "delegated traps stay in S-mode");
    check((env.csr[0x300] >> 5) & 1, 1, "SIE moves into SPIE");
    check((env.csr[0x300] >> 1) & 1, 0, "SIE is cleared on entry");

    /* Vectored mtvec: MODE=1 jumps to BASE + 4 * cause. */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 3;
    env.pc = 0x80000400;
    env.csr[0x305] = 0x80001001;          /* mtvec, vectored */
    env.csr[0x300] = 1ULL << 3;           /* MIE */
    env.csr[0x304] = 1ULL << 5;           /* MTIE */
    env.csr[0x14d] = 0;
    check(latr_deliver_interrupt(&env), 1, "vectored machine timer taken");
    check(env.pc, 0x80001014, "mtvec vector offset is 4*cause");

    /* Vectored stvec and privilege-dependent SIE gating. */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 1;
    env.pc = 0x80000480;
    env.csr[0x105] = 0x80002001;          /* stvec, vectored */
    env.csr[0x300] = 1ULL << 1;           /* SIE */
    env.csr[0x303] = 1ULL << 5;           /* mideleg.STIP */
    env.csr[0x304] = 1ULL << 5;
    env.csr[0x14d] = 0;
    check(latr_deliver_interrupt(&env), 1, "vectored supervisor timer taken");
    check(env.pc, 0x80002014, "stvec vector offset is 4*cause");

    /* U-mode is below the delegated S target, so SIE does not gate it. */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 0;                    /* U-mode */
    env.pc = 0x80000500;
    env.csr[0x105] = 0x80002000;
    env.csr[0x300] = 0;                   /* SIE=0 */
    env.csr[0x303] = 1ULL << 5;
    env.csr[0x304] = 1ULL << 5;
    env.csr[0x14d] = 0;
    check(latr_deliver_interrupt(&env), 1,
          "U-mode delegated interrupt ignores SIE");
    check(env.csr[0x141], 0x80000500, "U-mode trap records sepc");
    check(env.csr[0x142], (1ULL << 63) | 5, "U-mode trap records scause");
    check(env.privilege, 1, "U-mode trap enters S-mode");
    check((env.csr[0x300] >> 8) & 1, 0, "SPP records U-mode origin");
    check((env.csr[0x300] >> 5) & 1, 0, "SPIE captures SIE=0");

    /* Disabled interrupts do not trap. */
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 1;
    env.csr[0x304] = 0;                   /* mie clear */
    env.csr[0x14d] = 0;
    check(latr_deliver_interrupt(&env), 0, "masked timer interrupt is ignored");

    /* PLIC: SEIP follows the claimable state of this hart's context. */
    check(loongemu_plic_init(&test_plic, 8, 1), 1, "PLIC initialises");
    {
        int err = 0;
        loongemu_plic_write32(&test_plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 3,
                              2, &err);
        loongemu_plic_write32(&test_plic, LOONGEMU_PLIC_ENABLE_BASE, 1u << 3,
                              &err);
    }
    reset_guest_state();
    env.vclock = &test_vclock;
    env.privilege = 3;
    env.plic = &test_plic;
    env.plic_context = 0;
    env.csr[0x305] = 0x80003000;
    env.csr[0x300] = 1ULL << 3;           /* MIE */
    env.csr[0x304] = 1ULL << 11;          /* mie.MEIE */
    loongemu_plic_set_irq(&test_plic, 3, 1);
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 11), 1ULL << 11,
          "PLIC pending raises MEIP");
    check(latr_deliver_interrupt(&env), 1, "external interrupt taken");
    check(env.exception, (1ULL << 63) | 11,
          "mcause is a machine external interrupt");
    check(env.csr[0x342], (1ULL << 63) | 11,
          "external delivery writes mcause CSR");
    check(env.pc, 0x80003000, "external trap vectors to mtvec");
    {
        int err = 0;
        check(loongemu_plic_read32(&test_plic, LOONGEMU_PLIC_CONTEXT_BASE + 4,
                                   &err),
              3, "guest claim returns the PLIC source");
    }
    loongemu_plic_set_irq(&test_plic, 3, 0);
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 11), 0, "MEIP drops with the line");

    /* Two contexts: context 1 is the S-mode context and drives SEIP. */
    check(loongemu_plic_init(&test_plic, 8, 2), 1,
          "PLIC dual-context initialises");
    {
        int err = 0;
        loongemu_plic_write32(&test_plic, LOONGEMU_PLIC_PRIORITY_BASE + 4 * 3,
                              2, &err);
        loongemu_plic_write32(&test_plic,
                              LOONGEMU_PLIC_ENABLE_BASE +
                              LOONGEMU_PLIC_ENABLE_STRIDE,
                              1u << 3, &err);
    }
    reset_guest_state();
    env.vclock = &test_vclock;
    env.plic = &test_plic;
    env.plic_context = 0;
    loongemu_plic_set_irq(&test_plic, 3, 1);
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 9), 1ULL << 9,
          "S-context pending raises SEIP");
    check(env.csr[0x344] & (1ULL << 11), 0,
          "S-context does not raise MEIP");
    env.privilege = 1;
    env.csr[0x105] = 0x80004000;
    env.csr[0x303] = 1ULL << 9;           /* delegate SEI */
    env.csr[0x304] = 1ULL << 9;           /* sie.SEIE */
    env.csr[0x300] = 1ULL << 1;           /* SIE */
    check(latr_deliver_interrupt(&env), 1,
          "delegated S external interrupt taken");
    check(env.csr[0x142], (1ULL << 63) | 9,
          "scause is a supervisor external interrupt");
    check(env.pc, 0x80004000, "S external trap vectors to stvec");
    {
        int err = 0;
        check(loongemu_plic_read32(&test_plic,
                                   LOONGEMU_PLIC_CONTEXT_BASE +
                                   LOONGEMU_PLIC_CONTEXT_STRIDE + 4, &err),
              3, "S-context claim returns the source");
    }
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 9), 0, "claimed source drops SEIP");
    {
        int err = 0;
        loongemu_plic_write32(&test_plic,
                              LOONGEMU_PLIC_CONTEXT_BASE +
                              LOONGEMU_PLIC_CONTEXT_STRIDE + 4, 3, &err);
    }
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 9), 1ULL << 9,
          "completion re-pends the asserted line");
    loongemu_plic_set_irq(&test_plic, 3, 0);

    /* WFI marks the hart idle for the runtime. */
    static const uint32_t wfi[] = {0x10500073};
    check(run_block(wfi, 1, -1, 0), 1, "WFI block translated");
    check(env.wfi_pending, 1, "WFI reports the idle hint");

    /* Idle fast-forward jumps to the Sstc deadline. */
    reset_guest_state();
    env.vclock = &test_vclock;
    loongemu_vclock_advance_ns(&test_vclock, 1000);
    uint64_t now0 = loongemu_vclock_now_ns(&test_vclock);
    env.csr[0x14d] = now0 + 5000;
    latr_idle_advance(&env);
    check(loongemu_vclock_now_ns(&test_vclock), now0 + 5000,
          "idle advance lands on the timer deadline");
    latr_update_interrupts(&env);
    check(env.csr[0x344] & (1ULL << 5), 1ULL << 5,
          "advancing to the deadline raises STIP");
}

static uint8_t tb_cache_arena[4096];
static uint8_t tb_cache_arena_b[2048];
static LatrTbCache tb_cache;
static LatrTbCache tb_cache_b;
static CPUArchState lbare_other_env;

static LatrTbKey test_tb_key(const CPUArchState *env)
{
    return latr_tb_cache_key(env);
}

static void test_tb_cache(void)
{
    static const uint8_t blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    LatrTbKey key1 = { 0, 0, 0, 0 };
    LatrTbKey key2 = { 0, 0, 0, 0 };
    LatrTbKey key3;
    LatrTbKey key4;

    latr_tb_cache_init(&tb_cache, tb_cache_arena, sizeof(tb_cache_arena));
    check(tb_cache.arena_size, sizeof(tb_cache_arena),
          "cache arena recorded");

    reset_guest_state();
    env.privilege = 3;
    env.csr[0x180] = (UINT64_C(8) << 60) | (UINT64_C(1) << 44);
    key1 = test_tb_key(&env);
    check(key1.satp_mode, 8, "cache key carries the satp mode");
    check(key1.asid, 1, "cache key carries the ASID");
    check(key1.privilege, 3, "cache key carries the privilege");

    LatrTbEntry *e1 = latr_tb_cache_insert(&tb_cache, 0x1000, key1, blob,
                                           sizeof(blob), 2);
    check(e1 != 0, 1, "cache insert succeeds");
    check(e1->code_size, sizeof(blob), "entry records the code size");
    check(e1->code[0], 1, "entry copies the code into the arena");

    env.csr[0x180] = (UINT64_C(8) << 60) | (UINT64_C(2) << 44);
    key2 = test_tb_key(&env);
    latr_tb_cache_insert(&tb_cache, 0x1000, key2, blob, sizeof(blob), 2);

    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key1) != 0, 1,
          "lookup hits the same key");
    check(latr_tb_cache_lookup(&tb_cache, 0x2000, key1) == 0, 1,
          "lookup misses a different PC");
    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key2) != 0, 1,
          "lookup distinguishes ASIDs");
    key3 = key1;
    key3.privilege = 1;
    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key3) == 0, 1,
          "lookup distinguishes privilege levels");
    key4 = key1;
    key4.satp_mode = 9;
    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key4) == 0, 1,
          "lookup distinguishes satp modes");
    check(tb_cache.arena_used, 2 * sizeof(blob),
          "arena usage tracks inserted code");

    latr_tb_shootdown_asid(&tb_cache, 1);
    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key1) == 0, 1,
          "ASID shootdown drops the ASID's TBs");
    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key2) != 0, 1,
          "ASID shootdown keeps other ASIDs");
    check(tb_cache.count, 1, "entry count follows shootdowns");

    latr_tb_shootdown_privilege(&tb_cache, 3);
    check(latr_tb_cache_lookup(&tb_cache, 0x1000, key2) == 0, 1,
          "privilege shootdown drops that level");

    latr_tb_cache_insert(&tb_cache, 0x3000, key1, blob, sizeof(blob), 1);
    latr_tb_shootdown_all(&tb_cache);
    check(latr_tb_cache_lookup(&tb_cache, 0x3000, key1) == 0, 1,
          "global shootdown drops everything");
    check(tb_cache.arena_used, sizeof(blob),
          "global shootdown keeps code mapped until a safe point");
    check(tb_cache.reclaim_pending, 1,
          "global shootdown requests reclamation");
    latr_tb_cache_safepoint(&tb_cache);
    check(tb_cache.arena_used, 0, "safe point frees the arena");
    check(tb_cache.reclaim_pending, 0, "safe point clears the request");
    check(tb_cache.generation > 1, 1, "shootdowns bump the generation");

    /* Arena exhaustion is reported, not silently truncated. */
    latr_tb_cache_init(&tb_cache, tb_cache_arena, sizeof(blob));
    check(latr_tb_cache_insert(&tb_cache, 0x4000, key1, blob, sizeof(blob),
                               1) != 0, 1, "insert fills the arena");
    check(latr_tb_cache_insert(&tb_cache, 0x5000, key1, blob, sizeof(blob),
                               1) == 0, 1,
          "insert fails when the arena is full");

    /* Cross-core shootdowns: register two vCPU caches. */
    latr_tb_cache_init(&tb_cache, tb_cache_arena, sizeof(tb_cache_arena));
    latr_tb_cache_init(&tb_cache_b, tb_cache_arena_b,
                       sizeof(tb_cache_arena_b));
    check(latr_tb_cache_register(&tb_cache), 1, "core A registers");
    check(latr_tb_cache_register(&tb_cache_b), 1, "core B registers");
    check(latr_tb_cache_register(&tb_cache), 1, "double register is a no-op");
    latr_tb_cache_insert(&tb_cache, 0x6000, key1, blob, sizeof(blob), 1);
    latr_tb_cache_insert(&tb_cache, 0x7000, key2, blob, sizeof(blob), 1);
    latr_tb_cache_insert(&tb_cache_b, 0x6000, key1, blob, sizeof(blob), 1);
    latr_tb_cache_insert(&tb_cache_b, 0x7000, key2, blob, sizeof(blob), 1);
    latr_tb_shootdown_asid_all_cores(1);
    check(latr_tb_cache_lookup(&tb_cache, 0x6000, key1) == 0, 1,
          "ASID shootdown reaches core A");
    check(latr_tb_cache_lookup(&tb_cache_b, 0x6000, key1) == 0, 1,
          "ASID shootdown reaches core B");
    check(latr_tb_cache_lookup(&tb_cache, 0x7000, key2) != 0, 1,
          "ASID shootdown keeps other ASIDs on core A");
    check(latr_tb_cache_lookup(&tb_cache_b, 0x7000, key2) != 0, 1,
          "ASID shootdown keeps other ASIDs on core B");
    latr_tb_fence_i_all_cores();
    check(latr_tb_cache_lookup(&tb_cache, 0x7000, key2) == 0, 1,
          "FENCE.I shootdown clears core A");
    check(latr_tb_cache_lookup(&tb_cache_b, 0x7000, key2) == 0, 1,
          "FENCE.I shootdown clears core B");
    latr_tb_cache_unregister(&tb_cache_b);
    latr_tb_cache_insert(&tb_cache, 0x8000, key2, blob, sizeof(blob), 1);
    latr_tb_cache_insert(&tb_cache_b, 0x8000, key2, blob, sizeof(blob), 1);
    latr_tb_fence_i_all_cores();
    check(latr_tb_cache_lookup(&tb_cache, 0x8000, key2) == 0, 1,
          "registered core is shot down");
    check(latr_tb_cache_lookup(&tb_cache_b, 0x8000, key2) != 0, 1,
          "unregistered core is skipped");
    latr_tb_cache_unregister(&tb_cache);

    /* Per-vCPU env switch. */
    lbare_other_env = env;
    latr_set_cpu_env(&lbare_other_env);
    check(lsenv->cpu_state == (void *)&lbare_other_env, 1,
          "runtime env switch");
    latr_set_cpu_env(&env);
    check(lsenv->cpu_state == (void *)&env, 1,
          "runtime env switches back");
}

static void test_translator_context(void)
{
    static CPUArchState ctx_env_a;
    static CPUArchState ctx_env_b;
    static uint8_t ctx_code_a[4096];
    static uint8_t ctx_code_b[4096];
    static uint8_t ctx_arena_a[4096];
    static uint8_t ctx_arena_b[4096];
    static LatrTbCache ctx_cache_a;
    static LatrTbCache ctx_cache_b;
    static TranslationBlock ctx_tb_a;
    static TranslationBlock ctx_tb_b;
    PvirInstruction insn = { 0 };
    LatrTranslatorContext *saved = latr_context_current();
    LatrTranslatorContext *a;
    LatrTranslatorContext *b;
    LatrTbKey key;
    void *array_a;

    check(saved != 0, 1, "bootstrap context is bound");
    memset(&ctx_env_a, 0, sizeof(ctx_env_a));
    memset(&ctx_env_b, 0, sizeof(ctx_env_b));
    ctx_env_a.privilege = 3;
    ctx_env_b.privilege = 1;
    latr_tb_cache_init(&ctx_cache_a, ctx_arena_a, sizeof(ctx_arena_a));
    latr_tb_cache_init(&ctx_cache_b, ctx_arena_b, sizeof(ctx_arena_b));
    key = latr_tb_cache_key(&ctx_env_a);
    a = latr_context_create(&ctx_env_a, &ctx_cache_a, ctx_code_a,
                            sizeof(ctx_code_a));
    b = latr_context_create(&ctx_env_b, &ctx_cache_b, ctx_code_b,
                            sizeof(ctx_code_b));
    check(a != 0 && b != 0, 1, "translator contexts allocate");
    check(a->pvir_cache != b->pvir_cache, 1,
          "each context owns its PVIR cache");

    latr_context_bind(a);
    latr_tr_data_init(&ctx_env_a);
    tr_init(&ctx_tb_a);
    array_a = tr_data->ir2_inst_array;
    check(lsenv->cpu_state == (void *)&ctx_env_a, 1,
          "context A binds its env");
    check(array_a != 0, 1, "context A allocates its IR2 array");

    latr_context_bind(b);
    latr_tr_data_init(&ctx_env_b);
    tr_init(&ctx_tb_b);
    check(tr_data->ir2_inst_array != array_a, 1,
          "context B gets a separate IR2 array");
    check(lsenv->cpu_state == (void *)&ctx_env_b, 1,
          "context B binds its env");

    latr_pvir_cache_store(UINT64_C(0xa5a5), 0x11, 4, &insn);
    check(latr_pvir_cache_lookup(UINT64_C(0xa5a5), 0x11, 4, 0), 1,
          "context B stores into its own PVIR cache");
    latr_context_bind(a);
    check(!latr_pvir_cache_lookup(UINT64_C(0xa5a5), 0x11, 4, 0), 1,
          "context A does not see B's PVIR entries");
    latr_pvir_cache_store(UINT64_C(0x5a5a), 0x22, 8, &insn);
    check(latr_context_current() == a, 1, "bind reports the active context");
    check(tr_data->ir2_inst_array == array_a, 1,
          "rebinding restores context A's IR2 array");

    latr_tb_cache_insert(a->tb_cache, 0x9000, key, ctx_code_a, 4, 1);
    check(latr_tb_cache_lookup(b->tb_cache, 0x9000, key) == 0, 1,
          "TB caches are per context");

    latr_tb_shootdown_all(a->tb_cache);
    check(latr_tb_cache_lookup(b->tb_cache, 0x9000, key) == 0, 1,
          "shootdown on A leaves B's cache alone");
    check(latr_pvir_cache_lookup(UINT64_C(0x5a5a), 0x22, 8, 0), 1,
          "PVIR entry survives until the safe point");
    latr_context_safepoint(a);
    check(!latr_pvir_cache_lookup(UINT64_C(0x5a5a), 0x22, 8, 0), 1,
          "safe point drops the hart's PVIR cache");
    check(a->pvir_generation == a->tb_cache->generation, 1,
          "safe point records the TB generation");

    latr_context_bind(saved);
    latr_context_destroy(a);
    latr_context_destroy(b);
    check(latr_context_current() == saved, 1,
          "binding restores the test hart");
}

static void test_sfence_shootdown(void)
{
    static const uint32_t with_asid[] = {
        0x00500113, /* addi x2, x0, 5 */
        0x12208073, /* sfence.vma x1, x2 */
    };
    reset_guest_state();
    env.privilege = 1;
    check(latr_execute_block(with_asid, 2), 1, "SFENCE.VMA with ASID runs");
    check(env.sfence_pending, 1, "SFENCE.VMA requests a shootdown");
    check(env.sfence_asid, 5, "SFENCE.VMA records the ASID");

    static const uint32_t all_asids[] = {
        0x00500113, /* addi x2, x0, 5 */
        0x12008073, /* sfence.vma x1, x0 */
    };
    reset_guest_state();
    env.privilege = 1;
    check(latr_execute_block(all_asids, 2), 1, "SFENCE.VMA all-ASID runs");
    check(env.sfence_pending, 1, "all-ASID SFENCE requests a shootdown");
    check(env.sfence_asid, UINT32_MAX, "all-ASID SFENCE records ALL");

    reset_guest_state();
    check(latr_execute_block(all_asids, 2), 1, "U-mode SFENCE traps");
    check(env.exception, 2, "U-mode SFENCE raises illegal instruction");
    check(env.sfence_pending, 0, "illegal SFENCE does not request a shootdown");
}

static void test_rvv_state(void)
{
    static const uint32_t vsetvli_avl[] = {0x0001f2d7}; /* vsetvli x5,x3,0x00 */
    static const uint32_t vsetvli_max[] = {0x011072d7}; /* vsetvli x5,x0,0x11 */
    static const uint32_t vsetvli_keep[] = {0x01107057}; /* vsetvli x0,x0,0x11 */
    static const uint32_t vsetivli[] = {0xc111f2d7};    /* vsetivli x5,3,0x11 */
    static const uint32_t vsetvl[] = {0x8071f2d7};      /* vsetvl x5,x3,x7 */
    static const uint32_t vsetvli_vill[] = {0x0041f2d7}; /* vtype 0x04 -> vill */

    /* Helper: vtype/vlmax against QEMU's algorithm (VLEN=128, ELEN=64). */
    reset_guest_state();
    check(latr_rvv_vsetvl(&env, 100, 0x00), 16,
          "SEW=8 LMUL=1 gives vlmax 16");
    check(env.vl, 16, "vl clamps to vlmax");
    check(env.vtype, 0, "vtype stores the encoded type");
    check(env.vill, 0, "legal vtype clears vill");
    check(env.csr[0x300] & 0x600, 0x600, "vsetvl marks VS dirty");
    env.vstart = 9;
    check(latr_rvv_vsetvl(&env, 3, 0x11), 3, "AVL below vlmax is kept");
    check(env.vstart, 0, "vsetvl clears vstart");
    check(latr_rvv_vsetvl(&env, UINT64_MAX, 0x1b), 16,
          "SEW=64 LMUL=8 gives vlmax 16");
    check(latr_rvv_vsetvl(&env, UINT64_MAX, 0x07), 8,
          "LMUL=1/2 gives 8 lanes at SEW=8");
    check(latr_rvv_vsetvl(&env, UINT64_MAX, 0x06), 4,
          "LMUL=1/4 gives 4 lanes at SEW=8");
    check(latr_rvv_vsetvl(&env, UINT64_MAX, 0x05), 2,
          "LMUL=1/8 gives 2 lanes at SEW=8");

    check(latr_rvv_vsetvl(&env, 5, 0x05 | (3 << 3)), 0,
          "SEW=64 with LMUL=1/8 is vill");
    check(env.vill, 1, "illegal fractional LMUL sets vill");
    check(env.vl, 0, "vill forces vl to zero");
    check(env.vtype, 0, "vill clears the stored vtype");
    check(env.vstart, 0, "vill clears vstart");
    check(latr_rvv_vsetvl(&env, 5, 0x04), 0, "reserved vlmul=4 is vill");
    check(latr_rvv_vsetvl(&env, 5, 0x400), 0, "reserved vtype bit 10 is vill");
    check(latr_rvv_vsetvl(&env, 5, 0x100), 0, "non-zero VEDIV is vill");
    check(latr_rvv_vsetvl(&env, 5, (UINT64_C(1) << 63)), 0,
          "vill input bit is honored");
    check(latr_rvv_vsetvl(&env, 5, 4 << 3), 0, "SEW=128 exceeds ELEN");
    check(latr_rvv_vsetvl(&env, 5, 0x00), 5, "a legal type recovers from vill");

    /* Guest vsetvli/vsetivli/vsetvl. */
    reset_guest_state();
    check(latr_execute_block(vsetvli_avl, 1), 1, "vsetvli with VS=0 runs");
    check(env.exception, 2, "vsetvli requires mstatus.VS");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)vsetvli_avl,
          "VS-disabled vsetvli records the faulting PC");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[3] = 3;
    check(latr_execute_block(vsetvli_avl, 1), 1, "vsetvli x5,x3 runs");
    check(env.gpr[5], 3, "vsetvli returns min(AVL, vlmax)");
    check(env.vl, 3, "guest vsetvli updates vl");
    check(env.pc, (uint64_t)(uintptr_t)vsetvli_avl + 4,
          "vsetvli advances and exits the TB");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    check(latr_execute_block(vsetvli_max, 1), 1, "vsetvli x5,x0 runs");
    check(env.gpr[5], 8, "rs1=x0 requests the maximum AVL");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vl = 5;
    check(latr_execute_block(vsetvli_keep, 1), 1, "vsetvli x0,x0 runs");
    check(env.vl, 5, "rd=x0,rs1=x0 keeps the current vl");
    check(env.gpr[0], 0, "vsetvli x0 preserves the zero register");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    check(latr_execute_block(vsetivli, 1), 1, "vsetivli runs");
    check(env.gpr[5], 3, "vsetivli uses the uimm AVL");
    check(env.vl, 3, "vsetivli updates vl");
    check(env.vtype, 0x11, "vsetivli stores the vtype immediate");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[3] = 100;
    env.gpr[7] = 0x11;
    check(latr_execute_block(vsetvl, 1), 1, "vsetvl runs");
    check(env.gpr[5], 8, "vsetvl clamps the runtime AVL");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[3] = 7;
    check(latr_execute_block(vsetvli_vill, 1), 1, "vill vsetvli runs");
    check(env.gpr[5], 0, "vill returns vl=0");
    check(env.vill, 1, "guest vill sets the state");
    check(env.vtype, 0, "guest vill clears vtype");

    /* Vector CSRs. */
    static const uint32_t csr_vlenb[] = {0xc22022f3};
    static const uint32_t csr_vl[] = {0xc20022f3};
    static const uint32_t csr_vtype[] = {0xc21022f3};
    static const uint32_t csr_wr_vstart[] = {0x00831073};
    static const uint32_t csr_wr_vxrm[] = {0x00a31073};
    static const uint32_t csr_wr_vxsat[] = {0x00931073};
    static const uint32_t csr_vcsr[] = {0x00f022f3};
    static const uint32_t csr_wr_vl[] = {0xc2031073};

    reset_guest_state();
    env.csr[0x300] = 0x600;
    check(latr_execute_block(csr_vlenb, 1), 1, "read vlenb");
    check(env.gpr[5], 16, "vlenb reports VLEN/8");
    env.vl = 7;
    env.vtype = 0x11;
    check(latr_execute_block(csr_vl, 1), 1, "read vl");
    check(env.gpr[5], 7, "vl CSR mirrors the state");
    check(latr_execute_block(csr_vtype, 1), 1, "read vtype");
    check(env.gpr[5], 0x11, "vtype CSR mirrors the state");
    env.vill = 1;
    check(latr_execute_block(csr_vtype, 1), 1, "read vtype with vill");
    check(env.gpr[5], (UINT64_C(1) << 63) | 0x11,
          "vtype CSR reports vill in bit 63");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[6] = 0xffff;
    check(latr_execute_block(csr_wr_vstart, 1), 1, "write vstart");
    check(env.vstart, 0x7f, "vstart keeps lg2(VLEN) bits");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[6] = 3;
    check(latr_execute_block(csr_wr_vxrm, 1), 1, "write vxrm");
    check(env.vxrm, 3, "vxrm stores the written value");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[6] = 1;
    check(latr_execute_block(csr_wr_vxsat, 1), 1, "write vxsat");
    check(env.vxsat, 1, "vxsat stores the written value");
    env.vxrm = 3; /* separate runs reset env; re-establish the alias */
    check(latr_execute_block(csr_vcsr, 1), 1, "read vcsr");
    check(env.gpr[5], 7, "vcsr combines vxrm and vxsat");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.gpr[6] = 1;
    check(latr_execute_block(csr_wr_vl, 1), 1, "write vl traps");
    check(env.exception, 2, "vl is read-only");

    reset_guest_state();
    check(latr_execute_block(csr_vlenb, 1), 1, "read vlenb with VS=0");
    check(env.exception, 2, "vector CSRs require mstatus.VS");
}

static void vec_store32(int reg, int lane, uint32_t value)
{
    env.vreg[reg][lane * 4 + 0] = (uint8_t)value;
    env.vreg[reg][lane * 4 + 1] = (uint8_t)(value >> 8);
    env.vreg[reg][lane * 4 + 2] = (uint8_t)(value >> 16);
    env.vreg[reg][lane * 4 + 3] = (uint8_t)(value >> 24);
}

static uint32_t vec_read32(int reg, int lane)
{
    return (uint32_t)env.vreg[reg][lane * 4 + 0] |
           ((uint32_t)env.vreg[reg][lane * 4 + 1] << 8) |
           ((uint32_t)env.vreg[reg][lane * 4 + 2] << 16) |
           ((uint32_t)env.vreg[reg][lane * 4 + 3] << 24);
}

static void vec_store64(int reg, int lane, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        env.vreg[reg][lane * 8 + i] = (uint8_t)(value >> (8 * i));
    }
}

static uint64_t vec_read64(int reg, int lane)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= (uint64_t)env.vreg[reg][lane * 8 + i] << (8 * i);
    }
    return value;
}

static void rvv_seed_operands(void)
{
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10; /* SEW=32, LMUL=1, tu, mu */
    env.vl = 4;
    env.vreg[0][0] = 0x05; /* lanes 0 and 2 active for masked forms */
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 0x80000000u);
    vec_store32(1, 2, 5);
    vec_store32(1, 3, 0xffffffffu);
    vec_store32(2, 0, 2);
    vec_store32(2, 1, 1);
    vec_store32(2, 2, 7);
    vec_store32(2, 3, 0xffffffffu);
    env.gpr[3] = 0xff00ff00u;
    env.gpr[6] = 4;
}

static void rvv_fill_sentinel(int reg)
{
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(reg, lane, 0xdeadbeefu);
    }
}

static void test_rvv_ops(void)
{
    static const uint32_t alu[] = {
        0x021101d7, /* vadd.vv   v3, v1, v2 */
        0x0a110257, /* vsub.vv   v4, v1, v2 */
        0x2611c2d7, /* vand.vx   v5, v1, x3 */
        0x96123357, /* vsll.vi   v6, v1, 4 */
        0xa61343d7, /* vsra.vx   v7, v1, x6 */
        0x62110457, /* vmseq.vv  v8, v1, v2 */
        0x6e1104d7, /* vmslt.vv  v9, v1, v2 */
        0x5c110557, /* vmerge.vvm v10, v1, v2, v0 */
        0x5e0fb5d7, /* vmv.v.i   v11, -1 */
        0x5e01c7d7, /* vmv.v.x   v15, x3 */
        0x5e008857, /* vmv.v.v   v16, v1 */
    };
    static const uint32_t masked_add[] = {
        0x001101d7, /* vadd.vv v3, v1, v2, v0.t */
    };
    static const uint32_t add[] = {
        0x021101d7, /* vadd.vv v3, v1, v2 */
    };

    rvv_seed_operands();
    check(latr_execute_block(alu + 5, 1), 1, "isolated vmseq translates");
    check(env.pvir_status, 0, "isolated compare PVIR status");
    check(env.vreg[8][0], 0x08, "isolated vmseq bits");
    rvv_seed_operands();
    check(latr_execute_block(alu + 6, 1), 1, "isolated vmslt translates");
    check(env.pvir_status, 0, "isolated vmslt PVIR status");
    check(env.vreg[9][0], 0x07, "isolated vmslt bits");
    rvv_seed_operands();
    check(latr_execute_block(alu, 11), 1, "vector ALU block translates");
    check(env.pvir_status, 0, "PVIR ALU status");
    check(env.pvir_count, 11, "PVIR ALU count");
    check(vec_read32(3, 0), 3, "vadd lane 0");
    check(vec_read32(3, 1), 0x80000001u, "vadd lane 1");
    check(vec_read32(3, 2), 12, "vadd lane 2");
    check(vec_read32(3, 3), 0xfffffffeu, "vadd lane 3");
    check(vec_read32(4, 0), 0xffffffffu, "vsub lane 0 wraps");
    check(vec_read32(4, 1), 0x7fffffffu, "vsub lane 1");
    check(vec_read32(4, 2), 0xfffffffeu, "vsub lane 2");
    check(vec_read32(4, 3), 0, "vsub lane 3");
    check(vec_read32(5, 0), 0, "vand.vx lane 0");
    check(vec_read32(5, 1), 0x80000000u, "vand.vx lane 1");
    check(vec_read32(5, 2), 0, "vand.vx lane 2");
    check(vec_read32(5, 3), 0xff00ff00u, "vand.vx lane 3");
    check(vec_read32(6, 0), 0x10, "vsll.vi lane 0");
    check(vec_read32(6, 1), 0, "vsll.vi lane 1");
    check(vec_read32(6, 2), 0x50, "vsll.vi lane 2");
    check(vec_read32(6, 3), 0xfffffff0u, "vsll.vi lane 3");
    check(vec_read32(7, 0), 0, "vsra lane 0");
    check(vec_read32(7, 1), 0xf8000000u, "vsra sign-extends");
    check(vec_read32(7, 2), 0, "vsra lane 2");
    check(vec_read32(7, 3), 0xffffffffu, "vsra lane 3");
    check(env.pvir_status, 0, "PVIR compare status");
    check(env.pvir_count, 11, "PVIR compare count");
    check(env.vreg[8][0], 0x08, "vmseq packs bit 3");
    check(env.pvir_status, 0, "PVIR vmslt status");
    check(env.vreg[9][0], 0x07, "vmslt signed compare");
    check(vec_read32(10, 0), 1, "vmerge selects vs2 when v0=1");
    check(vec_read32(10, 1), 1, "vmerge selects vs1 when v0=0");
    check(vec_read32(10, 2), 5, "vmerge lane 2");
    check(vec_read32(10, 3), 0xffffffffu, "vmerge lane 3");
    check(vec_read32(11, 0), 0xffffffffu, "vmv.v.i lane 0");
    check(vec_read32(11, 1), 0xffffffffu, "vmv.v.i lane 1");
    check(vec_read32(11, 2), 0xffffffffu, "vmv.v.i lane 2");
    check(vec_read32(11, 3), 0xffffffffu, "vmv.v.i sign-extends");
    check(vec_read32(15, 0), 0xff00ff00u, "vmv.v.x broadcasts");
    check(vec_read32(16, 1), 0x80000000u, "vmv.v.v copies");

    /* Masked execution with vma=1: inactive lanes become all ones. */
    rvv_seed_operands();
    env.vtype = 0x10 | (1u << 7);
    rvv_fill_sentinel(3);
    check(latr_execute_block(masked_add, 1), 1, "masked vector add translates");
    check(vec_read32(3, 0), 3, "masked add active lane 0");
    check(vec_read32(3, 1), 0xffffffffu, "masked add vma lane 1");
    check(vec_read32(3, 2), 12, "masked add active lane 2");
    check(vec_read32(3, 3), 0xffffffffu, "masked add vma lane 3");

    /* Tail-agnostic with vta=1: lanes >= vl become all ones. */
    rvv_seed_operands();
    env.vtype = 0x10 | (1u << 6);
    env.vl = 2;
    rvv_fill_sentinel(3);
    check(latr_execute_block(add, 1), 1, "vector add with vl=2 translates");
    check(vec_read32(3, 0), 3, "vta lane 0 computed");
    check(vec_read32(3, 1), 0x80000001u, "vta lane 1 computed");
    check(vec_read32(3, 2), 0xffffffffu, "vta tail lane 2");
    check(vec_read32(3, 3), 0xffffffffu, "vta tail lane 3");

    /* vstart restarts at the given lane and is cleared after completion. */
    rvv_seed_operands();
    env.vstart = 2;
    rvv_fill_sentinel(3);
    check(latr_execute_block(add, 1), 1, "vstart restart translates");
    check(vec_read32(3, 0), 0xdeadbeefu, "vstart skips lane 0");
    check(vec_read32(3, 1), 0xdeadbeefu, "vstart skips lane 1");
    check(vec_read32(3, 2), 12, "vstart resumes lane 2");
    check(vec_read32(3, 3), 0xfffffffeu, "vstart resumes lane 3");
    check(env.vstart, 0, "vstart cleared after the instruction");

    rvv_seed_operands();
    env.vstart = 4;
    rvv_fill_sentinel(3);
    check(latr_execute_block(add, 1), 1, "vstart >= vl translates");
    check(vec_read32(3, 0), 0xdeadbeefu, "vstart >= vl does no work");
    check(env.vstart, 0, "vstart >= vl clears vstart");

    /* Isolated vmv.v.i translation. */
    static const uint32_t vmv_i_only[] = { 0x5e0fb5d7 };
    rvv_seed_operands();
    env.vl = 1;
    check(latr_execute_block(vmv_i_only, 1), 1, "isolated vmv.v.i translates");
    check(vec_read32(11, 0), 0xffffffffu, "isolated vmv.v.i lane 0");
    check(env.vstart, 0, "isolated vmv.v.i clears vstart");

    /* Direct helper call isolates translation from helper semantics. */
    reset_guest_state();
    env.vtype = 0x10;
    env.vl = 4;
    env.rvv_args[0] = 11;
    env.rvv_args[1] = 31;
    env.rvv_args[2] = 2; /* LATR_RVV_SRC_IMM */
    env.rvv_args[3] = (uint64_t)-1;
    env.rvv_args[4] = 1;
    latr_rvv_mv(&env);
    check(vec_read32(11, 0), 0xffffffffu, "direct vmv lane 0");
    check(vec_read32(11, 1), 0xffffffffu, "direct vmv lane 1");
    check(vec_read32(11, 2), 0xffffffffu, "direct vmv lane 2");
    check(vec_read32(11, 3), 0xffffffffu, "direct vmv lane 3");

    /* Gate checks: VS=Off and vill both raise illegal instructions. */
    rvv_seed_operands();
    env.csr[0x300] = 0;
    check(latr_execute_block(add, 1), 1, "VS=0 block translates");
    check(env.exception, 2, "vector op with VS=0 is illegal");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)add,
          "VS=0 trap reports the vector PC");

    rvv_seed_operands();
    env.vill = 1;
    check(latr_execute_block(add, 1), 1, "vill block translates");
    check(env.exception, 2, "vector op with vill is illegal");
}

static void test_rvv_memory(void)
{
    static const uint32_t mem_code[] = {
        0x0203e607, /* vle32.v v12, (x7) */
        0x02046627, /* vse32.v v12, (x8) */
        0x0204e687, /* vle32.v v13, (x9) */
        0x02038087, /* vle8.v  v1, (x7) */
        0x020470a7, /* vse64.v v1, (x8) */
        0x0003e607, /* vle32.v v12, (x7), v0.t */
        0x00046627, /* vse32.v v12, (x8), v0.t */
    };
    static uint8_t rvv_data[4096] __attribute__((aligned(4096)));
    static uint8_t rvv_copy[4096] __attribute__((aligned(4096)));
    static LoongemuMemMap rvv_map;

    for (int i = 0; i < 32; i++) {
        rvv_data[i] = (uint8_t)(i + 1);
        rvv_copy[i] = 0xaa;
    }
    loongemu_mem_map_init(&rvv_map);
    map_host_range(&rvv_map, mem_code, 0x1000);
    check(loongemu_mem_map_add(&rvv_map, 0x10000, 0x1000,
                               (uint64_t)(uintptr_t)rvv_data,
                               LOONGEMU_MEM_RAM, "rvv-data"), 1,
          "vector data page registers");
    check(loongemu_mem_map_add(&rvv_map, 0x11000, 0x1000,
                               (uint64_t)(uintptr_t)rvv_copy,
                               LOONGEMU_MEM_RAM, "rvv-copy"), 1,
          "vector copy page registers");

    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    env.gpr[8] = 0x11000;
    check(latr_execute_block(mem_code, 2), 1, "vector load/store block translates");
    check(vec_read32(12, 0), 0x04030201u, "vle32 lane 0");
    check(vec_read32(12, 1), 0x08070605u, "vle32 lane 1");
    check(vec_read32(12, 3), 0x100f0e0du, "vle32 lane 3");
    check((uint32_t)rvv_copy[0] | ((uint32_t)rvv_copy[1] << 8) |
          ((uint32_t)rvv_copy[2] << 16) | ((uint32_t)rvv_copy[3] << 24),
          0x04030201u, "vse32 lane 0");
    check(rvv_copy[12] == 0x0d && rvv_copy[15] == 0x10, 1,
          "vse32 stores all active lanes");
    check(rvv_copy[16], 0xaa, "vse32 leaves memory beyond vl untouched");

    /* SEW=64 store. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x18; /* SEW=64, LMUL=1 */
    env.vl = 2;
    env.gpr[8] = 0x11000;
    vec_store64(1, 0, UINT64_C(0x1122334455667788));
    vec_store64(1, 1, UINT64_C(0xaabbccddeeff0011));
    check(latr_execute_block(mem_code + 4, 1), 1, "vse64 block translates");
    check(vec_read64(1, 0), UINT64_C(0x1122334455667788), "vse64 source lane 0");
    check(rvv_copy[0], 0x88, "vse64 stores byte 0");
    check(rvv_copy[7], 0x11, "vse64 stores byte 7");
    check(rvv_copy[8], 0x11, "vse64 stores byte 8");
    check(rvv_copy[15], 0xaa, "vse64 keeps the second lane untouched");

    /* SEW=8 load. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00; /* SEW=8 */
    env.vl = 4;
    env.gpr[7] = 0x10000;
    check(latr_execute_block(mem_code + 3, 1), 1, "vle8 block translates");
    check(env.vreg[1][0], 1, "vle8 lane 0");
    check(env.vreg[1][1], 2, "vle8 lane 1");
    check(env.vreg[1][3], 4, "vle8 lane 3");
    check(env.vreg[1][4], 0, "vle8 leaves the tail untouched");

    /* Masked load with vma=1. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10 | (1u << 7);
    env.vl = 4;
    env.gpr[7] = 0x10000;
    env.vreg[0][0] = 0x05;
    rvv_fill_sentinel(12);
    check(latr_execute_block(mem_code + 5, 1), 1, "masked load translates");
    check(vec_read32(12, 0), 0x04030201u, "masked load active lane 0");
    check(vec_read32(12, 1), 0xffffffffu, "masked load vma lane 1");
    check(vec_read32(12, 2), 0x0c0b0a09u, "masked load active lane 2");
    check(vec_read32(12, 3), 0xffffffffu, "masked load vma lane 3");

    /* Masked store: only active lanes reach memory. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[8] = 0x11000;
    env.vreg[0][0] = 0x05;
    vec_store32(12, 0, 0x11223344u);
    vec_store32(12, 1, 0x55667788u);
    vec_store32(12, 2, 0x99aabbccu);
    vec_store32(12, 3, 0xddeeff00u);
    for (int i = 0; i < 16; i++) {
        rvv_copy[i] = 0xaa;
    }
    check(latr_execute_block(mem_code + 6, 1), 1, "masked store translates");
    check(rvv_copy[0], 0x44, "masked store lane 0 written");
    check(rvv_copy[4], 0xaa, "masked store lane 1 skipped");
    check(rvv_copy[8], 0xcc, "masked store lane 2 written");
    check(rvv_copy[12], 0xaa, "masked store lane 3 skipped");

    /* An unmapped element address traps with vstart naming the lane. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[9] = 0x30000;
    check(latr_execute_block(mem_code + 2, 1), 1, "faulting vector load translates");
    check(env.exception, 5, "unmapped vector load raises an access fault");
    check(env.vstart, 0, "faulting lane 0 sets vstart=0");
    check(env.badaddr, 0x30000, "vector fault reports the element address");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)(mem_code + 2),
          "vector fault reports the instruction PC");

    /* vstart restart for a load: earlier lanes stay untouched. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vstart = 2;
    env.gpr[7] = 0x10000;
    rvv_fill_sentinel(12);
    check(latr_execute_block(mem_code, 1), 1, "vector load restart translates");
    check(vec_read32(12, 0), 0xdeadbeefu, "load restart skips lane 0");
    check(vec_read32(12, 1), 0xdeadbeefu, "load restart skips lane 1");
    check(vec_read32(12, 2), 0x0c0b0a09u, "load restart resumes lane 2");
    check(vec_read32(12, 3), 0x100f0e0du, "load restart resumes lane 3");
    check(env.vstart, 0, "load restart clears vstart");

    /* Load tail with vta=1 fills the lanes beyond vl with ones. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &rvv_map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10 | (1u << 6);
    env.vl = 2;
    env.gpr[7] = 0x10000;
    rvv_fill_sentinel(12);
    check(latr_execute_block(mem_code, 1), 1, "vector load tail translates");
    check(vec_read32(12, 1), 0x08070605u, "load tail lane 1 loaded");
    check(vec_read32(12, 2), 0xffffffffu, "load tail lane 2 filled");
    check(vec_read32(12, 3), 0xffffffffu, "load tail lane 3 filled");
}


/*
 * Whole-register moves (vmv<n>r / vl<n>re / vs<n>r), segment accesses (nf>1),
 * fault-only-first loads and the byte-granular mask transfer (vlm.v/vsm.v).
 *
 * Every expected value here was measured on QEMU first (`-cpu max`, i.e.
 * rvv_ta_all_1s=false, probes build/probe8.S and build/probe9.S) rather than
 * read off the spec, because the corner cases are the point:
 *   - vl<n>re / vs<n>r ignore vl *and* vtype, moving vlmax*nf whole elements
 *     and treating vstart as an element index;
 *   - vmv<n>r does nothing at all when vstart != 0, but still clears vstart;
 *   - segment faults write back the lanes that already completed and name the
 *     faulting *lane* in vstart;
 *   - vle<eew>ff narrows vl to the faulting lane without raising, and neither
 *     vta nor vma touches the tail;
 *   - vlm.v transfers ceil(vl/8) bytes and leaves the rest of the mask
 *     register alone even with vta=1.
 */
static void test_rvv_whole_segment_ldff(void)
{
    static const uint32_t code[] = {
        0x2283e207, /* 0  vl2re32.v   v4, (x7)  */
        0x22840227, /* 1  vs2r.v      v4, (x8)  */
        0x9e80b257, /* 2  vmv2r.v     v4, v8    */
        0x9e83bc57, /* 3  vmv8r.v     v24, v8   */
        0x2203e207, /* 4  vlseg2e32.v v4, (x7)  */
        0x22046227, /* 5  vsseg2e32.v v4, (x8)  */
        0x0303e207, /* 6  vle32ff.v   v4, (x7)  */
        0x02b38407, /* 7  vlm.v       v8, (x7)  */
        0x02b40427, /* 8  vsm.v       v8, (x8)  */
        0x0283e207, /* 9  vl1re32.v   v4, (x7)  */
        0x9e803257, /* 10 vmv1r.v     v4, v8    */
        0x4203e207, /* 11 vlseg3e32.v v4, (x7)  */
        0xe283ec07, /* 12 vl8re32.v   v24, (x7) */
    };
    static uint8_t page[4096] __attribute__((aligned(4096)));
    static uint8_t copy[4096] __attribute__((aligned(4096)));
    static uint8_t edge[4096] __attribute__((aligned(4096)));
    static LoongemuMemMap map;

    for (int i = 0; i < 4096; i++) {
        page[i] = (uint8_t)(i + 1);
        edge[i] = (uint8_t)(0xa0 + i);
        copy[i] = 0xaa;
    }
    loongemu_mem_map_init(&map);
    map_host_range(&map, code, 0x1000);
    check(loongemu_mem_map_add(&map, 0x10000, 0x1000,
                               (uint64_t)(uintptr_t)page,
                               LOONGEMU_MEM_RAM, "whole-data"), 1,
          "whole-register data page registers");
    check(loongemu_mem_map_add(&map, 0x11000, 0x1000,
                               (uint64_t)(uintptr_t)copy,
                               LOONGEMU_MEM_RAM, "whole-copy"), 1,
          "whole-register copy page registers");
    /* Only the first page is mapped: 0x21000 faults, which is what the
     * fault-only-first and segment-fault cases below rely on. */
    check(loongemu_mem_map_add(&map, 0x20000, 0x1000,
                               (uint64_t)(uintptr_t)edge,
                               LOONGEMU_MEM_RAM, "edge-page"), 1,
          "edge page registers");

    /* vl2re32 ignores vl=1 and SEW=8: it moves two whole registers. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00; /* SEW=8, LMUL=1, tu, mu */
    env.vl = 1;
    env.gpr[7] = 0x10000;
    rvv_fill_sentinel(4);
    rvv_fill_sentinel(5);
    check(latr_execute_block(code, 1), 1, "vl2re32 translates");
    check(vec_read32(4, 0), 0x04030201u, "vl2re32 ignores vl: byte 0");
    check(vec_read32(4, 3), 0x100f0e0du, "vl2re32 fills the whole first register");
    check(vec_read32(5, 0), 0x14131211u, "vl2re32 crosses into the second register");
    check(vec_read32(5, 3), 0x201f1e1du, "vl2re32 fills the whole second register");
    check(env.vl, 1, "vl2re32 leaves vl alone");
    check(env.vstart, 0, "vl2re32 leaves vstart clear");

    /* vstart is an element index: 3 elements of e32 = 12 bytes. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 4;
    env.vstart = 3;
    env.gpr[7] = 0x10000;
    rvv_fill_sentinel(4);
    rvv_fill_sentinel(5);
    check(latr_execute_block(code, 1), 1, "vl2re32 restart translates");
    check(vec_read32(4, 0), 0xdeadbeefu, "vl2re32 restart skips byte 0");
    check(vec_read32(4, 2), 0xdeadbeefu, "vl2re32 restart skips byte 8");
    check(vec_read32(4, 3), 0x100f0e0du, "vl2re32 restart resumes at byte 12");
    check(vec_read32(5, 0), 0x14131211u, "vl2re32 restart runs into the second register");
    check(env.vstart, 0, "vl2re32 restart clears vstart");

    /* vmv<n>r is a no-op once vstart != 0, but still clears vstart. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vstart = 2;
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(8, lane, 0x11223344u);
        vec_store32(9, lane, 0x55667788u);
    }
    rvv_fill_sentinel(4);
    rvv_fill_sentinel(5);
    check(latr_execute_block(code + 2, 1), 1, "vmv2r translates");
    check(vec_read32(4, 0), 0xdeadbeefu, "vmv2r with vstart set moves nothing");
    check(vec_read32(5, 0), 0xdeadbeefu, "vmv2r with vstart set leaves the second register");
    check(vec_read32(8, 0), 0x11223344u, "vmv2r leaves the source alone");
    check(env.vstart, 0, "vmv2r clears vstart even when it does nothing");

    /* With vstart clear it copies vlmax*nf elements, ignoring vl. */
    env.vl = 1;
    check(latr_execute_block(code + 2, 1), 1, "vmv2r with vstart clear translates");
    check(vec_read32(4, 0), 0x11223344u, "vmv2r copies lane 0");
    check(vec_read32(4, 3), 0x11223344u, "vmv2r ignores vl and copies lane 3");
    check(vec_read32(5, 0), 0x55667788u, "vmv2r copies the second register");

    /* vmv1r moves exactly one register; vmv8r moves eight. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 1;
    for (int r = 8; r < 16; r++) {
        for (int lane = 0; lane < 4; lane++) {
            vec_store32(r, lane, 0x0a000000u + (uint32_t)r);
        }
    }
    rvv_fill_sentinel(4);
    rvv_fill_sentinel(5);
    check(latr_execute_block(code + 10, 1), 1, "vmv1r translates");
    check(vec_read32(4, 0), 0x0a000008u, "vmv1r copies its source");
    check(vec_read32(5, 0), 0xdeadbeefu, "vmv1r leaves the next register");

    for (int r = 24; r < 32; r++) {
        for (int lane = 0; lane < 4; lane++) {
            vec_store32(r, lane, 0xdeadbeefu);
        }
    }
    check(latr_execute_block(code + 3, 1), 1, "vmv8r translates");
    check(vec_read32(24, 0), 0x0a000008u, "vmv8r copies the first register");
    check(vec_read32(28, 3), 0x0a00000cu, "vmv8r copies the fifth register");
    check(vec_read32(31, 3), 0x0a00000fu, "vmv8r copies the eighth register");

    /* vl8re32 moves eight whole registers = 128 bytes at SEW=8. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 1;
    env.gpr[7] = 0x10000;
    check(latr_execute_block(code + 12, 1), 1, "vl8re32 translates");
    check(env.vreg[24][0], 1, "vl8re32 fills byte 0");
    check(env.vreg[24][15], 16, "vl8re32 fills the first register");
    check(env.vreg[27][0], 49, "vl8re32 crosses into the fourth register");
    check(env.vreg[31][15], 128, "vl8re32 fills the last of eight registers");

    /* vl1re32 at vstart=3 restarts at element 3 instead of reloading. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 4;
    env.vstart = 3;
    env.gpr[7] = 0x10000;
    rvv_fill_sentinel(4);
    check(latr_execute_block(code + 9, 1), 1, "vl1re32 restart translates");
    check(env.vreg[4][11], 0xde, "vl1re32 restart skips the first elements");
    check(env.vreg[4][12], 13, "vl1re32 restart resumes at element 3");
    check(env.vreg[4][15], 16, "vl1re32 restart still fills the whole register");
    check(env.vstart, 0, "vl1re32 restart clears vstart");

    /* vlseg2e32 interleaves: field k of lane i lands in register vd+k. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    check(latr_execute_block(code + 4, 1), 1, "vlseg2e32 translates");
    check(vec_read32(4, 0), 0x04030201u, "vlseg2e32 lane 0 field 0");
    check(vec_read32(4, 1), 0x0c0b0a09u, "vlseg2e32 lane 1 field 0 skips a field");
    check(vec_read32(5, 0), 0x08070605u, "vlseg2e32 lane 0 field 1");
    check(vec_read32(5, 3), 0x201f1e1du, "vlseg2e32 lane 3 field 1");

    /* nf=3 is legal and not a power of two. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    check(latr_execute_block(code + 11, 1), 1, "vlseg3e32 translates");
    check(vec_read32(4, 0), 0x04030201u, "vlseg3e32 lane 0 field 0");
    check(vec_read32(5, 0), 0x08070605u, "vlseg3e32 lane 0 field 1");
    check(vec_read32(6, 0), 0x0c0b0a09u, "vlseg3e32 lane 0 field 2");
    check(vec_read32(4, 1), 0x100f0e0du, "vlseg3e32 lane 1 strides three fields");

    /* vsseg2e32 writes the same interleaving back out. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[8] = 0x11000;
    vec_store32(4, 0, 0x11111111u);
    vec_store32(4, 1, 0x22222222u);
    vec_store32(4, 2, 0x55555555u);
    vec_store32(5, 0, 0x33333333u);
    vec_store32(5, 1, 0x44444444u);
    for (int i = 0; i < 64; i++) {
        copy[i] = 0xaa;
    }
    check(latr_execute_block(code + 5, 1), 1, "vsseg2e32 translates");
    check(copy[0], 0x11, "vsseg2e32 lane 0 field 0 byte 0");
    check(copy[4], 0x33, "vsseg2e32 lane 0 field 1");
    check(copy[8], 0x22, "vsseg2e32 lane 1 field 0");
    check(copy[12], 0x44, "vsseg2e32 lane 1 field 1");
    check(copy[16], 0x55, "vsseg2e32 lane 2 field 0 comes from v4");
    check(copy[32], 0xaa, "vsseg2e32 leaves memory beyond vl untouched");

    /* Segment fault: completed lanes stay written, vstart names the lane. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x20ff0;
    rvv_fill_sentinel(4);
    rvv_fill_sentinel(5);
    check(latr_execute_block(code + 4, 1), 1, "faulting vlseg2e32 translates");
    check(env.exception, 5, "segment fault raises an access fault");
    check(env.vstart, 2, "segment fault names the faulting lane");
    check(env.badaddr, 0x21000, "segment fault reports the element address");
    check(vec_read32(4, 0), 0x93929190u, "segment fault keeps lane 0 field 0");
    check(vec_read32(5, 0), 0x97969594u, "segment fault keeps lane 0 field 1");
    check(vec_read32(4, 1), 0x9b9a9998u, "segment fault keeps lane 1 field 0");
    check(vec_read32(4, 2), 0xdeadbeefu, "segment fault leaves the faulting lane");

    /* vle32ff narrows vl instead of raising, and ignores vta/vma. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10 | (1u << 6) | (1u << 7); /* ta, ma */
    env.vl = 4;
    env.gpr[7] = 0x20ff8;
    rvv_fill_sentinel(4);
    check(latr_execute_block(code + 6, 1), 1, "vle32ff translates");
    check(env.exception, 0, "vle32ff does not raise");
    check(env.vl, 2, "vle32ff narrows vl to the faulting lane");
    check(env.vstart, 0, "vle32ff clears vstart");
    check(vec_read32(4, 0), 0x9b9a9998u, "vle32ff keeps the loaded lanes");
    check(vec_read32(4, 1), 0x9f9e9d9cu, "vle32ff loads up to the faulting lane");
    check(vec_read32(4, 2), 0xdeadbeefu, "vle32ff leaves the tail despite vta/vma");

    /* vlm.v moves ceil(vl/8) bytes and never touches the tail. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00; /* tu, mu */
    env.vl = 4;
    env.gpr[7] = 0x20000;
    for (int i = 0; i < LATR_RVV_VLENB; i++) {
        env.vreg[8][i] = 0x5a;
    }
    check(latr_execute_block(code + 7, 1), 1, "vlm translates");
    check(env.vreg[8][0], 0xa0, "vlm moves one byte for vl=4");
    check(env.vreg[8][1], 0x5a, "vlm leaves the tail with vta=0");
    check(env.vreg[8][15], 0x5a, "vlm leaves the rest of the mask register");

    env.vtype = 0x00 | (1u << 6); /* vta=1 must behave the same way */
    for (int i = 0; i < LATR_RVV_VLENB; i++) {
        env.vreg[8][i] = 0x5a;
    }
    check(latr_execute_block(code + 7, 1), 1, "vlm with vta=1 translates");
    check(env.vreg[8][0], 0xa0, "vlm vta=1 still moves one byte");
    check(env.vreg[8][1], 0x5a, "vlm vta=1 leaves the tail alone");

    /* vsm.v mirrors it: one byte out, the rest of the page untouched. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[8] = 0x11000;
    env.vreg[8][0] = 0x5a;
    for (int i = 0; i < 64; i++) {
        copy[i] = 0xaa;
    }
    check(latr_execute_block(code + 8, 1), 1, "vsm translates");
    check(copy[0], 0x5a, "vsm stores one byte for vl=4");
    check(copy[1], 0xaa, "vsm leaves the next byte untouched");

    /* vs2r stores two whole registers whatever vl says. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 1;
    env.gpr[8] = 0x11000;
    for (int i = 0; i < LATR_RVV_VLENB; i++) {
        env.vreg[4][i] = 0x11;
        env.vreg[5][i] = 0x22;
    }
    for (int i = 0; i < 32; i++) {
        copy[i] = 0xaa;
    }
    check(latr_execute_block(code + 1, 1), 1, "vs2r translates");
    check(copy[0], 0x11, "vs2r stores the first register");
    check(copy[15], 0x11, "vs2r stores all of the first register");
    check(copy[16], 0x22, "vs2r stores the second register");
    check(copy[31], 0x22, "vs2r stores all of the second register");
}

/*
 * vstart restart across the non-memory families.
 *
 * vstart is not part of the TB key, so a block translated while vstart == 0
 * can be entered again with vstart != 0.  Each case below therefore translates
 * once and then enters that same TB twice: first with vstart == 0 (fast path
 * taken), then with a non-zero vstart, where the lanes below vstart must stay
 * exactly as they were instead of being recomputed.
 */
static void test_rvv_vstart_restart(void)
{
    static const uint32_t code[] = {
        0x021101d7, /* 0  vadd.vv        v3, v1, v2       */
        0x001101d7, /* 1  vadd.vv        v3, v1, v2, v0.t */
        0x021111d7, /* 2  vfadd.vv       v3, v1, v2       */
        0x661121d7, /* 3  vmand.mm       v3, v1, v2       */
        0x5c1101d7, /* 4  vmerge.vvm     v3, v1, v2, v0   */
        0x3e12e1d7, /* 5  vslide1down.vx v3, v1, x5       */
        0xc6112257, /* 6  vwadd.vv       v4, v1, v2       */
    };

    /* --- integer ALU: the PVIR path and the LSX fast path agree on vstart. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10; /* e32, m1, tu, mu */
    env.vl = 4;
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(1, lane, (uint32_t)(1 + lane));
        vec_store32(2, lane, (uint32_t)(10 + 10 * lane));
    }
    check(latr_translate_block(code, 1), 1, "vadd TB translates once");
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read32(3, 0), 11, "vadd lane 0");
    check(vec_read32(3, 3), 44, "vadd lane 3");
    check(env.vstart, 0, "vadd clears vstart");

    for (int lane = 0; lane < 4; lane++) {
        vec_store32(1, lane, (uint32_t)(100 * (lane + 1)));
        vec_store32(2, lane, 1);
    }
    rvv_fill_sentinel(3);
    env.vstart = 2;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0xdeadbeefu, "vadd restart leaves lane 0 undisturbed");
    check(vec_read32(3, 1), 0xdeadbeefu, "vadd restart leaves lane 1 undisturbed");
    check(vec_read32(3, 2), 301, "vadd restart resumes at lane 2");
    check(vec_read32(3, 3), 401, "vadd restart resumes at lane 3");
    check(env.vstart, 0, "vadd restart clears vstart");

    /* --- masked ALU: vstart wins over an active mask bit below it. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[0][0] = 0x0f;
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(1, lane, (uint32_t)(1 + lane));
        vec_store32(2, lane, 10);
    }
    check(latr_translate_block(code + 1, 1), 1, "masked vadd TB translates once");
    rvv_fill_sentinel(3);
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read32(3, 0), 11, "masked vadd lane 0");
    check(vec_read32(3, 3), 14, "masked vadd lane 3");

    rvv_fill_sentinel(3);
    env.vstart = 1;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0xdeadbeefu, "masked vadd restart leaves lane 0");
    check(vec_read32(3, 1), 12, "masked vadd restart resumes at lane 1");
    check(vec_read32(3, 3), 14, "masked vadd restart resumes at lane 3");

    /* vma=1 must not treat the already-done lanes as inactive: below vstart
     * they stay as they were instead of being filled with ones. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10 | (1u << 7); /* e32, m1, tu, ma */
    env.vl = 4;
    env.vreg[0][0] = 0x05; /* lanes 0 and 2 active */
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(1, lane, (uint32_t)(1 + lane));
        vec_store32(2, lane, 10);
    }
    check(latr_translate_block(code + 1, 1), 1, "masked vadd vma TB translates once");
    rvv_fill_sentinel(3);
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read32(3, 0), 11, "masked vadd vma lane 0");
    check(vec_read32(3, 1), 0xffffffffu, "masked vadd vma fills the inactive lane");
    check(vec_read32(3, 2), 13, "masked vadd vma lane 2");

    rvv_fill_sentinel(3);
    env.vstart = 2;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0xdeadbeefu, "masked vadd vma restart keeps lane 0");
    check(vec_read32(3, 1), 0xdeadbeefu, "masked vadd vma restart keeps lane 1");
    check(vec_read32(3, 2), 13, "masked vadd vma restart resumes at lane 2");

    /* --- FP arithmetic. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 0x3f800000u); /* 1.0 */
    vec_store32(1, 1, 0x40000000u); /* 2.0 */
    vec_store32(1, 2, 0x40400000u); /* 3.0 */
    vec_store32(1, 3, 0x40800000u); /* 4.0 */
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(2, lane, 0x3f000000u); /* 0.5 */
    }
    check(latr_translate_block(code + 2, 1), 1, "vfadd TB translates once");
    rvv_fill_sentinel(3);
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0x3fc00000u, "vfadd 1.0 + 0.5");
    check(vec_read32(3, 3), 0x40900000u, "vfadd 4.0 + 0.5");

    rvv_fill_sentinel(3);
    env.vstart = 2;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0xdeadbeefu, "vfadd restart leaves lane 0 undisturbed");
    check(vec_read32(3, 1), 0xdeadbeefu, "vfadd restart leaves lane 1 undisturbed");
    check(vec_read32(3, 2), 0x40600000u, "vfadd restart resumes at lane 2");
    check(vec_read32(3, 3), 0x40900000u, "vfadd restart resumes at lane 3");

    /* --- mask logic: elements are bits, so vstart counts bits. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x00; /* e8, m1 */
    env.vl = 8;
    env.vreg[1][0] = 0xf0;
    env.vreg[2][0] = 0xf0;
    check(latr_translate_block(code + 3, 1), 1, "vmand TB translates once");
    env.vreg[3][0] = 0x00;
    env.vstart = 0;
    latr_reenter_tb();
    check(env.vreg[3][0], 0xf0, "vmand computes all active bits");

    env.vreg[3][0] = 0x0f;
    env.vstart = 4;
    latr_reenter_tb();
    check(env.vreg[3][0], 0xff, "vmand restart keeps the low bits and sets the high ones");

    /* --- vmerge. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[0][0] = 0x05;
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(1, lane, (uint32_t)(1 + lane));
        vec_store32(2, lane, (uint32_t)(10 + 10 * lane));
    }
    check(latr_translate_block(code + 4, 1), 1, "vmerge TB translates once");
    rvv_fill_sentinel(3);
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read32(3, 0), 1, "vmerge takes v1 where the mask is set");
    check(vec_read32(3, 1), 20, "vmerge takes v2 where the mask is clear");

    rvv_fill_sentinel(3);
    env.vstart = 2;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0xdeadbeefu, "vmerge restart leaves lane 0 undisturbed");
    check(vec_read32(3, 1), 0xdeadbeefu, "vmerge restart leaves lane 1 undisturbed");
    check(vec_read32(3, 2), 3, "vmerge restart resumes at lane 2");
    check(vec_read32(3, 3), 40, "vmerge restart resumes at lane 3");

    /* --- permute: vslide1down. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[5] = 0x7777;
    for (int lane = 0; lane < 4; lane++) {
        vec_store32(1, lane, (uint32_t)(1 + lane));
    }
    check(latr_translate_block(code + 5, 1), 1, "vslide1down TB translates once");
    rvv_fill_sentinel(3);
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read32(3, 0), 2, "vslide1down shifts lane 0");
    check(vec_read32(3, 3), 0x7777, "vslide1down feeds the scalar into the last lane");

    rvv_fill_sentinel(3);
    env.vstart = 2;
    latr_reenter_tb();
    check(vec_read32(3, 0), 0xdeadbeefu, "vslide1down restart leaves lane 0 undisturbed");
    check(vec_read32(3, 1), 0xdeadbeefu, "vslide1down restart leaves lane 1 undisturbed");
    check(vec_read32(3, 2), 4, "vslide1down restart resumes at lane 2");
    check(vec_read32(3, 3), 0x7777, "vslide1down restart still feeds the last lane");

    /* --- widening: the destination spans two registers. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 0x80000000u);
    vec_store32(1, 1, 5);
    vec_store32(2, 0, 0x80000000u);
    vec_store32(2, 1, 7);
    check(latr_translate_block(code + 6, 1), 1, "vwadd TB translates once");
    vec_store64(4, 0, 0);
    vec_store64(4, 1, 0);
    env.vstart = 0;
    latr_reenter_tb();
    check(vec_read64(4, 0), UINT64_C(0xffffffff00000000),
          "vwadd sign-extends the operands before widening");
    check(vec_read64(4, 1), 12, "vwadd lane 1");

    vec_store64(4, 0, UINT64_C(0xdeadbeefdeadbeef));
    vec_store64(4, 1, UINT64_C(0xdeadbeefdeadbeef));
    env.vstart = 1;
    latr_reenter_tb();
    check(vec_read64(4, 0), UINT64_C(0xdeadbeefdeadbeef),
          "vwadd restart leaves lane 0 undisturbed");
    check(vec_read64(4, 1), 12, "vwadd restart resumes at lane 1");
}

static void test_rvv_strided_indexed(void)
{
    static const uint32_t code[] = {
        0x0a83e607, /* vlse32.v   v12, (x7), x8 */
        0x0a83e627, /* vsse32.v   v12, (x7), x8 */
        0x0623e607, /* vluxei32.v v12, (x7), v2 */
        0x0623e627, /* vsuxei32.v v12, (x7), v2 */
        0x06348687, /* vluxei8.v  v13, (x9), v3 */
        0x0a83f707, /* vlse64.v   v14, (x7), x8 */
        0x0e23e787, /* vloxei32.v v15, (x7), v2 */
        0x0883e607, /* vlse32.v   v12, (x7), x8, v0.t */
        0x0423e607, /* vluxei32.v v12, (x7), v2, v0.t */
    };
    static uint8_t data[4096] __attribute__((aligned(4096)));
    static uint8_t copy[4096] __attribute__((aligned(4096)));
    static LoongemuMemMap map;

    for (int i = 0; i < 4096; i++) {
        data[i] = (uint8_t)i;
        copy[i] = 0xaa;
    }
    *(uint32_t *)&data[0] = 0x11111111u;
    *(uint32_t *)&data[8] = 0x22222222u;
    *(uint32_t *)&data[16] = 0x33333333u;
    *(uint32_t *)&data[24] = 0x44444444u;

    loongemu_mem_map_init(&map);
    map_host_range(&map, code, 0x1000);
    check(loongemu_mem_map_add(&map, 0x10000, 0x1000,
                               (uint64_t)(uintptr_t)data,
                               LOONGEMU_MEM_RAM, "si-data"), 1,
          "strided data page registers");
    check(loongemu_mem_map_add(&map, 0x11000, 0x1000,
                               (uint64_t)(uintptr_t)copy,
                               LOONGEMU_MEM_RAM, "si-copy"), 1,
          "strided copy page registers");

    /* misa.V is advertised. */
    static const uint32_t read_misa[] = { 0x301022f3 }; /* csrr x5, misa */
    reset_guest_state();
    env.privilege = 3;
    check(latr_execute_block(read_misa, 1), 1, "misa read translates");
    check(env.gpr[5], UINT64_C(0x800000000034112d), "misa advertises S/U/V");

    /* Strided load/store. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    env.gpr[8] = 8;
    check(latr_execute_block(code, 1), 1, "vlse32 translates");
    check(vec_read32(12, 0), 0x11111111u, "vlse32 lane 0");
    check(vec_read32(12, 1), 0x22222222u, "vlse32 lane 1");
    check(vec_read32(12, 3), 0x44444444u, "vlse32 lane 3");

    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x11000;
    env.gpr[8] = 8;
    vec_store32(12, 0, 0xa0a0a0a0u);
    vec_store32(12, 1, 0xb1b1b1b1u);
    vec_store32(12, 2, 0xc2c2c2c2u);
    vec_store32(12, 3, 0xd3d3d3d3u);
    check(latr_execute_block(code + 1, 1), 1, "vsse32 translates");
    check(*(uint32_t *)&copy[0], 0xa0a0a0a0u, "vsse32 lane 0");
    check(*(uint32_t *)&copy[8], 0xb1b1b1b1u, "vsse32 lane 1");
    check(*(uint32_t *)&copy[16], 0xc2c2c2c2u, "vsse32 lane 2");
    check(*(uint32_t *)&copy[4], 0xaaaaaaaaaaaaaaauLL & 0xffffffffu,
          "vsse32 leaves the gap untouched");

    /* Indexed 32-bit: indices are zero-extended and byte-scaled. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    vec_store32(2, 0, 0);
    vec_store32(2, 1, 8);
    vec_store32(2, 2, 16);
    vec_store32(2, 3, 24);
    check(latr_execute_block(code + 2, 1), 1, "vluxei32 translates");
    check(vec_read32(12, 0), 0x11111111u, "vluxei32 lane 0");
    check(vec_read32(12, 2), 0x33333333u, "vluxei32 lane 2");

    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x11000;
    vec_store32(2, 0, 0);
    vec_store32(2, 1, 8);
    vec_store32(2, 2, 16);
    vec_store32(2, 3, 24);
    vec_store32(12, 0, 0x01020304u);
    vec_store32(12, 3, 0x0a0b0c0du);
    check(latr_execute_block(code + 3, 1), 1, "vsuxei32 translates");
    check(*(uint32_t *)&copy[0], 0x01020304u, "vsuxei32 lane 0");
    check(*(uint32_t *)&copy[24], 0x0a0b0c0du, "vsuxei32 lane 3");

    /* Indexed with 8-bit indices and 64-bit data. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x18;
    env.vl = 2;
    env.gpr[9] = 0x10000;
    *(uint64_t *)&data[0] = UINT64_C(0x1122334455667788);
    *(uint64_t *)&data[8] = UINT64_C(0x99aabbccddeeff00);
    env.vreg[3][0] = 0;
    env.vreg[3][1] = 8;
    check(latr_execute_block(code + 4, 1), 1, "vluxei8 translates");
    check(vec_read64(13, 0), UINT64_C(0x1122334455667788), "vluxei8 lane 0");
    check(vec_read64(13, 1), UINT64_C(0x99aabbccddeeff00), "vluxei8 lane 1");

    /* Ordered indexed uses the same lane order for a functional emulator. */
    *(uint32_t *)&data[8] = 0x22222222u;
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    vec_store32(2, 0, 0);
    vec_store32(2, 1, 8);
    vec_store32(2, 2, 16);
    vec_store32(2, 3, 24);
    check(latr_execute_block(code + 6, 1), 1, "vloxei32 translates");
    check(vec_read32(15, 1), 0x22222222u, "vloxei32 lane 1");

    /* Masked strided load with vma=0 leaves inactive lanes untouched. */
    *(uint32_t *)&data[0] = 0x11111111u;
    *(uint32_t *)&data[8] = 0x22222222u;
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[7] = 0x10000;
    env.gpr[8] = 8;
    env.vreg[0][0] = 0x05;
    rvv_fill_sentinel(12);
    check(latr_execute_block(code + 7, 1), 1, "masked vlse32 translates");
    check(vec_read32(12, 0), 0x11111111u, "masked vlse32 lane 0");
    check(vec_read32(12, 2), 0x33333333u, "masked vlse32 lane 2");
    check(vec_read32(12, 1), 0xdeadbeefu, "masked vlse32 lane 1 untouched");

    /* An indexed lane fault reports the element address and restarts there. */
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    env.gpr[7] = 0x10000;
    vec_store32(2, 0, 0);
    vec_store32(2, 1, 0x20000);
    check(latr_execute_block(code + 2, 1), 1, "faulting indexed load translates");
    check(env.exception, 5, "indexed fault raises a load access fault");
    check(env.vstart, 1, "indexed fault names the failing lane");
    check(env.badaddr, 0x30000, "indexed fault reports the element address");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)(code + 2),
          "indexed fault reports the instruction PC");
}


static void test_rvv_masks(void)
{
    static const uint32_t logic[] = {
        0x661121d7, /* vmand.mm  v3, v1, v2 */
        0x76112257, /* vmnand.mm v4, v1, v2 */
        0x621122d7, /* vmandn.mm v5, v1, v2 */
        0x6e112357, /* vmxor.mm  v6, v1, v2 */
        0x6a1123d7, /* vmor.mm   v7, v1, v2 */
        0x7a112457, /* vmnor.mm  v8, v1, v2 */
        0x721124d7, /* vmorn.mm  v9, v1, v2 */
        0x7e112557, /* vmxnor.mm v10, v1, v2 */
    };
    static const uint32_t scans[] = {
        0x401822d7, /* vcpop.m  x5, v1, v0.t */
        0x4018a357, /* vfirst.m x6, v1, v0.t */
        0x5010a1d7, /* vmsbf.m  v3, v1, v0.t */
        0x5011a257, /* vmsif.m  v4, v1, v0.t */
        0x501122d7, /* vmsof.m  v5, v1, v0.t */
        0x50182357, /* viota.m  v6, v1, v0.t */
        0x5008a3d7, /* vid.v    v7, v0.t */
    };
    static const uint32_t masked_scans[] = {
        0x5010a1d7, /* vmsbf.m v3, v1, v0.t */
        0x50182357, /* viota.m v6, v1, v0.t */
    };

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 8;
    env.vreg[1][0] = 0x5a;
    env.vreg[2][0] = 0x3c;
    check(latr_execute_block(logic, 8), 1, "mask logic block translates");
    check(env.vreg[3][0], 0x18, "vmand.mm");
    check(env.vreg[4][0], 0xe7, "vmnand.mm");
    check(env.vreg[5][0], 0x42, "vmandn.mm");
    check(env.vreg[6][0], 0x66, "vmxor.mm");
    check(env.vreg[7][0], 0x7e, "vmor.mm");
    check(env.vreg[8][0], 0x81, "vmnor.mm");
    check(env.vreg[9][0], 0xdb, "vmorn.mm");
    check(env.vreg[10][0], 0x99, "vmxnor.mm");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 8;
    env.vreg[0][0] = 0x0f;
    env.vreg[1][0] = 0x5a;
    check(latr_execute_block(scans + 5, 1), 1, "isolated viota translates");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 8;
    env.vreg[0][0] = 0x0f;
    env.vreg[1][0] = 0x5a;
    env.vreg[6][0] = 0xaa;
    env.vreg[6][1] = 0xbb;
    env.rvv_args[0] = 5;
    env.rvv_args[1] = 6;
    env.rvv_args[2] = 1;
    env.rvv_args[3] = 0;
    latr_rvv_mask_scan(&env);
    check(env.vreg[6][0], 0, "direct viota lane 0");
    check(env.vreg[6][1], 0, "direct viota lane 1");
    check(env.vreg[6][2], 1, "direct viota lane 2");
    check(env.vreg[6][3], 1, "direct viota lane 3");
    check(env.vreg[6][0], 0, "isolated viota lane 0");
    check(env.vreg[6][1], 0, "isolated viota lane 1");
    check(env.vreg[6][2], 1, "isolated viota lane 2");
    check(env.vreg[6][3], 1, "isolated viota lane 3");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 8;
    env.vreg[0][0] = 0x0f;
    env.vreg[1][0] = 0x5a;
    check(latr_execute_block(scans, 7), 1, "mask scan block translates");
    check(env.gpr[5], 2, "vcpop counts active set bits");
    check(env.gpr[6], 1, "vfirst finds the first active set bit");
    check(env.vreg[3][0], 0x01, "vmsbf before-first");
    check(env.vreg[4][0], 0x03, "vmsif includes the first");
    check(env.vreg[5][0], 0x02, "vmsof only the first");
    check(env.vreg[6][0], 0, "viota lane 0");
    check(env.vreg[6][1], 0, "viota lane 1");
    check(env.vreg[6][2], 1, "viota lane 2");
    check(env.vreg[6][3], 1, "viota lane 3");
    check(env.vreg[7][0], 0, "vid.v lane 0");
    check(env.vreg[7][1], 1, "vid.v lane 1");
    check(env.vreg[7][2], 2, "vid.v lane 2");
    check(env.vreg[7][3], 3, "vid.v lane 3");
    check(env.vreg[3][1], 0, "vmsbf leaves the tail");
    check(env.vreg[6][1], 0, "viota leaves the tail");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00 | (1u << 7);
    env.vl = 8;
    env.vreg[0][0] = 0x0f;
    env.vreg[1][0] = 0x5a;
    check(latr_execute_block(masked_scans, 2), 1, "masked scans translate");
    check(env.vreg[3][0], 0xf1, "masked vmsbf fills vma lanes");
    check(env.vreg[6][0], 0, "masked viota lane 0");
    check(env.vreg[6][2], 1, "masked viota lane 2");
    check(env.vreg[6][3], 1, "masked viota lane 3");
    check(env.vreg[6][4], 0xff, "masked viota vma lane 4");
    check(env.vreg[6][7], 0xff, "masked viota vma lane 7");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 8;
    env.vstart = 2;
    env.vreg[0][0] = 0x0f;
    env.vreg[1][0] = 0x5a;
    check(latr_execute_block(scans, 1), 1, "vcpop vstart block translates");
    check(env.exception, 2, "vcpop requires vstart == 0");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00;
    env.vl = 8;
    env.vstart = 2;
    env.vreg[0][0] = 0x0f;
    env.vreg[1][0] = 0x5a;
    check(latr_execute_block(scans + 1, 1), 1, "vfirst vstart block translates");
    check(env.exception, 2, "vfirst requires vstart == 0");
}


static void test_rvv_reductions(void)
{
    static const uint32_t int_red[] = {
        0x021121d7, /* vredsum.vs  v3, v1, v2 */
        0x06112257, /* vredand.vs  v4, v1, v2 */
        0x0a1122d7, /* vredor.vs   v5, v1, v2 */
        0x0e112357, /* vredxor.vs  v6, v1, v2 */
        0x161123d7, /* vredmin.vs  v7, v1, v2 */
        0x1e112457, /* vredmax.vs  v8, v1, v2 */
        0x121124d7, /* vredminu.vs v9, v1, v2 */
        0x1a112557, /* vredmaxu.vs v10, v1, v2 */
    };
    static const uint32_t masked_red[] = {
        0x001121d7, /* vredsum.vs v3, v1, v2, v0.t */
    };
    static const uint32_t fp_red[] = {
        0x061111d7, /* vfredusum.vs v3, v1, v2 */
        0x0e111257, /* vfredosum.vs v4, v1, v2 */
        0x161112d7, /* vfredmin.vs  v5, v1, v2 */
        0x1e111357, /* vfredmax.vs  v6, v1, v2 */
    };
    static const uint32_t wide_red[] = {
        0xc61101d7, /* vwredsum.vs  v3, v1, v2 */
        0xc2110257, /* vwredsumu.vs v4, v1, v2 */
    };

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 2);
    vec_store32(1, 2, 3);
    vec_store32(1, 3, 4);
    vec_store32(2, 0, 10);
    check(latr_execute_block(int_red, 8), 1, "integer reductions translate");
    check(vec_read32(3, 0), 20, "vredsum");
    check(vec_read32(4, 0), 0, "vredand");
    check(vec_read32(5, 0), 15, "vredor");
    check(vec_read32(6, 0), 14, "vredxor");
    check(vec_read32(7, 0), 1, "vredmin");
    check(vec_read32(8, 0), 10, "vredmax");
    check(vec_read32(9, 0), 1, "vredminu");
    check(vec_read32(10, 0), 10, "vredmaxu");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[0][0] = 0x05;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 2);
    vec_store32(1, 2, 3);
    vec_store32(1, 3, 4);
    vec_store32(2, 0, 10);
    check(latr_execute_block(masked_red, 1), 1, "masked reduction translates");
    check(vec_read32(3, 0), 14, "masked vredsum skips inactive lanes");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[1][0] = 0x00;
    env.vreg[1][1] = 0x00;
    env.vreg[1][2] = 0x00;
    env.vreg[1][3] = 0x00;
    vec_store32(1, 0, 0x3fc00000u); /* 1.5f */
    vec_store32(1, 1, 0x40200000u); /* 2.5f */
    vec_store32(1, 2, 0x40400000u); /* 3.0f */
    vec_store32(1, 3, 0x40800000u); /* 4.0f */
    vec_store32(2, 0, 0x3f000000u); /* 0.5f */
    check(latr_execute_block(fp_red, 4), 1, "FP reductions translate");
    check(vec_read32(3, 0), 0x41380000u, "vfredusum 11.5");
    check(vec_read32(4, 0), 0x41380000u, "vfredosum 11.5");
    check(vec_read32(5, 0), 0x3f000000u, "vfredmin 0.5");
    check(vec_read32(6, 0), 0x40800000u, "vfredmax 4.0");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x08; /* SEW=16 */
    env.vl = 2;
    env.vreg[1][0] = 0x00;
    env.vreg[1][1] = 0x10; /* 0x1000 */
    env.vreg[1][2] = 0x00;
    env.vreg[1][3] = 0x20; /* 0x2000 */
    vec_store32(2, 0, 0x10000);
    check(latr_execute_block(wide_red, 2), 1, "wide reductions translate");
    check(vec_read32(3, 0), 0x13000, "vwredsum");
    check(vec_read32(4, 0), 0x13000, "vwredsumu");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10 | (1u << 6);
    env.vl = 4;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 2);
    vec_store32(1, 2, 3);
    vec_store32(1, 3, 4);
    vec_store32(2, 0, 10);
    check(latr_execute_block(int_red, 1), 1, "vta reduction translates");
    check(vec_read32(3, 0), 20, "vta reduction result");
    check(vec_read32(3, 1), 0xffffffffu, "vta reduction tail lane 1");
    check(vec_read32(3, 3), 0xffffffffu, "vta reduction tail lane 3");

    /* Reductions accumulate lanes below vstart into state the instruction
     * does not keep, so RVV 1.0 (and QEMU's vext_check_reduction) require
     * vstart == 0 here: a non-zero vstart is an illegal instruction, not a
     * resume point. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vstart = 2;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 2);
    vec_store32(1, 2, 3);
    vec_store32(1, 3, 4);
    vec_store32(2, 0, 10);
    check(latr_execute_block(int_red, 1), 1, "reduction with vstart translates");
    check(env.exception, 2, "vredsum with vstart != 0 is illegal");
    check(vec_read32(3, 0), 0, "illegal vredsum leaves the destination alone");
}


/*
 * Operand legality: QEMU raises an illegal instruction when a register group
 * would wrap past v31, when a masked destination would clobber v0, and when
 * the destination overlaps a source it may not. Before this check existed the
 * wraparound in latr_rvv_read_elem()/write_elem() silently redirected those
 * accesses to another architectural register.
 */
static void test_rvv_legality(void)
{
    static const uint32_t add_v31[] = {0x02008fd7};   /* vadd.vv v31, v0, v1 */
    static const uint32_t add_v8[] = {0x02040457};    /* vadd.vv v8, v0, v8 */
    static const uint32_t masked_v0[] = {0x00110057}; /* vadd.vv v0,v1,v2,v0.t */
    static const uint32_t masked_v1[] = {0x001100d7}; /* vadd.vv v1,v1,v2,v0.t */
    static const uint32_t load_v3[] = {0x0200e187};   /* vle32.v v3, (x1) */
    static const uint32_t store_v3[] = {0x0200e1a7};  /* vse32.v v3, (x1) */
    static const uint32_t gather_v4[] = {0x32208257}; /* vrgather.vv v4,v2,v1 */
    static const uint32_t gather_bad[] = {0x32408257};/* vrgather.vv v4,v4,v1 */
    static const uint32_t mask_logic[] = {0x6621a0d7};/* vmand.mm v1,v2,v3 */
    static const uint32_t vfadd[] = {0x022190d7};     /* vfadd.vv v1,v2,v3 */

    /* SEW=64, LMUL=8: the v31 group would span v31..v38. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x1b;
    env.vl = 2;
    env.vreg[0][0] = 0xff; /* clobbered by the old wraparound */
    env.vreg[7][0] = 0x5a;
    check(latr_execute_block(add_v31, 1), 1, "vadd.v31 translates");
    check(env.exception, 2, "vadd.vv v31 with LMUL=8 is illegal");
    check(env.vreg[0][0], 0xff, "illegal group leaves the mask register alone");
    check(env.vreg[7][0], 0x5a, "illegal group leaves v7 alone");

    /* The aligned form of the same instruction must still execute. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x1b;
    env.vl = 2;
    vec_store64(0, 0, 3);
    vec_store64(8, 0, 4);
    check(latr_execute_block(add_v8, 1), 1, "aligned LMUL=8 vadd translates");
    check(env.exception, 0, "vadd.vv v8 with LMUL=8 is legal");
    check(vec_read64(8, 0), 7, "vadd.vv v8 result");

    /* A masked destination may not overlap v0. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    check(latr_execute_block(masked_v1, 1), 1, "masked vadd into v1 translates");
    check(env.exception, 0, "masked vadd.vv into v1 is legal");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[0][0] = 0x05;
    check(latr_execute_block(masked_v0, 1), 1, "masked vadd into v0 translates");
    check(env.exception, 2, "masked vadd.vv into v0 is illegal");
    check(env.vreg[0][0], 0x05, "illegal masked vadd keeps the mask intact");

    /* EMUL for a unit-stride access is EEW - SEW + LMUL. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x12; /* SEW=32, LMUL=2 */
    env.vl = 2;
    check(latr_execute_block(load_v3, 1), 1, "misaligned vle32 translates");
    check(env.exception, 2, "vle32.v v3 with LMUL=2 is illegal");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x12;
    env.vl = 2;
    check(latr_execute_block(store_v3, 1), 1, "misaligned vse32 translates");
    check(env.exception, 2, "vse32.v v3 with LMUL=2 is illegal");

    /* Gather rejects a destination that aliases a source. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    check(latr_execute_block(gather_v4, 1), 1, "vrgather.vv translates");
    check(env.exception, 0, "vrgather.vv v4,v2,v1 is legal");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    check(latr_execute_block(gather_bad, 1), 1, "aliasing vrgather translates");
    check(env.exception, 2, "vrgather.vv v4,v4,v1 is illegal");

    /* Mask logic works on single registers, so LMUL does not constrain it. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x1b;
    env.vl = 2;
    check(latr_execute_block(mask_logic, 1), 1, "vmand.mm translates");
    check(env.exception, 0, "vmand.mm ignores LMUL alignment");

    /* A reserved frm is illegal for every vector FP instruction that rounds. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.fcsr = 5u << 5; /* frm = 5 is reserved */
    env.vtype = 0x10;
    env.vl = 2;
    check(latr_execute_block(vfadd, 1), 1, "vfadd with frm=5 translates");
    check(env.exception, 2, "vfadd.vv with a reserved frm is illegal");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.fcsr = 4u << 5; /* RMM stays valid */
    env.vtype = 0x10;
    env.vl = 2;
    check(latr_execute_block(vfadd, 1), 1, "vfadd with frm=4 translates");
    check(env.exception, 0, "vfadd.vv with RMM is legal");
}

static void test_rvv_permute(void){
    static const uint32_t code[] = {
        0x3a12c1d7, /* vslideup.vx    v3, v1, x5 */
        0x3e12c257, /* vslidedown.vx  v4, v1, x5 */
        0x3a1362d7, /* vslide1up.vx   v5, v1, x6 */
        0x3e136357, /* vslide1down.vx v6, v1, x6 */
        0x321103d7, /* vrgather.vv    v7, v1, v2 */
        0x3213c457, /* vrgather.vx    v8, v1, x7 */
        0x3211b4d7, /* vrgather.vi    v9, v1, 3 */
        0x3a110557, /* vrgatherei16.vv v10, v1, v2 */
        0x5e1025d7, /* vcompress.vm   v11, v1, v0 */
        0x3812c1d7, /* vslideup.vx    v3, v1, x5, v0.t */
    };

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 10);
    vec_store32(1, 1, 20);
    vec_store32(1, 2, 30);
    vec_store32(1, 3, 40);
    vec_store32(2, 0, 3);
    vec_store32(2, 1, 2);
    vec_store32(2, 2, 1);
    vec_store32(2, 3, 0);
    env.gpr[5] = 1;
    env.gpr[6] = 0x99;
    env.gpr[7] = 2;
    check(latr_execute_block(code, 7), 1, "slide/gather block translates");
    check(vec_read32(3, 0), 0, "vslideup keeps lane 0");
    check(vec_read32(3, 1), 10, "vslideup lane 1");
    check(vec_read32(3, 3), 30, "vslideup lane 3");
    check(vec_read32(4, 0), 20, "vslidedown lane 0");
    check(vec_read32(4, 2), 40, "vslidedown lane 2");
    check(vec_read32(4, 3), 0, "vslidedown fills zero");
    check(vec_read32(5, 0), 0x99, "vslide1up inserts the scalar");
    check(vec_read32(5, 1), 10, "vslide1up shifts the source");
    check(vec_read32(6, 3), 0x99, "vslide1down inserts at the end");
    check(vec_read32(7, 0), 40, "vrgather.vv first index");
    check(vec_read32(7, 3), 10, "vrgather.vv last index");
    check(vec_read32(8, 0), 30, "vrgather.vx broadcasts");
    check(vec_read32(8, 3), 30, "vrgather.vx lane 3");
    check(vec_read32(9, 2), 40, "vrgather.vi index 3");
    env.vreg[2][0] = 3;
    env.vreg[2][1] = 0;
    env.vreg[2][2] = 2;
    env.vreg[2][3] = 0;
    env.vreg[2][4] = 1;
    env.vreg[2][5] = 0;
    env.vreg[2][6] = 0;
    env.vreg[2][7] = 0;
    check(latr_execute_block(code + 7, 1), 1, "vrgatherei16 translates");
    check(vec_read32(10, 0), 40, "vrgatherei16 lane 0");
    check(vec_read32(10, 1), 30, "vrgatherei16 lane 1");

    env.vreg[0][0] = 0x0a;
    vec_store32(11, 0, 0xdeadbeefu);
    vec_store32(11, 1, 0xdeadbeefu);
    vec_store32(11, 2, 0xdeadbeefu);
    vec_store32(11, 3, 0xdeadbeefu);
    check(latr_execute_block(code + 8, 1), 1, "vcompress translates");
    check(vec_read32(11, 0), 20, "vcompress first active element");
    check(vec_read32(11, 1), 40, "vcompress second active element");
    check(vec_read32(11, 2), 0xdeadbeefu, "vcompress leaves a spare lane");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 10);
    vec_store32(1, 1, 20);
    vec_store32(1, 2, 30);
    vec_store32(1, 3, 40);
    env.gpr[5] = 1;
    env.vreg[0][0] = 0x05;
    rvv_fill_sentinel(3);
    check(latr_execute_block(code + 9, 1), 1, "masked slideup translates");
    check(vec_read32(3, 0), 0xdeadbeefu, "masked slideup lane 0 untouched");
    check(vec_read32(3, 2), 20, "masked slideup lane 2");
    check(vec_read32(3, 1), 0xdeadbeefu, "masked slideup lane 1 untouched");
}


static void test_rvv_width_sat(void)
{
    /* Widening destinations have EMUL = 2*LMUL, so with LMUL=1 the register
     * number must be even (RVV 1.0 section 3.4.2). */
    static const uint32_t wide[] = {
        0xc6112257, /* vwadd.vv   v4, v1, v2 */
        0xc212e357, /* vwaddu.vx  v6, v1, x5 */
        0xce112457, /* vwsub.vv   v8, v1, v2 */
        0xca12e557, /* vwsubu.vx  v10, v1, x5 */
        0xee112657, /* vwmul.vv   v12, v1, v2 */
        0xe212e757, /* vwmulu.vx  v14, v1, x5 */
        0xea112857, /* vwmulsu.vv v16, v1, v2 */
    };
    static const uint32_t macc[] = {
        0xc61121d7, /* placeholder replaced below */
    };
    /* Narrowing sources have EMUL = 2*LMUL and the destination must stay clear
     * of the whole source group. */
    static const uint32_t narrow[] = {
        0xb220b257, /* vnsrl.wi  v4, v2, 1 */
        0xb620b357, /* vnsra.wi  v6, v2, 1 */
        0xbe20b457, /* vnclip.wi v8, v2, 1 */
        0xba20b557, /* vnclipu.wi v10, v2, 1 */
    };
    static const uint32_t sat[] = {
        0x821103d7, /* vsaddu.vv v7, v1, v2 */
        0x86110457, /* vsadd.vv  v8, v1, v2 */
        0x8a1104d7, /* vssubu.vv v9, v1, v2 */
        0x8e110557, /* vssub.vv  v10, v1, v2 */
        0x221125d7, /* vaaddu.vv v11, v1, v2 */
        0x26112657, /* vaadd.vv  v12, v1, v2 */
        0x2a1126d7, /* vasubu.vv v13, v1, v2 */
        0x2e112757, /* vasub.vv  v14, v1, v2 */
        0xaa10b7d7, /* vssrl.vi  v15, v1, 1 */
        0xae10b857, /* vssra.vi  v16, v1, 1 */
        0x021fb8d7, /* vadd.vi   v17, v1, -1 */
    };

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 2);
    vec_store32(2, 0, 3);
    vec_store32(2, 1, 4);
    env.gpr[5] = 10;
    check(latr_execute_block(wide, 7), 1, "widening block translates");
    check(vec_read64(4, 0), 4, "vwadd lane 0");
    check(vec_read64(4, 1), 6, "vwadd lane 1");
    check(vec_read64(6, 0), 11, "vwaddu.vx lane 0");
    check(vec_read64(6, 1), 12, "vwaddu.vx lane 1");
    check(vec_read64(8, 0), (uint64_t)-2, "vwsub lane 0");
    check(vec_read64(10, 0), (uint64_t)-9, "vwsubu.vx wraps");
    check(vec_read64(12, 0), 3, "vwmul lane 0");
    check(vec_read64(12, 1), 8, "vwmul lane 1");
    check(vec_read64(14, 1), 20, "vwmulu.vx lane 1");
    check(vec_read64(16, 1), 8, "vwmulsu.vv lane 1");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store64(2, 0, 6);
    vec_store64(2, 1, 8);
    check(latr_execute_block(narrow, 1), 1, "vnsrl translates");
    check(vec_read32(4, 0), 3, "vnsrl lane 0");
    check(vec_read32(4, 1), 4, "vnsrl lane 1");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store64(2, 0, (uint64_t)-8);
    vec_store64(2, 1, 8);
    check(latr_execute_block(narrow + 1, 1), 1, "vnsra translates");
    check(vec_read32(6, 0), 0xfffffffcu, "vnsra lane 0");
    check(vec_read32(6, 1), 4, "vnsra lane 1");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store64(2, 0, UINT64_C(0x100000000));
    vec_store64(2, 1, UINT64_MAX);
    check(latr_execute_block(narrow + 2, 2), 1, "clip translates");
    check(vec_read32(8, 0), 0x7fffffffu, "vnclip saturates lane 0");
    check(vec_read32(8, 1), 0xffffffffu, "vnclip keeps -1 lane 1");
    check(vec_read32(10, 0), 0x80000000u, "vnclipu keeps 2^31 lane 0");
    check(vec_read32(10, 1), 0xffffffffu, "vnclipu saturates lane 1");
    check(env.vxsat, 1, "clip sets vxsat");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 0xffffffffu);
    vec_store32(1, 1, 0x7fffffffu);
    vec_store32(2, 0, 1);
    vec_store32(2, 1, 1);
    check(latr_execute_block(sat, 4), 1, "saturating add/sub block translates");
    check(vec_read32(7, 0), 0xffffffffu, "vsaddu lane 0");
    check(vec_read32(7, 1), 0x80000000u, "vsaddu lane 1");
    check(vec_read32(8, 0), 0, "vsadd lane 0 wraps -1+1");
    check(vec_read32(8, 1), 0x7fffffffu, "vsadd saturates lane 1");
    check(vec_read32(9, 0), 0xfffffffeu, "vssubu lane 0");
    check(vec_read32(10, 0), 0xfffffffeu, "vssub lane 0");
    check(env.vxsat, 1, "saturating add sets vxsat");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 3);
    vec_store32(2, 0, 2);
    vec_store32(2, 1, 3);
    check(latr_execute_block(sat + 4, 4), 1, "averaging block translates");
    check(vec_read32(11, 0), 2, "vaaddu rounds 1.5 up");
    check(vec_read32(11, 1), 3, "vaaddu rounds 3 exactly");
    check(vec_read32(12, 0), 2, "vaadd rounds 1.5 up");
    check(vec_read32(13, 0), 0, "vasubu saturates below zero");
    check(env.vxsat, 1, "vasubu sets vxsat on underflow");
    check(vec_read32(14, 0), 0xffffffffu, "vasub rounds -0.5 away from zero");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 3);
    vec_store32(1, 1, 1);
    check(latr_execute_block(sat + 8, 1), 1, "vssrl block translates");
    check(vec_read32(15, 0), 2, "vssrl rounds 3/2 up");
    check(vec_read32(15, 1), 1, "vssrl rounds 1/2 up to 1");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 0xfffffffdu); /* -3 */
    vec_store32(1, 1, 0xfffffffeu); /* -2 */
    check(latr_execute_block(sat + 9, 2), 1, "vssra/vadd.vi block translates");
    check(vec_read32(16, 0), 0xfffffffeu, "vssra rounds -3/2 away from zero");
    check(vec_read32(16, 1), 0xffffffffu, "vssra rounds -2/2 exactly");
    check(vec_read32(17, 0), 0xfffffffcu, "vadd.vi sign-extends -1");
    check(vec_read32(17, 1), 0xfffffffdu, "vadd.vi lane 1");
}


static void test_rvv_fp(void)
{
    {
        float_status st = { 0 };
        uint64_t lo, hi;
        set_float_rounding_mode(float_round_nearest_even, &st);
        set_default_nan_mode(true, &st);
        set_snan_bit_is_one(false, &st);
        mulu64(&lo, &hi, UINT64_C(0x123456789abcdef0),
               UINT64_C(0xfedcba9876543210));
        check(lo, UINT64_C(0x236d88fe5618cf00), "mulu64 low half");
        check(hi, UINT64_C(0x121fa00ad77d7422), "mulu64 high half");
        check((uint32_t)float32_mul(0x3fc00000u, 0x3f000000u, &st),
              0x3f400000u, "softfloat 1.5*0.5");
        check((uint32_t)float32_mul(0x40000000u, 0x40400000u, &st),
              0x40c00000u, "softfloat 2*3");
        check((uint32_t)float32_add(0x3fc00000u, 0x3f800000u, &st),
              0x40200000u, "softfloat 1.5+1");
    }
    static const uint32_t code[] = {
        0x021111d7, /* vfadd.vv     v3, v1, v2 */
        0x02105257, /* vfadd.vf     v4, v1, f0 */
        0x0a1112d7, /* vfsub.vv     v5, v1, v2 */
        0x92111357, /* vfmul.vv     v6, v1, v2 */
        0x821113d7, /* vfdiv.vv     v7, v1, v2 */
        0x12111457, /* vfmin.vv     v8, v1, v2 */
        0x1a1114d7, /* vfmax.vv     v9, v1, v2 */
        0x4e101557, /* vfsqrt.v      v10, v1 */
        0x221115d7, /* vfsgnj.vv    v11, v1, v2 */
        0x26111657, /* vfsgnjn.vv   v12, v1, v2 */
        0x2a1116d7, /* vfsgnjx.vv   v13, v1, v2 */
        0x5c105757, /* vfmerge.vfm  v14, v1, f0, v0 */
        0x5e0057d7, /* vfmv.v.f     v15, f0 */
        0x421010d7, /* vfmv.f.s     f1, v1 */
        0x42005857, /* vfmv.s.f     v16, f0 */
        0x621118d7, /* vmfeq.vv     v17, v1, v2 */
        0x72111957, /* vmfne.vv     v18, v1, v2 */
        0x6e1119d7, /* vmflt.vv     v19, v1, v2 */
        0x66111a57, /* vmfle.vv     v20, v1, v2 */
        0x76105ad7, /* vmfgt.vf     v21, v1, f0 */
        0x7e105b57, /* vmfge.vf     v22, v1, f0 */
        0x4e181bd7, /* vfclass.v    v23, v1 */
        0x4a101c57, /* vfcvt.xu.f.v v24, v1 */
        0x4a109cd7, /* vfcvt.x.f.v  v25, v1 */
        0x4a111d57, /* vfcvt.f.xu.v v26, v1 */
        0x4a119dd7, /* vfcvt.f.x.v  v27, v1 */
        0x4a161e57, /* vfwcvt.f.f.v v28, v1 */
        0x4a2a1ed7, /* vfncvt.f.f.w v29, v2 */
        0x4a2a9f57, /* vfncvt.rod.f.f.w v30, v2 */
    };
    const uint32_t f15 = 0x3fc00000u;
    const uint32_t fn2 = 0xc0000000u;
    const uint32_t f05 = 0x3f000000u;
    const uint32_t f20 = 0x40000000u;
    const uint32_t fn1 = 0xbf800000u;
    const uint32_t f40 = 0x40800000u;
    const uint32_t finf = 0x7f800000u;
    const uint32_t f25 = 0x40200000u;

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[0][0] = 0x05;
    vec_store32(1, 0, f15);
    vec_store32(1, 1, fn2);
    vec_store32(1, 2, 0);
    vec_store32(1, 3, finf);
    vec_store32(2, 0, f05);
    vec_store32(2, 1, f20);
    vec_store32(2, 2, fn1);
    vec_store32(2, 3, f40);
    env.fpr[0] = UINT64_C(0xffffffff00000000) | f25;
    check(latr_execute_block(code + 3, 1), 1, "isolated vfmul translates");
    check(vec_read32(6, 0), 0x3f400000u, "isolated vfmul lane 0");
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.vreg[0][0] = 0x05;
    vec_store32(1, 0, f15);
    vec_store32(1, 1, fn2);
    vec_store32(1, 2, 0);
    vec_store32(1, 3, finf);
    vec_store32(2, 0, f05);
    vec_store32(2, 1, f20);
    vec_store32(2, 2, fn1);
    vec_store32(2, 3, f40);
    env.fpr[0] = UINT64_C(0xffffffff00000000) | f25;
    check(latr_execute_block(code, 12), 1, "vector FP arithmetic translates");
    check(vec_read32(3, 0), 0x40000000u, "vfadd lane 0");
    check(vec_read32(3, 3), finf, "vfadd lane 3");
    check(vec_read32(4, 0), 0x40800000u, "vfadd.vf lane 0");
    check(vec_read32(5, 1), 0xc0800000u, "vfsub lane 1");
    check(vec_read32(6, 0), 0x3f400000u, "vfmul lane 0");
    check(vec_read32(7, 0), 0x40400000u, "vfdiv lane 0");
    check(vec_read32(8, 0), f05, "vfmin lane 0");
    check(vec_read32(9, 1), f20, "vfmax lane 1");
    check(vec_read32(10, 0), 0x3f9cc471u, "vfsqrt lane 0");
    check(vec_read32(10, 1), 0x7fc00000u, "vfsqrt negative gives NaN");
    check(vec_read32(11, 1), f20, "vfsgnj lane 1");
    check(vec_read32(12, 0), 0xbfc00000u, "vfsgnjn lane 0");
    check(vec_read32(13, 1), fn2, "vfsgnjx lane 1");
    check(vec_read32(14, 0), f15, "vfmerge lane 0 selects vs2");
    check(vec_read32(14, 1), f25, "vfmerge lane 1 selects the scalar");
    check(vec_read32(14, 2), 0, "vfmerge lane 2 selects vs2");
    check(vec_read32(14, 3), f25, "vfmerge lane 3 selects the scalar");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, f15);
    vec_store32(1, 1, fn2);
    env.fpr[0] = UINT64_C(0xffffffff00000000) | f25;
    check(latr_execute_block(code + 12, 1), 1, "isolated vfmv.v.f translates");
    check(vec_read32(15, 0), f25, "isolated vfmv.v.f lane 0");
    check(latr_execute_block(code + 12, 3), 1, "FP moves translate");
    check(vec_read32(15, 0), f25, "vfmv.v.f lane 0");
    check(vec_read32(15, 3), f25, "vfmv.v.f lane 3");
    check(env.fpr[1], UINT64_C(0xffffffff3fc00000), "vfmv.f.s NaN-boxes");
    check(vec_read32(16, 0), f25, "vfmv.s.f writes lane 0");
    check(vec_read32(16, 1), 0, "vfmv.s.f leaves the tail");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, f15);
    vec_store32(1, 1, fn2);
    vec_store32(1, 2, 0);
    vec_store32(1, 3, finf);
    vec_store32(2, 0, f05);
    vec_store32(2, 1, f20);
    vec_store32(2, 2, fn1);
    vec_store32(2, 3, f40);
    env.fpr[0] = UINT64_C(0xffffffff00000000) | f25;
    check(latr_execute_block(code + 16, 1), 1, "isolated vmfne translates");
    check(env.pvir_status, 0, "isolated vmfne PVIR status");
    check(env.vreg[18][0], 0x0f, "isolated vmfne bits");
    check(latr_execute_block(code + 15, 7), 1, "FP compares/class translate");
    check(env.vreg[17][0], 0x00, "vmfeq packed bits");
    check(env.vreg[18][0], 0x0f, "vmfne packed bits");
    check(env.vreg[19][0], 0x02, "vmflt packed bits");
    check(env.vreg[20][0], 0x02, "vmfle packed bits");
    check(env.vreg[21][0], 0x08, "vmfgt packed bits");
    check(env.vreg[22][0], 0x08, "vmfge packed bits");
    check(vec_read32(23, 0), 0x40, "vfclass positive normal");
    check(vec_read32(23, 1), 0x02, "vfclass negative normal");
    check(vec_read32(23, 2), 0x10, "vfclass +0");
    check(vec_read32(23, 3), 0x80, "vfclass +inf");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, f15);
    vec_store32(1, 1, fn2);
    vec_store32(1, 2, 0);
    vec_store32(1, 3, finf);
    check(latr_execute_block(code + 22, 2), 1, "FP-to-int conversions translate");
    check(vec_read32(24, 0), 2, "vfcvt.xu.f lane 0 rounds to even");
    check(vec_read32(24, 1), 0, "vfcvt.xu.f negative clamps");
    check(vec_read32(24, 3), 0xffffffffu, "vfcvt.xu.f inf saturates");
    check(vec_read32(25, 0), 2, "vfcvt.x.f lane 0 rounds to even");
    check(vec_read32(25, 1), 0xfffffffeu, "vfcvt.x.f lane 1");
    check(vec_read32(25, 3), 0x7fffffffu, "vfcvt.x.f inf saturates");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 1);
    vec_store32(1, 1, 2);
    vec_store32(1, 2, 0xffffffffu);
    vec_store32(1, 3, 3);
    check(latr_execute_block(code + 24, 2), 1, "int-to-FP conversions translate");
    check(vec_read32(26, 0), 0x3f800000u, "vfcvt.f.xu lane 0");
    check(vec_read32(26, 2), 0x4f800000u, "vfcvt.f.xu lane 2");
    check(vec_read32(27, 0), 0x3f800000u, "vfcvt.f.x lane 0");
    check(vec_read32(27, 1), 0x40000000u, "vfcvt.f.x lane 1");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, f15);
    vec_store32(1, 1, fn2);
    vec_store32(1, 2, 0);
    vec_store32(1, 3, finf);
    check(latr_execute_block(code + 26, 1), 1, "vfwcvt.f.f translates");
    check(vec_read64(28, 0), UINT64_C(0x3ff8000000000000), "vfwcvt 1.5");
    check(vec_read64(28, 1), UINT64_C(0xc000000000000000), "vfwcvt -2.0");
    check(vec_read64(28, 3), UINT64_C(0x7ff0000000000000), "vfwcvt inf");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store64(2, 0, UINT64_C(0x3ff0000008000000)); /* 1 + 2^-25 */
    vec_store64(2, 1, UINT64_C(0xc000000000000000)); /* -2.0 */
    vec_store64(2, 2, 0);
    vec_store64(2, 3, UINT64_C(0x7ff0000000000000)); /* inf */
    check(latr_execute_block(code + 28, 1), 1, "vfncvt.rod translates");
    check(vec_read32(30, 0), 0x3f800001u, "vfncvt.rod rounds to odd");
    check(vec_read32(30, 1), fn2, "vfncvt.rod -2.0");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store64(2, 0, UINT64_C(0x3ff8000000000000));
    vec_store64(2, 1, UINT64_C(0xc000000000000000));
    check(latr_execute_block(code + 27, 1), 1, "vfncvt.f.f translates");
    check(vec_read32(29, 0), f15, "vfncvt.f.f 1.5");
    check(vec_read32(29, 1), fn2, "vfncvt.f.f -2.0");
}


static void test_rvv_moves_wide_fp(void)
{
    static const uint32_t moves[] = {
        0x421022d7, /* vmv.x.s x5, v1 */
        0x420361d7, /* vmv.s.x v3, x6 */
    };
    static const uint32_t wide_fp[] = {
        0xc2111257, /* vfwadd.vv v4, v1, v2 */
        0xca111357, /* vfwsub.vv v6, v1, v2 */
        0xe2111457, /* vfwmul.vv v8, v1, v2 */
    };
    static const uint32_t wide_src_fp[] = {
        0xd2111257, /* vfwadd.wv v4, v1, v2 */
        0xda111357, /* vfwsub.wv v6, v1, v2 */
        0xd2105457, /* vfwadd.wf v8, v1, f0 */
        0xda105557, /* vfwsub.wf v10, v1, f0 */
    };

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 0xfffffffeu);
    env.gpr[6] = 0x1234;
    check(latr_execute_block(moves, 2), 1, "integer scalar moves translate");
    check(env.gpr[5], UINT64_C(0xfffffffffffffffe), "vmv.x.s sign-extends");
    check(vec_read32(3, 0), 0x1234, "vmv.s.x writes element 0");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 0x3fc00000u);
    vec_store32(1, 1, 0x40200000u);
    vec_store32(2, 0, 0x3f000000u);
    vec_store32(2, 1, 0x3e800000u);
    check(latr_execute_block(wide_fp, 3), 1, "FP widening block translates");
    check(vec_read64(4, 0), UINT64_C(0x4000000000000000), "vfwadd 2.0");
    check(vec_read64(4, 1), UINT64_C(0x4006000000000000), "vfwadd 2.75");
    check(vec_read64(6, 0), UINT64_C(0x3ff0000000000000), "vfwsub 1.0");
    check(vec_read64(6, 1), UINT64_C(0x4002000000000000), "vfwsub 2.25");
    check(vec_read64(8, 0), UINT64_C(0x3fe8000000000000), "vfwmul 0.75");
    check(vec_read64(8, 1), UINT64_C(0x3fe4000000000000), "vfwmul 0.625");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 0x3fc00000u);
    vec_store32(1, 1, 0x40200000u);
    vec_store64(2, 0, UINT64_C(0x3fe0000000000000));
    vec_store64(2, 1, UINT64_C(0x3fd0000000000000));
    env.fpr[0] = UINT64_C(0xffffffff3f000000);
    check(latr_execute_block(wide_src_fp, 4), 1, "FP wide-source block translates");
    check(vec_read64(4, 0), UINT64_C(0x4000000000000000), "vfwadd.wv 2.0");
    check(vec_read64(4, 1), UINT64_C(0x4006000000000000), "vfwadd.wv 2.75");
    check(vec_read64(6, 0), UINT64_C(0x3ff0000000000000), "vfwsub.wv 1.0");
    check(vec_read64(8, 0), UINT64_C(0x4000000000000000), "vfwadd.wf 2.0");
    check(vec_read64(10, 0), UINT64_C(0x3ff0000000000000), "vfwsub.wf 1.0");
    check(vec_read64(10, 1), UINT64_C(0x4000000000000000), "vfwsub.wf 2.0");
}

static void test_rvv_pvir_lsx_lasx(void)
{
    static const uint32_t rvv_diff_seq[] = {
        LATR_DIFF_RVV_VMV_V_I_V1_7,
        LATR_DIFF_RVV_VMV_V_X_V2_T0,
        LATR_DIFF_RVV_VMUL_VV_V3,
        LATR_DIFF_RVV_VADD_VX_V3_T1,
        LATR_DIFF_RVV_VMAX_VV_V3,
        LATR_DIFF_RVV_VSLL_VI_V4_2,
        LATR_DIFF_RVV_VDIV_VV_V5,
        LATR_DIFF_RVV_VREM_VV_V6,
        LATR_DIFF_RVV_VXOR_VV_V3,
        LATR_DIFF_RVV_VMV_V_I_V7_0,
        LATR_DIFF_RVV_VREDSUM_V7,
        LATR_DIFF_RVV_VMV_X_S_T0_V7,
    };
    static const uint32_t new_alu_ops[] = {
        0x961121d7, /* vmul.vv  v3, v1, v2 */
        0x9612e257, /* vmul.vx  v4, v1, t0 (reg=5) -> check rd */
        0x821122d7, /* vdivu.vv v5, v1, v2 */
        0x86112357, /* vdiv.vv  v6, v1, v2 */
        0x8a1123d7, /* vremu.vv v7, v1, v2 */
        0x8e112457, /* vrem.vv  v8, v1, v2 */
        0x121104d7, /* vminu.vv v9, v1, v2 */
        0x16110557, /* vmin.vv  v10, v1, v2 */
        0x1a1105d7, /* vmaxu.vv v11, v1, v2 */
        0x1e110657, /* vmax.vv  v12, v1, v2 */
    };
    static const uint32_t new_cmp_ops[] = {
        0x7a12c057, /* vmsgtu.vx v0, v1, t0 */
        0x7a12b0d7, /* vmsgtu.vi v1, v1, 5 (overwrite v1? check v3) */
        0x7e1fb157, /* vmsgt.vi  v2, v1, -1 */
    };
    static const uint32_t shift_imm_high[] = {
        0x9618b1d7, /* vsll.vi v3, v1, 17 */
        0xa218b257, /* vsrl.vi v4, v1, 17 */
        0xa618b2d7, /* vsra.vi v5, v1, 17 */
    };
    static const uint32_t ext_ops[] = {
        0x4a1321d7, /* vzext.vf2 v3, v1 */
        0x4a122257, /* vzext.vf4 v4, v1 */
        0x4a1122d7, /* vzext.vf8 v5, v1 */
        0x4a13a357, /* vsext.vf2 v6, v1 */
        0x4a12a3d7, /* vsext.vf4 v7, v1 */
        0x4a11a457, /* vsext.vf8 v8, v1 */
    };
    static const uint32_t mask_mem_ops[] = {
        0x02b30427, /* vsm.v v8, (t1) */
        0x02b28407, /* vlm.v v8, (t0) */
    };
    static uint8_t mask_buf[4];

    /* 1. RVV differential vector parity check against QEMU -cpu max. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0xd0; /* e32, m1, ta, ma */
    env.vl = 4;
    env.gpr[5] = 3;
    env.gpr[6] = (uint64_t)(int64_t)-5;
    check(latr_execute_block(rvv_diff_seq, 12), 1,
          "RVV differential sequence translates");
    check(env.gpr[5], 80, "RVV diff sequence computes 80");
    check(env.pvir_status, 0, "RVV diff sequence PVIR validates");
    diff_rvv = env.gpr[5];

    /* 2. New opcodes: vmul, vdiv(u), vrem(u), vmin(u), vmax(u), edge cases. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10; /* SEW=32, LMUL=1, tu, mu */
    env.vl = 4;
    env.gpr[5] = 3;
    vec_store32(1, 0, 0x80000000u); /* INT32_MIN */
    vec_store32(1, 1, 7u);
    vec_store32(1, 2, (uint32_t)-15);
    vec_store32(1, 3, 20u);
    vec_store32(2, 0, (uint32_t)-1);
    vec_store32(2, 1, 0u);          /* div/rem by zero */
    vec_store32(2, 2, 4u);
    vec_store32(2, 3, (uint32_t)-6);
    check(latr_execute_block(new_alu_ops, 10), 1, "new RVV ALU ops translate");
    check(env.pvir_status, 0, "new RVV ALU ops PVIR validates");
    check(vec_read32(3, 0), 0x80000000u, "vmul.vv INT32_MIN * -1");
    check(vec_read32(3, 2), (uint32_t)(-15 * 4), "vmul.vv -15 * 4");
    check(vec_read32(5, 1), 0xffffffffu, "vdivu.vv by zero -> all 1s");
    check(vec_read32(6, 0), 0x80000000u, "vdiv.vv INT32_MIN / -1 wraps safely");
    check(vec_read32(6, 1), 0xffffffffu, "vdiv.vv by zero -> -1");
    check(vec_read32(7, 1), 7u, "vremu.vv by zero -> dividend");
    check(vec_read32(8, 0), 0u, "vrem.vv INT32_MIN % -1 -> 0");
    check(vec_read32(8, 2), (uint32_t)(-3), "vrem.vv -15 % 4 -> -3");
    check(vec_read32(9, 0), 0x80000000u, "vminu.vv treats 0x80000000 < 0xffffffff");
    check(vec_read32(10, 0), 0x80000000u, "vmin.vv signed INT32_MIN");
    check(vec_read32(11, 1), 7u, "vmaxu.vv(7, 0) -> 7");
    check(vec_read32(12, 0), 0xffffffffu, "vmax.vv(-2^31, -1) -> -1");

    /* 3. vmsgt / vmsgtu + unsigned shift immediate >= 16. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 4;
    env.gpr[5] = 4;
    vec_store32(1, 0, 0xffffffffu);
    vec_store32(1, 1, 2u);
    vec_store32(1, 2, 10u);
    vec_store32(1, 3, (uint32_t)-5);
    /* test vmsgtu.vx v0, v1, t0 (=0b1101 -> bits 0,2,3) and vmsgt.vi v2, v1, -1 */
    static const uint32_t cmp_gt[] = {
        0x7a12c057, /* vmsgtu.vx v0, v1, t0 */
        0x7a12b157, /* vmsgtu.vi v2, v1, 5 */
        0x7e1fb1d7, /* vmsgt.vi  v3, v1, -1 */
    };
    (void)new_cmp_ops;
    check(latr_execute_block(cmp_gt, 3), 1, "vmsgt/vmsgtu ops translate");
    check(env.vreg[0][0] & 0x0f, 0x0d, "vmsgtu.vx lane mask");
    check(env.vreg[2][0] & 0x0f, 0x0d, "vmsgtu.vi lane mask");
    check(env.vreg[3][0] & 0x0f, 0x06, "vmsgt.vi vs -1 signed");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 2;
    vec_store32(1, 0, 1u);
    vec_store32(1, 1, 0x80000000u);
    check(latr_execute_block(shift_imm_high, 3), 1, "uimm5=17 shifts translate");
    check(vec_read32(3, 0), 0x00020000u, "vsll.vi imm=17 unsigned");
    check(vec_read32(4, 1), 0x00004000u, "vsrl.vi imm=17 unsigned");
    check(vec_read32(5, 1), 0xffffc000u, "vsra.vi imm=17 unsigned shift");

    /* 4. vzext.vf* / vsext.vf* at SEW=64. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x18; /* SEW=64, LMUL=1 */
    env.vl = 2;
    vec_store32(1, 0, 0x8001ff80u); /* low 8b=0x80, 16b=0xff80, 32b=0x8001ff80 */
    vec_store32(1, 1, 0x7f000001u); /* vf2 lane 1 = 0x7f000001 */
    check(latr_execute_block(ext_ops, 6), 1, "vzext/vsext ops translate");
    check(env.pvir_status, 0, "vzext/vsext PVIR validates");
    check(vec_read64(3, 0), UINT64_C(0x000000008001ff80), "vzext.vf2");
    check(vec_read64(3, 1), UINT64_C(0x000000007f000001), "vzext.vf2 lane 1");
    check(vec_read64(4, 0), UINT64_C(0x000000000000ff80), "vzext.vf4");
    check(vec_read64(5, 0), UINT64_C(0x0000000000000080), "vzext.vf8");
    check(vec_read64(6, 0), UINT64_C(0xffffffff8001ff80), "vsext.vf2");
    check(vec_read64(7, 0), UINT64_C(0xffffffffffffff80), "vsext.vf4");
    check(vec_read64(8, 0), UINT64_C(0xffffffffffffff80), "vsext.vf8");

    /* 5. vlm.v / vsm.v mask memory loads/stores + PVIR check. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x00; /* SEW=8 */
    env.vl = 12;      /* 2 mask bytes */
    env.vreg[8][0] = 0xa5;
    env.vreg[8][1] = 0x0f;
    mask_buf[0] = mask_buf[1] = 0;
    env.gpr[6] = (uint64_t)(uintptr_t)&mask_buf[0]; /* t1 for vsm.v */
    env.gpr[5] = (uint64_t)(uintptr_t)&mask_buf[2]; /* t0 for vlm.v */
    mask_buf[2] = 0x5a;
    mask_buf[3] = 0x0c;
    check(latr_execute_block(mask_mem_ops, 2), 1, "vlm.v/vsm.v translate");
    check(env.pvir_status, 0, "vlm.v/vsm.v PVIR validates");
    check(mask_buf[0], 0xa5, "vsm.v byte 0");
    check(mask_buf[1], 0x0f, "vsm.v byte 1");
    check(env.vreg[8][0], 0x5a, "vlm.v byte 0");
    check(env.vreg[8][1], 0x0c, "vlm.v byte 1");

    /* 6. vmv.x.s x0 protection + vmv.s.x/vfmv.s.f vl==0 and vta=1 guards. */
    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x50; /* SEW=32, LMUL=1, vta=1 */
    env.vl = 4;
    vec_store32(1, 0, 0x12345678u);
    vec_store32(3, 0, 0x11111111u);
    vec_store32(3, 1, 0x22222222u);
    env.gpr[6] = 0xabcdef01u;
    env.fpr[0] = UINT64_C(0xffffffff3fc00000);
    static const uint32_t scalar_edge[] = {
        0x42102057, /* vmv.x.s  x0, v1 */
        0x420361d7, /* vmv.s.x  v3, t1 */
        0x42005257, /* vfmv.s.f v4, ft0 */
    };
    check(latr_execute_block(scalar_edge, 3), 1, "vmv scalar edge block");
    check(env.gpr[0], 0, "vmv.x.s respects x0 hardwired zero");
    check(vec_read32(3, 0), 0xabcdef01u, "vmv.s.x writes lane 0");
    check(vec_read32(3, 1), 0xffffffffu, "vmv.s.x vta=1 sets tail to all 1s");
    check(vec_read32(4, 0), 0x3fc00000u, "vfmv.s.f writes lane 0");
    check(vec_read32(4, 1), 0xffffffffu, "vfmv.s.f vta=1 sets tail to all 1s");

    reset_guest_state();
    env.csr[0x300] = 0x600;
    env.vtype = 0x10;
    env.vl = 0;
    vec_store32(3, 0, 0xdeadbeefu);
    vec_store32(4, 0, 0xcafebabeu);
    env.gpr[6] = 0x55555555u;
    check(latr_execute_block(scalar_edge + 1, 2), 1, "vl==0 scalar move block");
    check(vec_read32(3, 0), 0xdeadbeefu, "vmv.s.x vl==0 preserves lane 0");
    check(vec_read32(4, 0), 0xcafebabeu, "vfmv.s.f vl==0 preserves lane 0");

    /* 7. Per-TB PVIR descriptor cache hits on cached TB re-execution. */
    {
        static const uint32_t div_loop = 0x861121d7; /* vdiv.vv v3, v1, v2 */
        reset_guest_state();
        env.csr[0x300] = 0x600;
        env.vtype = 0x10;
        env.vl = 4;
        vec_store32(1, 0, 20u); vec_store32(2, 0, 4u);
        vec_store32(1, 1, 30u); vec_store32(2, 1, 5u);
        check(latr_execute_block(&div_loop, 1), 1, "cached helper TB translates");
        check(env.pvir_cache_hits, 0, "first PVIR execution misses cache");
        for (int rep = 0; rep < 3; rep++) {
            env.pc = (uint64_t)(uintptr_t)&div_loop;
            latr_enter_tb(&env, code_buf);
        }
        check(env.pvir_cache_hits, 3, "re-entering TB hits PVIR cache");
        check(vec_read32(3, 0), 5u, "cached vdiv.vv lane 0");
        check(vec_read32(3, 1), 6u, "cached vdiv.vv lane 1");
    }

    /* 8. Adaptive LSX / LASX / mixed LASX+LSX lowering and feature gating. */
    {
        static const uint32_t lmul1_add = 0x021101d7; /* vadd.vv v3, v1, v2 */
        static const uint32_t lmul2_add = 0x02430157; /* vadd.vv v2, v4, v6 */
        static const uint32_t lmul4_add = 0x02860257; /* vadd.vv v4, v8, v12 */
        static const uint32_t lmul1_fp  = 0x02221357; /* vfadd.vv v6, v2, v4 */
        static const uint32_t mask_and  = 0x661121d7; /* vmand.mm v3, v1, v2 */

        /* 8a: LMUL=1 <=16B uses Tier 1 LSX even on LASX host. */
        reset_guest_state();
        env.csr[0x300] = 0x600;
        env.vtype = 0x10; /* e32, m1 */
        env.vl = 4;
        vec_store32(1, 0, 10u); vec_store32(2, 0, 20u);
        vec_store32(1, 3, 40u); vec_store32(2, 3, 50u);
        check(latr_execute_block(&lmul1_add, 1), 1, "LMUL=1 vadd.vv translates");
        check(vec_read32(3, 0), 30u, "LMUL=1 direct vadd lane 0");
        check(vec_read32(3, 3), 90u, "LMUL=1 direct vadd lane 3");
        if (latx_cpu_has_lsx()) {
            check(env.pvir_direct_count, 1, "LMUL=1 uses direct path");
            check(env.pvir_lsx_chunks, 1, "LMUL=1 uses 1 LSX chunk");
            check(env.pvir_lasx_chunks, 0, "LMUL=1 uses 0 LASX chunks");
        }

        /* 8b: LMUL=1 FP direct + mask logic direct. */
        reset_guest_state();
        env.csr[0x300] = 0x600;
        env.vtype = 0x10;
        env.vl = 4;
        vec_store32(2, 0, 0x3fc00000u); /* 1.5 */
        vec_store32(4, 0, 0x40000000u); /* 2.0 */
        vec_store32(2, 3, 0x3f800000u); /* 1.0 */
        vec_store32(4, 3, 0x3f000000u); /* 0.5 */
        check(latr_execute_block(&lmul1_fp, 1), 1, "LMUL=1 vfadd.vv translates");
        check(vec_read32(6, 0), 0x40600000u, "direct vfadd.vv lane 0 (3.5)");
        check(vec_read32(6, 3), 0x3fc00000u, "direct vfadd.vv lane 3 (1.5)");

        reset_guest_state();
        env.csr[0x300] = 0x600;
        env.vtype = 0x00;
        env.vl = 8;
        env.vreg[1][0] = 0x5a;
        env.vreg[2][0] = 0x3c;
        env.vreg[3][1] = 0x77;
        check(latr_execute_block(&mask_and, 1), 1, "vmand.mm direct translates");
        check(env.vreg[3][0], 0x18, "vmand.mm direct byte 0");
        check(env.vreg[3][1], 0x77, "vmand.mm preserves tail byte 1");

        /* 8c: LMUL=2 (32B) uses Tier 2 LASX on LASX hosts. */
        if (latx_cpu_has_lasx()) {
            reset_guest_state();
            env.csr[0x300] = 0x600;
            env.vtype = 0x11; /* e32, m2 */
            env.vl = 8;
            for (int lane = 0; lane < 8; lane++) {
                vec_store32(4, lane, (uint32_t)(lane + 1));
                vec_store32(6, lane, (uint32_t)((lane + 1) * 10));
            }
            check(latr_execute_block(&lmul2_add, 1), 1,
                  "LMUL=2 vadd.vv LASX translates");
            check(env.pvir_lasx_chunks, 1, "LMUL=2 uses 1 LASX chunk");
            check(env.pvir_lsx_chunks, 0, "LMUL=2 uses 0 LSX chunks");
            for (int lane = 0; lane < 8; lane++) {
                check(vec_read32(2, lane), (uint32_t)((lane + 1) * 11),
                      "LMUL=2 LASX lane sum");
            }

            /* 8d: Mixed LASX (32B) + LSX (16B) inside ONE instruction: LMUL=4, vl=12, SEW=32. */
            reset_guest_state();
            env.csr[0x300] = 0x600;
            env.vtype = 0x12; /* e32, m4, tu, mu */
            env.vl = 12;
            for (int lane = 0; lane < 12; lane++) {
                vec_store32(8, lane, (uint32_t)(100 + lane));
                vec_store32(12, lane, (uint32_t)(200 + lane));
            }
            for (int lane = 12; lane < 16; lane++) {
                vec_store32(4, lane, 0xdeadbeefu);
            }
            check(latr_execute_block(&lmul4_add, 1), 1,
                  "LMUL=4 mixed LASX+LSX translates");
            check(env.pvir_lasx_chunks, 1,
                  "mixed LASX+LSX runs 1 LASX chunk (bytes 0..31)");
            check(env.pvir_lsx_chunks, 1,
                  "mixed LASX+LSX runs 1 LSX tail chunk (bytes 32..47)");
            check(env.pvir_direct_count, 1, "mixed insn counts 1 direct op");
            for (int lane = 0; lane < 12; lane++) {
                check(vec_read32(4, lane), (uint32_t)(300 + lane * 2),
                      "mixed LASX+LSX active lane");
            }
            for (int lane = 12; lane < 16; lane++) {
                check(vec_read32(4, lane), 0xdeadbeefu,
                      "mixed LASX+LSX preserves tu tail v7");
            }
        }

        /* 8e: latx_cpu_set_vector_features gating (LSX-only & C helper fallback). */
        if (latx_cpu_has_lsx()) {
            latx_cpu_set_vector_features(1, 0);
            reset_guest_state();
            env.csr[0x300] = 0x600;
            env.vtype = 0x11; /* e32, m2 */
            env.vl = 8;
            for (int lane = 0; lane < 8; lane++) {
                vec_store32(4, lane, (uint32_t)(lane + 2));
                vec_store32(6, lane, (uint32_t)(lane + 3));
            }
            check(latr_execute_block(&lmul2_add, 1), 1,
                  "LSX-forced LMUL=2 translates");
            check(env.pvir_lasx_chunks, 0, "LSX-forced emits 0 LASX chunks");
            check(env.pvir_lsx_chunks, 2, "LSX-forced emits 2 LSX chunks");
            check(vec_read32(2, 7), 19u, "LSX-forced lane 7 matches");
        }
        latx_cpu_set_vector_features(0, 0);
        reset_guest_state();
        env.csr[0x300] = 0x600;
        env.vtype = 0x11;
        env.vl = 8;
        for (int lane = 0; lane < 8; lane++) {
            vec_store32(4, lane, (uint32_t)(lane + 5));
            vec_store32(6, lane, (uint32_t)(lane + 6));
        }
        check(latr_execute_block(&lmul2_add, 1), 1, "Tier 0 fallback translates");
        check(env.pvir_direct_count, 0, "Tier 0 fallback emits 0 direct");
        check(env.pvir_lasx_chunks, 0, "Tier 0 fallback emits 0 LASX");
        check(env.pvir_lsx_chunks, 0, "Tier 0 fallback emits 0 LSX");
        check(env.pvir_status, 0, "Tier 0 fallback PVIR validates");
        check(env.pvir_count, 1, "Tier 0 fallback executes via PVIR");
        check(vec_read32(2, 7), 25u, "Tier 0 fallback lane 7 matches");
        latx_cpu_set_vector_features(-1, -1);

        /* 8f: Masked FP inactive/tail lanes do not pollute fflags. */
        {
            static const uint32_t masked_fp[] = {
                0x80221357, /* vfdiv.vv v6, v2, v4, v0.t */
                0x4c2013d7, /* vfsqrt.v v7, v2, v0.t */
            };
            reset_guest_state();
            env.csr[0x300] = 0x600;
            env.vtype = 0x10; /* e32, m1, tu, mu */
            env.vl = 3;
            env.vreg[0][0] = 0x05; /* active: lanes 0, 2; masked: lane 1; tail: lane 3 */
            vec_store32(2, 0, 0x41100000u); /* 9.0 */
            vec_store32(4, 0, 0x40000000u); /* 2.0 */
            vec_store32(2, 1, 0xbf800000u); /* -1.0 (masked off!) */
            vec_store32(4, 1, 0x00000000u); /*  0.0 (masked off div-by-zero!) */
            vec_store32(2, 2, 0x40800000u); /* 4.0 */
            vec_store32(4, 2, 0x40000000u); /* 2.0 */
            vec_store32(2, 3, 0x00000000u); /* tail 0/0 / sqrt(-4.0) */
            vec_store32(4, 3, 0x00000000u);
            vec_store32(6, 1, 0xdead0001u);
            vec_store32(6, 3, 0xdead0003u);
            vec_store32(7, 1, 0xdead0011u);
            vec_store32(7, 3, 0xdead0013u);
            check(latr_execute_block(masked_fp, 2), 1,
                  "masked FP div/sqrt translates");
            check(env.fcsr, 0, "inactive/tail lanes raise zero FP flags");
            check(vec_read32(6, 0), 0x40900000u, "vfdiv active lane 0 (4.5)");
            check(vec_read32(6, 1), 0xdead0001u, "vfdiv preserves masked lane 1");
            check(vec_read32(6, 2), 0x40000000u, "vfdiv active lane 2 (2.0)");
            check(vec_read32(6, 3), 0xdead0003u, "vfdiv preserves tail lane 3");
            check(vec_read32(7, 0), 0x40400000u, "vfsqrt active lane 0 (3.0)");
            check(vec_read32(7, 1), 0xdead0011u, "vfsqrt preserves masked lane 1");
            check(vec_read32(7, 2), 0x40000000u, "vfsqrt active lane 2 (2.0)");
            check(vec_read32(7, 3), 0xdead0013u, "vfsqrt preserves tail lane 3");
        }
    }
}


static void test_mstatus_fs_vs(void)
{
    static const uint32_t fadd[] = { 0x0020f1d3 }; /* fadd.s f3, f1, f2, dyn */
    static const uint32_t vadd[] = { 0x021101d7 }; /* vadd.vv v3, v1, v2 */

    reset_guest_state();
    env.privilege = 3;
    env.csr[0x300] = 0; /* FS=Off */
    env.fpr[1] = UINT64_C(0xffffffff3f800000);
    env.fpr[2] = UINT64_C(0xffffffff40000000);
    check(latr_execute_block(fadd, 1), 1, "FS=Off block translates");
    check(env.exception, 2, "scalar FP with FS=Off is illegal");
    check(env.csr[LATR_TEST_MEPC], (uint64_t)(uintptr_t)fadd,
          "FS=Off trap reports the FP PC");

    reset_guest_state();
    env.privilege = 3;
    env.csr[0x300] = UINT64_C(1) << 13; /* FS=Initial */
    env.fpr[1] = UINT64_C(0xffffffff3f800000);
    env.fpr[2] = UINT64_C(0xffffffff40000000);
    check(latr_execute_block(fadd, 1), 1, "FS=Initial FP block translates");
    check(env.exception, 0, "FP executes when FS is enabled");
    check((env.csr[0x300] >> 13) & 3, 3, "FP execution marks FS dirty");

    reset_guest_state();
    env.privilege = 3;
    env.csr[0x300] = UINT64_C(1) << 9; /* VS=Initial */
    env.vtype = 0x10;
    env.vl = 4;
    vec_store32(1, 0, 1);
    vec_store32(2, 0, 2);
    check(latr_execute_block(vadd, 1), 1, "VS=Initial vector block translates");
    check(env.exception, 0, "vector executes when VS is enabled");
    check((env.csr[0x300] >> 9) & 3, 3, "vector execution marks VS dirty");

    static const uint32_t sret[] = { 0x10200073 };
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x300] = (UINT64_C(1) << 17) | (UINT64_C(1) << 8); /* MPRV, SPP=S */
    env.csr[0x141] = 0x80000000;
    check(latr_execute_block(sret, 1), 1, "sret block translates");
    check((env.csr[0x300] >> 17) & 1, 0, "sret clears MPRV");
    check(env.privilege, 1, "sret returns to S-mode");

    static const uint32_t fflags_csr[] = { 0x001022f3 }; /* csrr x5, fflags */
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x300] = 0;
    check(latr_execute_block(fflags_csr, 1), 1, "fflags with FS=Off translates");
    check(env.exception, 2, "fflags with FS=Off is illegal");

    reset_guest_state();
    env.privilege = 3;
    env.csr[0x300] = UINT64_C(1) << 13;
    check(latr_execute_block(fflags_csr, 1), 1, "fflags read with FS=Initial translates");
    check(env.exception, 0, "fflags access allowed when FS is on");
    {
        static const uint32_t fflags_write[] = { 0x00101073 }; /* csrw fflags, x0 */
        check(latr_execute_block(fflags_write, 1), 1,
              "fflags write with FS=Initial translates");
        check((env.csr[0x300] >> 13) & 3, 3, "fflags write marks FS dirty");
    }

    static const uint32_t hartid[] = { 0xf14022f3 }; /* csrr x5, mhartid */
    reset_guest_state();
    env.privilege = 3;
    env.hartid = 7;
    check(latr_execute_block(hartid, 1), 1, "mhartid read translates");
    check(env.gpr[5], 7, "mhartid returns the configured hart");

    static const uint32_t vendorid[] = { 0xf1102373 }; /* csrr x6, mvendorid */
    reset_guest_state();
    env.privilege = 3;
    check(latr_execute_block(vendorid, 1), 1, "mvendorid read translates");
    check(env.gpr[6], 0, "mvendorid reads zero");
}


static void test_pmp(void)
{
    uint64_t host = 0;
    uint64_t phys = 0;
    uint64_t pa = 0;

    reset_guest_state();
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          0, "permissive PMP allows S-mode loads");

    env.csr[0x3a0] = 0; /* no enabled entries */
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "S-mode without PMP rules faults");
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          0, "M-mode without PMP rules still succeeds");

    /* NA4 window at 0x1000 with R/W. */
    env.csr[0x3a0] = 0x13;
    env.csr[0x3b0] = 0x1000 >> 2;
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "NA4 PMP allows its window");
    check(latr_translate(&env, 0x2000, LATR_ACCESS_LOAD, 8, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "NA4 PMP rejects outside the window");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_STORE, 4, &host, &phys),
          0, "NA4 PMP allows configured stores");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_EXEC, 4, &host, &phys),
          LATR_EXCP_INST_ACCESS_FAULT, "NA4 PMP rejects unconfigured execute");

    /* 4 KiB NAPOT window at 0x1000. */
    env.csr[0x3a0] = 0x1b;
    env.csr[0x3b0] = (0x1000 >> 2) | 0x1ff;
    check(latr_translate(&env, 0x1ff8, LATR_ACCESS_LOAD, 8, &host, &phys),
          0, "NAPOT covers its last byte");
    check(latr_translate(&env, 0x2000, LATR_ACCESS_LOAD, 8, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "NAPOT stops at its upper bound");

    /* MMWP denies even M-mode when no rule matches. */
    env.csr[0x3a0] = 0;
    env.csr[0x747] = 2;
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "MMWP denies unmatched M-mode loads");
    env.csr[0x747] = 0;

    /* Locked cfg/addr entries are immutable. */
    static const uint32_t lock_ops[] = {
        0x3a009073, /* csrw pmpcfg0, x1 */
        0x3a0022f3, /* csrr x5, pmpcfg0 */
        0x3b011073, /* csrw pmpaddr0, x2 */
        0x3b002373, /* csrr x6, pmpaddr0 */
    };
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x3a0] = 0x93; /* L | NA4 | R/W/X */
    env.csr[0x3b0] = 0x400;
    env.gpr[1] = 0;
    env.gpr[2] = UINT64_MAX;
    check(latr_execute_block(lock_ops, 4), 1, "PMP lock block translates");
    check(env.gpr[5], 0x93, "locked pmpcfg ignores writes");
    check(env.gpr[6], 0x400, "locked pmpaddr ignores writes");

    /* 54-bit pmpaddr masking. */
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x3a0] = 0; /* unlocked */
    env.gpr[1] = 0;
    env.gpr[2] = UINT64_MAX;
    check(latr_execute_block(lock_ops, 4), 1, "pmpaddr mask block translates");
    check(env.gpr[6], (UINT64_C(1) << 54) - 1, "pmpaddr keeps 54 bits");

    /* MPRV uses MPP for data translation and PMP checks. */
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x3a0] = 0; /* no S/U rule */
    env.csr[0x300] = (UINT64_C(1) << 17) | (UINT64_C(1) << 11); /* MPRV, MPP=S */
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "MPRV applies S-mode PMP to M loads");
    env.csr[0x300] = 0;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          0, "clearing MPRV restores M-mode PMP bypass");

    /* First-match priority: entry 0 covers only part of the access, so the
     * access fails instead of falling through to the wider entry 1. */
    reset_guest_state();
    env.privilege = 1;
    env.csr[0x3a0] = 0x11 | (UINT64_C(0x1b) << 8); /* NA4 R; NAPOT RW */
    env.csr[0x3b0] = 0x1000 >> 2;
    env.csr[0x3b1] = (0x1000 >> 2) | 0x1ff;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 8, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT,
          "partial first match fails without fallback");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "first matching entry grants its own width");
    check(latr_translate(&env, 0x1004, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "next entry covers the following bytes");

    /* An A!=OFF entry with RWX=000 is still a matching, denying entry. */
    env.csr[0x3a0] = 0x10 | (UINT64_C(0x1b) << 8);
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "RWX=000 entry denies instead of skipping");

    /* M-mode bypass applies only to unlocked (L=0) matching entries. */
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x3a0] = 0x11; /* NA4 R, L=0 */
    env.csr[0x3b0] = 0x1000 >> 2;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_STORE, 4, &host, &phys),
          0, "M-mode ignores an unlocked rule's permissions");
    env.csr[0x3a0] = 0x91; /* L|NA4|R */
    check(latr_translate(&env, 0x1000, LATR_ACCESS_STORE, 4, &host, &phys),
          LATR_EXCP_STORE_ACCESS_FAULT, "locked rule constrains M-mode stores");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "locked rule still grants its own permission");

    /* mseccfg.MML truth table. L=0, W-only is a shared data region. */
    reset_guest_state();
    env.csr[0x747] = 4; /* MML */
    env.csr[0x3a0] = 0x1a; /* NAPOT W */
    env.csr[0x3b0] = (0x1000 >> 2) | 0x1ff;
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "MML M-mode reads the shared data region");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_STORE, 4, &host, &phys),
          0, "MML M-mode writes the shared data region");
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "MML S-mode reads the shared data region");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_STORE, 4, &host, &phys),
          LATR_EXCP_STORE_ACCESS_FAULT, "MML S-mode cannot write shared data");

    /* L=0, X-only is S/U execute-only and denied to M-mode. */
    env.csr[0x3a0] = 0x1c; /* NAPOT X */
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_EXEC, 4, &host, &phys),
          0, "MML S/U execute-only rule");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "MML S/U execute-only denies loads");
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_EXEC, 4, &host, &phys),
          LATR_EXCP_INST_ACCESS_FAULT, "MML M-mode execute-only rule");

    /* L=1, X=1 is M-mode execute-only and denied to S/U. */
    env.csr[0x3a0] = 0x9c; /* L|NAPOT|X */
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_EXEC, 4, &host, &phys),
          0, "MML locked M-mode execute rule");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "MML locked execute-only denies M loads");
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_EXEC, 4, &host, &phys),
          LATR_EXCP_INST_ACCESS_FAULT, "MML locked M-only rule denies S/U");

    /* L=1, RWX=111 is read-only for both M and S/U. */
    env.csr[0x3a0] = 0x9f;
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "MML locked shared data is M-readable");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_STORE, 4, &host, &phys),
          LATR_EXCP_STORE_ACCESS_FAULT, "MML locked shared data is not writable");
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "MML locked shared data is S-readable");

    /* MML default policy: M-mode data still succeeds, execute does not. */
    env.csr[0x3a0] = 0;
    env.privilege = 3;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          0, "MML unmatched M-mode load succeeds");
    check(latr_translate(&env, 0x1000, LATR_ACCESS_EXEC, 4, &host, &phys),
          LATR_EXCP_INST_ACCESS_FAULT, "MML unmatched M-mode fetch fails");
    env.privilege = 1;
    check(latr_translate(&env, 0x1000, LATR_ACCESS_LOAD, 4, &host, &phys),
          LATR_EXCP_LOAD_ACCESS_FAULT, "MML unmatched S-mode load fails");

    /* PTE read PMP faults use the original access type. */
    clear_page_tables();
    env.privilege = 1;
    env.csr[0x180] = (UINT64_C(8) << 60) |
                     ((uint64_t)(uintptr_t)pte_table[0] >> 12);
    map_page(8, UINT64_C(0x1000), UINT64_C(0x2000),
             LATR_PTE_V | LATR_PTE_R);
    {
        uint64_t root = (uint64_t)(uintptr_t)&pte_table[0][0];
        env.csr[0x747] = 0;
        env.csr[0x3a0] = 0x18; /* NAPOT, no permissions */
        env.csr[0x3b0] = (root >> 2) | 1; /* 8-byte NAPOT on the root PTE */
        check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
              LATR_EXCP_LOAD_ACCESS_FAULT, "PTE read fault maps to a load fault");
        check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_STORE, &pa),
              LATR_EXCP_STORE_ACCESS_FAULT,
              "PTE read fault maps to a store fault");
        check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_EXEC, &pa),
              LATR_EXCP_INST_ACCESS_FAULT,
              "PTE read fault maps to an instruction fault");
    }

    /* Svadu A/D write-back is an S-mode PMP store; denial is an access
     * fault of the original type and leaves the PTE unchanged. */
    clear_page_tables();
    map_page(8, UINT64_C(0x1000), UINT64_C(0x2000),
             LATR_PTE_V | LATR_PTE_R | LATR_PTE_W);
    {
        uint64_t leaf = (uint64_t)(uintptr_t)&pte_table[2][1];
        /* entry0: TOR below the leaf (RWX); entry1: 8-byte NAPOT leaf (R);
         * entry2: TOR above the leaf (RWX). */
        env.csr[0x3a0] = 0x0f | (UINT64_C(0x19) << 8) |
                         (UINT64_C(0x0f) << 16);
        env.csr[0x3b0] = leaf >> 2;
        env.csr[0x3b1] = (leaf >> 2) | 1;
        env.csr[0x3b2] = (UINT64_C(1) << 54) - 1;
        check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_LOAD, &pa),
              LATR_EXCP_LOAD_ACCESS_FAULT,
              "PMP-denied A update is a load access fault");
        check(pte_table[2][1] & LATR_PTE_A, 0,
              "PMP-denied A update leaves the PTE clear");
        check(latr_walk(&env, UINT64_C(0x1000), LATR_ACCESS_STORE, &pa),
              LATR_EXCP_STORE_ACCESS_FAULT,
              "PMP-denied D update is a store access fault");
    }

    /* mseccfg writes expose MML and keep MMWP/MML sticky. */
    static const uint32_t mseccfg_ops[] = {
        0x74709073, /* csrw mseccfg, x1 */
        0x747022f3, /* csrr x5, mseccfg */
    };
    reset_guest_state();
    env.privilege = 3;
    /* MML only permits M-mode fetch from a locked shared-code rule. */
    env.csr[0x3a0] = 0x9e; /* L|NAPOT|W|X */
    env.csr[0x3b0] = (UINT64_C(1) << 54) - 1;
    env.gpr[1] = 4; /* MML */
    check(latr_execute_block(mseccfg_ops, 2), 1, "mseccfg write translates");
    check(env.gpr[5], 4, "mseccfg writes MML");
    env.gpr[1] = 0;
    check(latr_execute_block(mseccfg_ops, 2), 1, "mseccfg clear translates");
    check(env.gpr[5], 4, "MML is sticky across writes");

    /* Under MML, adding an M-only executable rule is refused unless RLB
     * is set. Entry 0 stays as the locked shared-code fetch rule. */
    static const uint32_t mml_cfg_ops[] = {
        0x3a009073, /* csrw pmpcfg0, x1 */
        0x3a0022f3, /* csrr x5, pmpcfg0 */
    };
    reset_guest_state();
    env.privilege = 3;
    env.csr[0x747] = 4; /* MML */
    env.csr[0x3a0] = 0x9e; /* entry0: L|NAPOT|W|X shared code */
    env.csr[0x3b0] = (UINT64_C(1) << 54) - 1;
    /* entry0 stays 0x9e; entry1 would become L|NAPOT|X. */
    env.gpr[1] = UINT64_C(0x9c9e);
    check(latr_execute_block(mml_cfg_ops, 2), 1,
          "MML pmpcfg write translates");
    check(env.gpr[5], 0x9e, "MML refuses an M-only executable rule");
    env.csr[0x747] = 5; /* MML | RLB */
    check(latr_execute_block(mml_cfg_ops, 2), 1,
          "RLB pmpcfg write translates");
    check(env.gpr[5], UINT64_C(0x9c9e),
          "RLB accepts the M-only executable rule");
}


static void test_cross_page(void)
{
    static const uint32_t code[] __attribute__((aligned(4096))) = {
        0x0000b183, /* ld x3, 0(x1) */
        0x0020b023, /* sd x2, 0(x1) */
    };
    static uint8_t page0[4096] __attribute__((aligned(4096)));
    static uint8_t page1[4096] __attribute__((aligned(4096)));
    static LoongemuMemMap map;

    for (int i = 0; i < 4096; i++) {
        page0[i] = (uint8_t)i;
        page1[i] = (uint8_t)(i + 1);
    }
    loongemu_mem_map_init(&map);
    map_host_range(&map, code, 0x1000);
    check(loongemu_mem_map_add(&map, 0x1000, 0x1000,
                               (uint64_t)(uintptr_t)page0,
                               LOONGEMU_MEM_RAM, "page0"), 1,
          "cross-page low page registers");
    check(loongemu_mem_map_add(&map, 0x2000, 0x1000,
                               (uint64_t)(uintptr_t)page1,
                               LOONGEMU_MEM_RAM, "page1"), 1,
          "cross-page high page registers");

    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.gpr[1] = 0x1ffc;
    check(latr_execute_block(code, 1), 1, "cross-page load translates");
    check(env.exception, 0, "cross-page load does not fault");
    {
        uint64_t want = 0;
        for (int i = 0; i < 4; i++) {
            want |= (uint64_t)page0[0xffc + i] << (8 * i);
            want |= (uint64_t)page1[i] << (8 * (4 + i));
        }
        check(env.gpr[3], want, "cross-page load reads both pages");
    }

    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.gpr[1] = 0x1ffc;
    env.gpr[2] = UINT64_C(0x1122334455667788);
    check(latr_execute_block(code + 1, 1), 1, "cross-page store translates");
    check(env.exception, 0, "cross-page store does not fault");
    check(page0[0xffc], 0x88, "cross-page store low byte");
    check(page0[0xfff], 0x55, "cross-page store page0 byte 3");
    check(page1[0], 0x44, "cross-page store page1 byte 0");
    check(page1[3], 0x11, "cross-page store page1 byte 3");

    /* The unified interface is the path used by RVV/PVIR lanes. */
    {
        uint64_t value = 0;
        uint64_t want = 0;
        int i;

        page0[0xffc] = 0xaa;
        page0[0xffd] = 0xbb;
        page0[0xffe] = 0xcc;
        page0[0xfff] = 0xdd;
        page1[0] = 0x11;
        page1[1] = 0x22;
        page1[2] = 0x33;
        page1[3] = 0x44;
        reset_guest_state();
        env.privilege = 1;
        env.mem_map = &map;
        check(latr_guest_load(&env, 0x1ffc, 8, &value), 0,
              "guest_load splits at a page boundary");
        for (i = 0; i < 4; i++) {
            want |= (uint64_t)page0[0xffc + i] << (8 * i);
            want |= (uint64_t)page1[i] << (8 * (4 + i));
        }
        check(value, want, "guest_load assembles both pages");
        check(latr_guest_store(&env, 0x1ffc, 8,
                               UINT64_C(0x1122334455667788)), 0,
              "guest_store splits at a page boundary");
        check(page0[0xffc], 0x88, "guest_store writes page0 head");
        check(page0[0xfff], 0x55, "guest_store writes page0 tail");
        check(page1[0], 0x44, "guest_store writes page1 head");
        check(page1[3], 0x11, "guest_store writes page1 tail");
    }

    loongemu_mem_map_init(&map);
    map_host_range(&map, code, 0x1000);
    check(loongemu_mem_map_add(&map, 0x1000, 0x1000,
                               (uint64_t)(uintptr_t)page0,
                               LOONGEMU_MEM_RAM, "page0-only"), 1,
          "fault map low page registers");
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.gpr[1] = 0x1ffc;
    check(latr_execute_block(code, 1), 1, "faulting cross-page load translates");
    check(env.exception, LATR_EXCP_LOAD_ACCESS_FAULT,
          "cross-page fault reports an access fault");
    check(env.badaddr, 0x2000, "cross-page fault names the second page");

    {
        uint64_t value = UINT64_C(0xdeadbeef);
        check(latr_guest_load(&env, 0x1ffc, 8, &value),
              LATR_EXCP_LOAD_ACCESS_FAULT,
              "guest_load reports a second-page fault");
        check(env.mem_va, 0x2000, "guest_load names the faulting page");
        check(latr_guest_store(&env, 0x1ffc, 8, UINT64_C(0x12345678)),
              LATR_EXCP_STORE_ACCESS_FAULT,
              "guest_store reports a second-page fault");
        check(env.mem_va, 0x2000, "guest_store names the faulting page");
    }

    /* RVV unit-stride (PVIR lane path) and strided (rvv lane path) accesses
     * whose element spans two non-contiguous physical pages. */
    loongemu_mem_map_init(&map);
    map_host_range(&map, code, 0x1000);
    check(loongemu_mem_map_add(&map, 0x1000, 0x1000,
                               (uint64_t)(uintptr_t)page0,
                               LOONGEMU_MEM_RAM, "page0-rvv"), 1,
          "RVV map low page registers");
    check(loongemu_mem_map_add(&map, 0x2000, 0x1000,
                               (uint64_t)(uintptr_t)page1,
                               LOONGEMU_MEM_RAM, "page1-rvv"), 1,
          "RVV map high page registers");

    for (int i = 0; i < 4; i++) {
        page0[0xffc + i] = (uint8_t)(1 + i);
        page1[i] = (uint8_t)(5 + i);
    }
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.vtype = 0x18; /* SEW=64, LMUL=1 */
    env.vl = 1;
    env.vstart = 0;
    env.rvv_args[0] = 0x1ffc;
    env.rvv_args[1] = 8; /* vd */
    env.rvv_args[2] = 3; /* EEW=64 */
    env.rvv_args[3] = 1; /* load */
    env.rvv_args[4] = 1; /* unmasked */
    env.rvv_args[5] = 0;
    check(latr_rvv_mem(&env), 0, "unit-stride lane crosses a page");
    {
        uint64_t got = 0;
        for (int i = 0; i < 8; i++) {
            got |= (uint64_t)env.vreg[8][i] << (8 * i);
        }
        check(got, UINT64_C(0x0807060504030201),
              "unit-stride lane reads both pages");
    }
    for (int i = 0; i < 8; i++) {
        env.vreg[8][i] = (uint8_t)(0x80 + i);
    }
    env.vstart = 0;
    env.rvv_args[3] = 0; /* store */
    check(latr_rvv_mem(&env), 0, "unit-stride lane store crosses a page");
    check(page0[0xffc], 0x80, "unit-stride store writes page0 head");
    check(page0[0xfff], 0x83, "unit-stride store writes page0 tail");
    check(page1[0], 0x84, "unit-stride store writes page1 head");
    check(page1[3], 0x87, "unit-stride store writes page1 tail");

    for (int i = 0; i < 4; i++) {
        page0[0xffc + i] = (uint8_t)(0x10 + i);
        page1[i] = (uint8_t)(0x14 + i);
    }
    reset_guest_state();
    env.privilege = 1;
    env.mem_map = &map;
    env.vtype = 0x18; /* SEW=64, LMUL=1 */
    env.vl = 1;
    env.vstart = 0;
    env.rvv_args[0] = 0x1ffc;
    env.rvv_args[1] = 8; /* vd */
    env.rvv_args[2] = 3; /* EEW=64 */
    env.rvv_args[3] = 1; /* load */
    env.rvv_args[4] = 1; /* unmasked */
    env.rvv_args[5] = 0; /* stride 0 keeps the lane on the boundary */
    env.rvv_args[6] = 1; /* strided */
    check(latr_rvv_stride_mem(&env), 0, "strided lane crosses a page");
    {
        uint64_t got = 0;
        for (int i = 0; i < 8; i++) {
            got |= (uint64_t)env.vreg[8][i] << (8 * i);
        }
        check(got, UINT64_C(0x1716151413121110),
              "strided lane reads both pages");
    }
}


static void test_tb_boundaries(void)
{
    static const uint32_t flow[] __attribute__((aligned(4096))) = {
        0x00100093, /* addi x1, x0, 1 */
        0x0000006f, /* jal x0, 0 */
        0x00200113, /* addi x2, x0, 2 */
    };
    static uint8_t page[8192] __attribute__((aligned(4096)));
    const uint32_t addi = 0x00100093;

    reset_guest_state();
    memset(&tb, 0, sizeof(tb));
    tb.pc = (uint64_t)(uintptr_t)flow;
    tb.cflags = 0;
    {
        int limit = 3;
        target_disasm(&tb, &limit, &cpu);
    }
    check(tb.icount, 2, "JAL terminates a system TB");
    check(tb.size, 8, "JAL TB size covers both instructions");

    /* Two 32-bit instructions at 2-byte alignment: the second one crosses
     * the 4 KiB page and must start a new TB. */
    for (int i = 0; i < 4; i++) {
        page[0xffa + i] = (uint8_t)(addi >> (8 * i));
        page[0xffe + i] = (uint8_t)(addi >> (8 * i));
    }
    reset_guest_state();
    memset(&tb, 0, sizeof(tb));
    tb.pc = (uint64_t)(uintptr_t)(page + 0xffa);
    tb.cflags = 0;
    {
        int limit = 3;
        target_disasm(&tb, &limit, &cpu);
    }
    check(tb.icount, 1, "a later cross-page instruction ends the TB");
    check(tb.size, 4, "the cross-page instruction is not included");

    /* The unit-test compatibility flag deliberately keeps the old behavior. */
    reset_guest_state();
    memset(&tb, 0, sizeof(tb));
    tb.pc = (uint64_t)(uintptr_t)flow;
    tb.cflags = LATR_TB_CFLAGS_ALLOW_FLOW;
    {
        int limit = 3;
        target_disasm(&tb, &limit, &cpu);
    }
    check(tb.icount, 3, "unit flag allows a multi-flow block");
}


static void test_delegation(void)
{
    static const uint32_t code[] = {
        0x30309073, /* csrw mideleg, x1 */
        0x303022f3, /* csrr x5, mideleg */
        0x30209073, /* csrw medeleg, x1 */
        0x30202373, /* csrr x6, medeleg */
        0x34409073, /* csrw mip, x1 */
        0x344023f3, /* csrr x7, mip */
    };
    static const uint32_t read_mip[] = { 0x344022f3 };
    static LoongemuVClock vclock;

    reset_guest_state();
    env.privilege = 3;
    env.gpr[1] = UINT64_MAX;
    check(latr_execute_block(code, 6), 1, "delegation block translates");
    check(env.gpr[5], 0x222, "mideleg masks to S-level interrupts");
    check(env.gpr[6], 0xb3ff, "medeleg masks to delegatable exceptions");
    check(env.gpr[7] & 0xa, 0xa, "mip accepts MSIP and SSIP writes");

    loongemu_vclock_init(&vclock, LOONGEMU_VCLOCK_VIRTUAL_STEP,
                         UINT64_C(1000000000), 0, 0, 0, NULL, NULL);
    reset_guest_state();
    env.privilege = 3;
    env.vclock = &vclock;
    env.csr[0x14d] = 0; /* expired */
    check(latr_execute_block(read_mip, 1), 1, "mip read translates");
    check(env.gpr[5] & (UINT64_C(1) << 5), UINT64_C(1) << 5,
          "mip read refreshes Sstc STIP");
}

/* --- ELF loader contract --- */

static uint8_t elf_ram[0x40000] __attribute__((aligned(4096)));
static uint8_t elf_file[0x200] __attribute__((aligned(8)));
static LatrSystem elf_sys;

static void elf_put(uint8_t *p, unsigned off, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        p[off + i] = (uint8_t)(value >> (8 * i));
    }
}

static void elf_build_ph(uint8_t *elf, unsigned phoff, uint64_t paddr,
                         uint64_t vaddr, uint64_t off, uint64_t filesz,
                         uint64_t memsz)
{
    uint8_t *ph = elf + phoff;

    memset(ph, 0, 56);
    elf_put(ph, 0, 1, 4);        /* PT_LOAD */
    elf_put(ph, 4, 5, 4);        /* R+X */
    elf_put(ph, 8, off, 8);
    elf_put(ph, 16, vaddr, 8);
    elf_put(ph, 24, paddr, 8);
    elf_put(ph, 32, filesz, 8);
    elf_put(ph, 40, memsz, 8);
    elf_put(ph, 48, 0x1000, 8);
}

static void elf_build(uint64_t entry, uint64_t paddr, uint64_t vaddr,
                      uint16_t phnum)
{
    static const uint8_t ident[16] = { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0 };
    static const uint8_t payload[16] = {
        0x93, 0x00, 0x00, 0x00, 0x13, 0x01, 0x00, 0x00,
        0x93, 0x81, 0x01, 0x00, 0x13, 0x02, 0x00, 0x00,
    };

    memset(elf_file, 0, sizeof(elf_file));
    memcpy(elf_file, ident, sizeof(ident));
    elf_put(elf_file, 16, 2, 2);          /* ET_EXEC */
    elf_put(elf_file, 18, 243, 2);        /* EM_RISCV */
    elf_put(elf_file, 20, 1, 4);
    elf_put(elf_file, 24, entry, 8);
    elf_put(elf_file, 32, 64, 8);         /* e_phoff */
    elf_put(elf_file, 52, 64, 2);         /* e_ehsize */
    elf_put(elf_file, 54, 56, 2);         /* e_phentsize */
    elf_put(elf_file, 56, phnum, 2);
    elf_build_ph(elf_file, 64, paddr, vaddr, 0x100, 16, 32);
    memcpy(elf_file + 0x100, payload, sizeof(payload));
}

static void test_elf_loader(void)
{
    const uint64_t base = LATR_SYSTEM_RAM_BASE;
    const uint64_t paddr = base + 0x1000;
    const uint64_t vaddr = UINT64_C(0xdeadbeef);
    uint64_t entry = 0;
    uint8_t *ph = elf_file + 64;

    check(latr_system_init(&elf_sys, elf_ram, sizeof(elf_ram), 0), 1,
          "ELF loader board initialises");
    memset(elf_ram, 0xcc, sizeof(elf_ram));

    /* p_vaddr is deliberately outside RAM: loading at offset 16 would fail. */
    elf_build(paddr + 8, paddr, vaddr, 1);
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), &entry),
          1, "ELF with distinct p_vaddr/p_paddr loads");
    check(entry, paddr + 8, "ELF entry comes from e_entry");
    check(elf_sys.env.pc, paddr + 8, "ELF sets the start PC");
    check(elf_ram[0x1000], 0x93, "ELF loads at p_paddr");
    check(elf_ram[0x100f], 0x00, "ELF copies the whole file segment");
    check(elf_ram[0x1010], 0, "ELF zeroes memsz beyond filesz");
    check(elf_ram[0x101f], 0, "ELF zero fill reaches p_memsz");

    /* Every rejection must leave RAM untouched (validation precedes writes). */
    elf_ram[0x1000] = 0x5a;

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(elf_file, 18, 183, 2); /* EM_AARCH64 */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "non-RISC-V e_machine is rejected");
    check(elf_ram[0x1000], 0x5a, "rejected image leaves RAM untouched");

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(ph, 32, 40, 8); /* filesz > memsz */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "filesz > memsz is rejected");

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(ph, 24, base + sizeof(elf_ram) - 8, 8); /* memsz overruns RAM */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "segment past the end of RAM is rejected");

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(ph, 8, 0x1f8, 8); /* off + filesz > len */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "file range past the image is rejected");

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(ph, 40, 0, 8); /* memsz == 0 */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "empty segment is rejected");

    elf_build(paddr + 8, paddr, vaddr, 2);
    elf_build_ph(elf_file, 64 + 56, paddr + 16, paddr + 16, 0x100, 8, 8);
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "overlapping segments are rejected");

    elf_build(paddr + 0x100, paddr, vaddr, 1); /* entry outside */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "entry outside every segment is rejected");

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(elf_file, 32, UINT64_MAX - 8, 8); /* phoff overflow */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "program header offset overflow is rejected");

    elf_build(paddr + 8, paddr, vaddr, 1);
    elf_put(elf_file, 54, 40, 2); /* e_phentsize too small */
    check(latr_system_load_elf(&elf_sys, elf_file, sizeof(elf_file), NULL), 0,
          "short program header entries are rejected");
}

void kmain(void)
{
    uint64_t euen;
    __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
#ifdef LOONGEMU_PERF
    /* Whole-suite tick span: the ISA harness executes blocks directly, so
     * run_hart's per-TB split does not apply here. This is the comparable
     * number to diff against a baseline build. */
    const uint64_t perf_suite_start = loongemu_perf_now();
#endif
    euen |= 1u; /* EUEN.FPE: bare QEMU resets with native FP disabled. */
    if (latx_cpu_has_lsx()) {
        euen |= 2u; /* EUEN.SXE */
    }
    if (latx_cpu_has_lasx()) {
        euen |= 4u; /* EUEN.ASXE */
    }
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    latr_tr_data_init(&env);
    test_integer_and_branch();
    test_memory_and_compressed();
    latx_bare_uart_puts("latr-isa: fp rmm begin\n");
    test_fp_rmm();
    latx_bare_uart_puts("latr-isa: fp rmm done\n");
    test_fp_rmm_conversion();
    test_fp_double();
    test_fp_rmm_division();
    test_fp_nan_semantics();
    test_fp_fma();
    test_fp_reserved_rounding();
    test_csr();
    test_hpm_stateen();
    test_mstatus_fs_vs();
    test_delegation();
    test_pmp();
    test_cross_page();
    test_tb_boundaries();
    test_system_traps();
    test_illegal_decode();
    test_atomics();
    test_bitmanip();
    test_privileged();
    test_mmu_csr_state();
    test_mmu_walk();
    test_tlb();
    test_mmu_data_path();
    test_phys_map();
    test_mmio_path();
    test_uart_plic_devices();
    test_sbi_service();
    test_interrupts();
    test_tb_cache();
    test_translator_context();
    test_sfence_shootdown();
    test_elf_loader();
    test_rvv_state();
    test_rvv_ops();
    test_rvv_memory();
    test_rvv_whole_segment_ldff();
    test_rvv_vstart_restart();
    test_rvv_strided_indexed();
    test_rvv_masks();
    test_rvv_reductions();
    test_rvv_legality();
    test_rvv_permute();
    test_rvv_width_sat();
    test_rvv_fp();
    test_rvv_moves_wide_fp();
    test_rvv_pvir_lsx_lasx();
    test_tb_insn_map();
    test_tb_recovery();
    test_recovery_atomic_apply();
    test_mmu_fetch();
    test_counters();
    latx_bare_uart_puts("latr-diff=");
    puthex(diff_div);
    puthex(diff_rem);
    puthex(diff_mulw);
    puthex((diff_caddi << 24) | (diff_cbeqz_taken << 16) |
           (diff_cbeqz_not_taken << 8) | diff_cj);
    puthex(diff_divw);
    puthex(diff_remw);
    puthex(diff_divuw);
    puthex(diff_remuw);
    puthex(diff_fp_rmm);
    puthex(diff_fp_rmm_dynamic);
    puthex(diff_fp_div_rmm);
    puthex(diff_fp_fma);
    puthex(diff_rvv);
    latx_bare_uart_putc('\n');
    if (failures == 0) {
        latx_bare_uart_puts("latr-isa: PASS checks=");
    } else {
        latx_bare_uart_puts("latr-isa: FAIL checks=");
    }
    putdec((unsigned)checks);
#ifdef LOONGEMU_PERF
    {
        /* `make latr-isa-run PERF=1` reports the guest-access event mix for
         * this direct-execution harness plus the whole-suite tick span. The
         * per-TB compile/exec split is counted in run_hart, which this harness
         * does not drive, so those counters stay out of this line. */
        static const char *const names[] = {
            "tlb-hit", "tlb-miss", "mmio", "mem-fault", "plan-hit",
            "fast-hit",
        };
        const unsigned report = (unsigned)(sizeof(names) / sizeof(names[0]));

        latx_bare_uart_puts("\nlatr-perf:");
        for (unsigned i = 0; i < report; i++) {
            latx_bare_uart_putc(' ');
            latx_bare_uart_puts(names[i]);
            latx_bare_uart_putc('=');
            putdec((unsigned)loongemu_perf_read(i));
        }
        latx_bare_uart_puts(" helper-calls=");
        puthex(loongemu_perf_read(LOONGEMU_PERF_HELPER_CALLS));
        latx_bare_uart_puts(" suite-ticks=");
        puthex(loongemu_perf_now() - perf_suite_start);
    }
#endif
    {
        /* Code-symbol census: the relocation surface a persistent cache would
         * have to rewrite, measured rather than assumed. */
        latx_bare_uart_puts("\nlatr-code-syms: blocks=");
        putdec(code_symbol_blocks);
        latx_bare_uart_puts(" with_syms=");
        putdec(code_symbol_blocks_with_syms);
        latx_bare_uart_puts(" total=");
        putdec(code_symbol_total);
        latx_bare_uart_puts(" max_per_block=");
        putdec(code_symbol_max);
        latx_bare_uart_puts(" overflows=");
        putdec(code_symbol_overflows);
        checks++;
        if (code_symbol_blocks_with_syms == 0) {
            failures++;
            latx_bare_uart_puts("\nlatr-isa: FAIL code-symbol registry never fired");
        }
        /* A truncated relocation list is silently wrong; fail instead. */
        checks++;
        if (code_symbol_overflows != 0) {
            failures++;
            latx_bare_uart_puts("\nlatr-isa: FAIL code-symbol table overflowed");
        }
    }
    {
        /* Register-cache accounting for design P0 (ir2-value-version.md): the
         * numbers that decide whether versioned state is worth building. */
        LatrRegCacheStats rc = latr_regcache_stats();

        latx_bare_uart_puts("\nlatr-regcache: flushes=");
        putdec((unsigned)rc.flushes);
        latx_bare_uart_puts(" empty=");
        putdec((unsigned)rc.empty_flushes);
        latx_bare_uart_puts(" scans=");
        putdec((unsigned)rc.scan_entries);
        latx_bare_uart_puts(" writebacks=");
        putdec((unsigned)rc.writebacks);
        latx_bare_uart_puts(" reloads=");
        putdec((unsigned)rc.reloads);
        latx_bare_uart_puts(" stores=");
        putdec((unsigned)rc.stores);
        latx_bare_uart_puts(" evictions=");
        putdec((unsigned)rc.evictions);
    }
    latx_bare_uart_puts("\nlatr-isa: done\n");
}
