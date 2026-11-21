#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "exec/exec-all.h"
#include "riscv-env.h"
#include "tests/latr_isa_diff_vectors.h"

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)

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
static uint8_t code_buf[4096] __attribute__((aligned(4096)));
static uint64_t data_word;
static uint64_t other_data_word;
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

extern void latr_tr_data_init(CPUArchState *env);
extern void tr_init(void *tb);
extern void tr_fini(void);
extern int tr_ir2_assemble(const void *code_start_addr);
extern void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu);
extern bool tr_ir2_generate(TranslationBlock *tb);
extern void latr_enter_tb(void *env, void *code);

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
__asm__(".text\n.globl _start\n_start:\n  la $sp, latr_stack\n"
        "  lu12i.w $t0, 16\n  add.d $sp, $sp, $t0\n"
        "  bl kmain\n1: b 1b\n");
static uint8_t latr_stack[65536] __attribute__((aligned(16), used));

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

static bool run_block_with_regs_and_fpr(const void *guest, int insns,
                                        int reg0, uint64_t value0,
                                        int reg1, uint64_t value1,
                                        int fpreg, uint64_t fpvalue)
{
    uint8_t *state = (uint8_t *)&env;
    for (size_t i = 0; i < sizeof(env); i++) {
        state[i] = 0;
    }
    if (reg0 >= 0) {
        env.gpr[reg0] = value0;
    }
    if (reg1 >= 0) {
        env.gpr[reg1] = value1;
    }
    if (fpreg >= 0) {
        env.fpr[fpreg] = fpvalue;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)guest;
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;

    int max_insns = insns;
    target_disasm(&tb, &max_insns, &cpu);
    if (tb.icount != insns) {
        return false;
    }
    tr_init(&tb);
    bool ok = tr_ir2_generate(&tb);
    if (ok) {
        tr_ir2_assemble(code_buf);
        __asm__ volatile("ibar 0" ::: "memory");
        latr_enter_tb(&env, code_buf);
    }
    tr_fini();
    return ok;
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
    check(run_block(rmm_conversion,
                    sizeof(rmm_conversion) / sizeof(rmm_conversion[0]),
                    -1, 0),
          1, "RMM integer conversion block translated");
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
    check(env.pc, (uint64_t)(uintptr_t)reserved_static,
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
    check(env.pc, (uint64_t)(uintptr_t)(reserved_dynamic + 1),
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
    check(env.pc, (uint64_t)(uintptr_t)unsupported_user_csr,
          "unsupported CSR trap records faulting PC");
    check(env.badaddr, unsupported_user_csr[0],
          "unsupported CSR trap records instruction bits");

    static const uint32_t privileged_csr[] = {0x30009073};
    check(run_block_with_regs(privileged_csr, 1, 1, 1, -1, 0), 1,
          "privileged CSR block translated to trap");
    check(env.exception, 2, "user access to machine CSR is illegal");
    check(env.pc, (uint64_t)(uintptr_t)privileged_csr,
          "privileged CSR trap records faulting PC");
    check(env.badaddr, privileged_csr[0],
          "privileged CSR trap records instruction bits");
}

static void test_system_traps(void)
{
    static const uint32_t ecall[] = {0x00000073};
    check(run_block(ecall, 1, -1, 0), 1, "ECALL block translated");
    check(env.exception, 8, "ECALL records user exception");
    check(env.pc, (uint64_t)(uintptr_t)ecall, "ECALL records faulting PC");
    check(env.badaddr, 0, "ECALL leaves trap value zero");

    static const uint32_t ebreak[] = {0x00100073};
    check(run_block(ebreak, 1, -1, 0), 1, "EBREAK block translated");
    check(env.exception, 3, "EBREAK records breakpoint exception");
    check(env.pc, (uint64_t)(uintptr_t)ebreak, "EBREAK records faulting PC");
    check(env.badaddr, (uint64_t)(uintptr_t)ebreak,
          "EBREAK records trap value");

    static const uint16_t c_ebreak = 0x9002;
    check(run_block(&c_ebreak, 1, -1, 0), 1,
          "C.EBREAK block translated");
    check(env.exception, 3, "C.EBREAK records breakpoint exception");
    check(env.pc, (uint64_t)(uintptr_t)&c_ebreak,
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

void kmain(void)
{
    uint64_t euen;
    __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
    euen |= 1u; /* EUEN.FPE: bare QEMU resets with native FP disabled. */
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
    test_system_traps();
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
    latx_bare_uart_putc('\n');
    if (failures == 0) {
        latx_bare_uart_puts("latr-isa: PASS checks=");
    } else {
        latx_bare_uart_puts("latr-isa: FAIL checks=");
    }
    putdec((unsigned)checks);
    latx_bare_uart_puts("\nlatr-isa: done\n");
}
