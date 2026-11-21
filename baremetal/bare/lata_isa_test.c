/*
 * Stage 1 of the ARM64 path: exercise the LATA translator on AArch64 code and
 * check the results in the environment.
 *
 * The value of this over lata_smoke.c's two instructions is coverage of the
 * parts that are easy to get wrong and invisible until something depends on
 * them: NZCV generation and consumption (SUBS then CSET), 32-bit operands
 * (AND/ORR w-form must clear the upper half), shifts, MOVK's partial write,
 * and a store/load pair through memory.
 *
 * Each case is a self-contained block of guest code, translated and run at its
 * own entry point, so a failure names the construct rather than "the CPU".
 */
#include "qemu/osdep.h"
#include <stdio.h>
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "target/arm/cpu.h"
#include "target/arm/tcg/translate.h"
#include "target/arm/lata/include/env.h"
#include "target/arm/lata/include/la-append.h"
#include "target/arm/lata/include/macro-inst.h"
#include "target/arm/lata/ir1/sve-pvir.h"
#include "target/arm/lata/translator/sve-lasx-lower.h"
#include "latx/compat/qemu/cpu-features.h"
extern int lata_gen_prologue(TCGContext *tcg_ctx);
extern int lata_gen_epilogue(TCGContext *tcg_ctx);
extern uint64_t context_switch_bt_to_native;
extern const int arm_la_fmap[];
extern IR2_OPND a0_ir2_opnd;
extern IR2_OPND a1_ir2_opnd;
extern IR2_OPND ra_ir2_opnd;
extern IR2_OPND zero_ir2_opnd;

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s) { while (*s) { latx_bare_uart_putc(*s++); } }

static void puthex(uint64_t v)
{
    char b[17];
    for (int i = 15; i >= 0; i--) { b[15 - i] = "0123456789abcdef"[(v >> (i * 4)) & 0xf]; }
    b[16] = 0;
    latx_bare_uart_puts(b);
}

static void putdec(uint64_t v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    if (v == 0) { b[--i] = '0'; }
    while (v && i > 0) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    latx_bare_uart_puts(&b[i]);
}

static int checks, fails;
static bool test_aa64_lse;

static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        latx_bare_uart_puts("lata-isa: FAIL ");
        latx_bare_uart_puts(what);
        latx_bare_uart_puts(" got=");
        puthex(got);
        latx_bare_uart_puts(" want=");
        puthex(want);
        latx_bare_uart_puts("\n");
    }
}

/* Called by the exit trampoline with whatever the TB left in $a2. */
volatile uint64_t lata_result;
volatile uint64_t lata_result_x3;
uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];

static uint8_t code_buf[1 << 16] __attribute__((aligned(4096)));
static TranslationBlock tb;
static CPUARMState env;
static CPUState cpu;

extern void lata_tr_data_init(void *env);
extern int tr_ir2_assemble(const void *code_start_addr);
extern void target_disasm(TranslationBlock *tb, int *max_insns, CPUState *cpu);
extern bool tr_ir2_generate(TranslationBlock *tb);
extern void tr_init(void *tb);
extern void tr_fini(void);
extern uint64_t context_switch_native_to_bt;
extern uint64_t context_switch_native_to_bt_ret_0;

void lata_enter_tb(void *envp, void *code);

__asm__(
".text\n"
".globl lata_enter_tb\n"
"lata_enter_tb:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $s8, $sp, 8\n"
"  move   $s8, $a0\n"
"  jirl   $ra, $a1, 0\n"
"  la     $t0, lata_result\n"
"  st.d   $a2, $t0, 0\n"
"  la     $t0, lata_result_x3\n"
"  st.d   $a3, $t0, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $s8, $sp, 8\n"
"  addi.d $sp, $sp, 16\n"
"  jirl   $zero, $ra, 0\n"
".globl lata_exit_tramp\n"
"lata_exit_tramp:\n"
"  jirl   $zero, $ra, 0\n");

uint32_t lata_guest_ldl(uint64_t addr)
{
    return *(const uint32_t *)(uintptr_t)addr;
}

void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    uint8_t *a = base;
    uint8_t tmp[64];
    if (sz > sizeof(tmp)) { return; }
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0; j--) {
            uint8_t *x = a + (j - 1) * sz, *y = a + j * sz;
            if (cmp(x, y) <= 0) { break; }
            for (size_t k = 0; k < sz; k++) { tmp[k] = x[k]; }
            for (size_t k = 0; k < sz; k++) { x[k] = y[k]; }
            for (size_t k = 0; k < sz; k++) { y[k] = tmp[k]; }
        }
    }
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");

static char kstack[65536] __attribute__((aligned(16), used));
FILE *stderr;

/*
 * Run one guest block and return what the TB left in the environment.
 *
 * Translating at the block's own entry each time keeps the cases independent:
 * a wrong value cannot leak from one to the next through the register file
 * unless the instruction itself wrote it there.
 */
static void run_block(uint32_t *guest, int insns, const char *name)
{
    latx_bare_uart_puts("lata-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" begin\n");
    extern void lata_exit_tramp(void);

    for (size_t i = 0; i < sizeof(env); i++) { ((uint8_t *)&env)[i] = 0; }
    for (size_t i = 0; i < sizeof(cpu); i++) { ((uint8_t *)&cpu)[i] = 0; }
    for (size_t i = 0; i < sizeof(tb); i++) { ((uint8_t *)&tb)[i] = 0; }

    cpu.env_ptr = &env;
    arm_cpu_set_aa64_lse(&env.arm_cpu, test_aa64_lse);
    env.pc = (uint64_t)(uintptr_t)guest;

    lata_tr_data_init(&env);
    context_switch_native_to_bt = (uint64_t)(uintptr_t)lata_exit_tramp;
    context_switch_native_to_bt_ret_0 = (uint64_t)(uintptr_t)lata_exit_tramp;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;

    tb.pc = (uint64_t)(uintptr_t)guest;
    tb.tc.ptr = code_buf;
    tb.cflags = 0;

    int max_insns = insns;
    target_disasm(&tb, &max_insns, &cpu);
    if (tb.icount != (uint32_t)insns) {
        latx_bare_uart_puts("lata-isa: ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" decoded=");
        puthex(tb.icount);
        latx_bare_uart_puts(" want=");
        puthex((uint64_t)insns);
        latx_bare_uart_puts("\n");
    }

    tr_init(&tb);
    bool ok = tr_ir2_generate(&tb);
    if (!ok) {
        fails++;
        checks++;
        latx_bare_uart_puts("lata-isa: FAIL ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" ir2_generate\n");
        tr_fini();
        return;
    }
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");

    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("lata-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" done\n");
}

/*
 * Exercise the generated context-switch pair, not run_block()'s direct TB
 * entry. The tiny IR2 TB writes three distinct vectors, snapshots them to
 * disjoint scratch storage, then branches through the generated epilogue.
 */
static uint8_t context_vector_expected[3][16]
    __attribute__((aligned(16)));

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    return value;
}

static void enable_context_units(void)
{
    uint64_t euen = (1u << 0) | (1u << 1);
    uint64_t actual;

    /* Enable only FPE+SXE; keep ASXE and BTE disabled. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "context fixture enables FPE+SXE with BTE off");
}

static void enable_bte_only(void)
{
    uint64_t euen = 1u << 3;
    uint64_t actual;

    /* Guest flag instructions use LBT_ARM; keep the other units disabled. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "integer/NZCV matrix enables only EUEN.BTE");
}

static void disable_optional_units(void)
{
    uint64_t euen = 0;
    uint64_t actual;

    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "scalar memory fixture disables optional EUEN units");
}

static void enable_sxe_only(void)
{
    uint64_t euen = 1u << 1;
    uint64_t actual;

    /* Do not inherit FPE, ASXE, or BTE from other fixture setup. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "NEON fixture enables only EUEN.SXE");
}

static void enable_sxe_asxe_only(void)
{
    uint64_t euen = (1u << 1) | (1u << 2);
    uint64_t actual;

    /* Exercise emitted LASX only; keep floating point and LBT disabled. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "SVE PVIR fixture enables only SXE and ASXE");
}


static void run_context_vector_save_restore(void)
{
    enum {
        PROLOGUE_OFFSET = 0x1000,
        EPILOGUE_OFFSET = 0x3000,
        TB_OFFSET = 0x5000,
    };
    const uint32_t pstate_nzcv = 0xa0000000u;
    uint8_t *prologue_code = code_buf + PROLOGUE_OFFSET;
    uint8_t *epilogue_code = code_buf + EPILOGUE_OFFSET;
    uint8_t *tb_code = code_buf + TB_OFFSET;
    uint8_t *env_bytes = (uint8_t *)&env;
    IR2_OPND v0 = ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[0]);
    IR2_OPND v1 = ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[1]);
    IR2_OPND v31 = ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[31]);

    for (size_t i = 0; i < sizeof(env); i++) {
        env_bytes[i] = 0;
    }
    for (size_t i = 0; i < sizeof(cpu); i++) {
        ((uint8_t *)&cpu)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }

    cpu.env_ptr = &env;
    lata_tr_data_init(&env);
    env.pstate = pstate_nzcv;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;

    /* run_block() leaves these set to its direct-exit trampoline. */
    context_switch_bt_to_native = 0;
    context_switch_native_to_bt = 0;
    context_switch_native_to_bt_ret_0 = 0;

    tcg_ctx->code_ptr = prologue_code;
    latx_bare_uart_puts("lata-isa: context prologue begin\n");
    check(lata_gen_prologue(tcg_ctx) > 0, 1,
          "context prologue generated");
    latx_bare_uart_puts("lata-isa: context prologue done\n");
    tcg_ctx->code_ptr = epilogue_code;
    latx_bare_uart_puts("lata-isa: context epilogue begin\n");
    check(lata_gen_epilogue(tcg_ctx) > 0, 1,
          "context epilogue generated");
    latx_bare_uart_puts("lata-isa: context epilogue done\n");

    /*
     * This is a tiny native IR2 TB between those hooks. The scratch stores
     * preserve the actual emitted vector patterns for independent comparison.
     */
    tcg_ctx->code_ptr = tb_code;
    tr_init(&tb);
    la_vldi(v0, 0x11);
    la_vldi(v1, 0x22);
    la_vldi(v31, 0x33);
    li_d(a0_ir2_opnd, (int64_t)(uintptr_t)context_vector_expected);
    la_vst(v0, a0_ir2_opnd, 0);
    la_vst(v1, a0_ir2_opnd, 16);
    la_vst(v31, a0_ir2_opnd, 32);
    li_d(ra_ir2_opnd, (int64_t)context_switch_native_to_bt);
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(tb_code);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");

    enable_context_units();
    latx_bare_uart_puts("lata-isa: context switch begin\n");
    ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(
        &env, tb_code);
    latx_bare_uart_puts("lata-isa: context switch done\n");
    check(env.pstate, pstate_nzcv,
          "context preserves PSTATE.NZCV with BTE disabled");

    check(read_le64(context_vector_expected[0]) !=
              read_le64(context_vector_expected[1]),
          1, "context fixture V0/V1 patterns differ");
    check(read_le64(context_vector_expected[1]) !=
              read_le64(context_vector_expected[2]),
          1, "context fixture V1/V31 patterns differ");
    check(read_le64(env_bytes + env_offset_fpr(0)),
          read_le64(context_vector_expected[0]), "context save V0 low");
    check(read_le64(env_bytes + env_offset_fpr(0) + 8),
          read_le64(context_vector_expected[0] + 8), "context save V0 high");
    check(read_le64(env_bytes + env_offset_fpr(1)),
          read_le64(context_vector_expected[1]), "context save V1 low");
    check(read_le64(env_bytes + env_offset_fpr(1) + 8),
          read_le64(context_vector_expected[1] + 8), "context save V1 high");
    check(read_le64(env_bytes + env_offset_fpr(31)),
          read_le64(context_vector_expected[2]), "context save V31 low");
    check(read_le64(env_bytes + env_offset_fpr(31) + 8),
          read_le64(context_vector_expected[2] + 8), "context save V31 high");
    check(env.xregs[0], context_switch_native_to_bt,
          "V31 upper half must not overwrite xregs[0]");
}

static void run_integer_nzcv_matrix(void)
{
    /* Encodings assembled with clang --target=aarch64-none-elf and llvm-objdump. */
    /* CSET packs x2[3:0] = {V,C,Z,N}; csel_false omits V merge because V=0. */
    /* Direct MRS NZCV is not implemented by this compatibility decoder. */
    static uint32_t adds_carry_zero[] __attribute__((aligned(16))) = {
        0x92800001u,  /* movn x1, #0 */
        0xd2800022u,  /* movz x2, #1 */
        0xab020023u,  /* adds x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adds_overflow[] __attribute__((aligned(16))) = {
        0xd29fffe1u,  /* movz x1, #0xffff */
        0xf2bfffe1u,  /* movk x1, #0xffff, lsl #16 */
        0xf2dfffe1u,  /* movk x1, #0xffff, lsl #32 */
        0xf2efffe1u,  /* movk x1, #0x7fff, lsl #48 */
        0xd2800022u,  /* movz x2, #1 */
        0xab020023u,  /* adds x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adds_negative[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0x92800002u,  /* movn x2, #0 */
        0xab020023u,  /* adds x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t subs_zero[] __attribute__((aligned(16))) = {
        0xd28000a1u,  /* movz x1, #5 */
        0xd28000a2u,  /* movz x2, #5 */
        0xeb020023u,  /* subs x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t subs_borrow[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800022u,  /* movz x2, #1 */
        0xeb020023u,  /* subs x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    /* Keep the 10-instruction standalone TB within one translation page. */
    static uint32_t subs_overflow[] __attribute__((aligned(4096))) = {
        0xd2f00001u,  /* movz x1, #0x8000, lsl #48 */
        0xd2800022u,  /* movz x2, #1 */
        0xeb020023u,  /* subs x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adc_carry[] __attribute__((aligned(16))) = {
        0xd2800021u,  /* movz x1, #1 */
        0xd28000a2u,  /* movz x2, #5 */
        0xd28000e4u,  /* movz x4, #7 */
        0xeb01003fu,  /* cmp x1, x1: C=1, Z=1 */
        0x9a040043u,  /* adc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adc_no_carry[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd28000a2u,  /* movz x2, #5 */
        0xd28000e4u,  /* movz x4, #7 */
        0xeb05003fu,  /* cmp x1, x5: C=0, N=1 */
        0x9a040043u,  /* adc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t sbc_carry[] __attribute__((aligned(16))) = {
        0xd2800021u,  /* movz x1, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb01003fu,  /* cmp x1, x1: C=1, Z=1 */
        0xda040043u,  /* sbc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t sbc_borrow[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb05003fu,  /* cmp x1, x5: C=0, N=1 */
        0xda040043u,  /* sbc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t bcond_taken[] __attribute__((aligned(16))) = {
        0xd2800021u,  /* movz x1, #1 */
        0xd2800223u,  /* movz x3, #0x11 */
        0xd2800025u,  /* movz x5, #1 */
        0xeb01003fu,  /* cmp x1, x1: Z=1, C=1 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
        0x54000040u,  /* b.eq +8: taken target */
    };
    static uint32_t bcond_not_taken[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800223u,  /* movz x3, #0x11 */
        0xeb05003fu,  /* cmp x1, x5: Z=0, C=0 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
        0x54000040u,  /* b.eq +8: not taken */
    };
    static uint32_t csel_true[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb05003fu,  /* cmp x1, x5: LT */
        0x9a84b043u,  /* csel x3, x2, x4, lt */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t csel_false[] __attribute__((aligned(16))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb05003fu,  /* cmp x1, x5: GT false */
        0x9a84c043u,  /* csel x3, x2, x4, gt */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V=0 for this comparison */
    };

    enable_bte_only();
    latx_bare_uart_puts("lata-isa: integer/NZCV matrix begin\n");

    run_block(adds_carry_zero, 10, "adds-carry-zero");
    check(lata_result_x3, 0, "ADDS carry destination");
    check(lata_result, 6, "ADDS carry/zero NZCV nibble");
    run_block(adds_overflow, 13, "adds-signed-overflow");
    check(lata_result_x3, 0x8000000000000000ull,
          "ADDS overflow destination");
    check(lata_result, 9, "ADDS negative/overflow NZCV nibble");
    run_block(adds_negative, 10, "adds-negative");
    check(lata_result_x3, UINT64_MAX, "ADDS negative destination");
    check(lata_result, 1, "ADDS negative NZCV nibble");

    run_block(subs_zero, 10, "subs-zero");
    check(lata_result_x3, 0, "SUBS zero destination");
    check(lata_result, 6, "SUBS zero NZCV nibble");
    run_block(subs_borrow, 10, "subs-borrow");
    check(lata_result_x3, UINT64_MAX, "SUBS borrow destination");
    check(lata_result, 1, "SUBS borrow/negative NZCV nibble");
    run_block(subs_overflow, 10, "subs-signed-overflow");
    check(tb.icount, 10, "SUBS overflow block fully decoded");
    latx_bare_uart_puts("lata-isa: SUBS overflow NZCV=");
    puthex(lata_result);
    latx_bare_uart_puts("\n");
    check(lata_result_x3, 0x7fffffffffffffffull,
          "SUBS overflow destination");
    check(lata_result, 12, "SUBS carry/overflow NZCV nibble");

    run_block(adc_carry, 12, "adc-carry-in");
    check(lata_result_x3, 13, "ADC carry-in destination");
    check(lata_result, 6, "ADC preserves carry NZCV nibble");
    run_block(adc_no_carry, 13, "adc-no-carry-in");
    check(lata_result_x3, 12, "ADC no-carry destination");
    check(lata_result, 1, "ADC preserves no-carry NZCV nibble");
    run_block(sbc_carry, 12, "sbc-no-borrow");
    check(lata_result_x3, 5, "SBC carry/no-borrow destination");
    check(lata_result, 6, "SBC preserves carry NZCV nibble");
    run_block(sbc_borrow, 13, "sbc-borrow");
    check(lata_result_x3, 4, "SBC borrow destination");
    check(lata_result, 1, "SBC preserves no-carry NZCV nibble");

    run_block(bcond_taken, 12, "bcond-taken");
    check(lata_result_x3, 0x11, "B.EQ preserves x3");
    check(env.pc, (uint64_t)(uintptr_t)bcond_taken + 52,
          "B.EQ taken target PC");
    check(lata_result, 6, "B.EQ taken NZCV nibble");
    run_block(bcond_not_taken, 12, "bcond-not-taken");
    check(lata_result_x3, 0x11, "B.EQ not-taken destination");
    check(env.pc, (uint64_t)(uintptr_t)bcond_not_taken + 48,
          "B.EQ not-taken fallthrough PC");
    check(lata_result, 1, "B.EQ not-taken NZCV nibble");
    run_block(csel_true, 13, "csel-true");
    check(lata_result_x3, 9, "CSEL LT destination");
    check(lata_result, 1, "CSEL LT preserves NZCV nibble");
    run_block(csel_false, 12, "csel-false");
    check(lata_result_x3, 4, "CSEL GT destination");
    check(lata_result, 1, "CSEL GT preserves NZCV nibble");

    latx_bare_uart_puts("lata-isa: integer/NZCV matrix done\n");
}


static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}
static void write_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned byte = 0; byte < 4; ++byte) {
        bytes[byte] = (uint8_t)(value >> (byte * 8u));
    }
}


static void run_scalar_memory_matrix(void)
{
    /* A64 words were assembled with clang and cross-checked by llvm-objdump. */
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[3];
    } ldr_str = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xf9400022u,  /* ldr x2, [x1] */
            0xd2995fc3u,  /* movz x3, #0xcafe */
            0xf9000423u,  /* str x3, [x1, #8] */
        },
        .data = {
            0x0123456789abcdefull,
            0xaaaaaaaaaaaaaaaaull,
            0xbbbbbbbbbbbbbbbbull,
        },
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[6];
    } ldp_stp = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xa9400c22u,  /* ldp x2, x3, [x1] */
            0xa9010c22u,  /* stp x2, x3, [x1, #16] */
            0xd503201fu,  /* nop: keep data at the 16-byte offset */
        },
        .data = {
            0x0123456789abcdefull,
            0x1020304050607080ull,
            0xaaaaaaaaaaaaaaaaull,
            0xbbbbbbbbbbbbbbbbull,
            0xccccccccccccccccull,
            0xddddddddddddddddull,
        },
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[7];
        uint8_t data[32];
    } unaligned = {
        .code = {
            0x300000e1u,  /* adr x1, data + 1 */
            0xf9400022u,  /* ldr x2, [x1] */
            0xd28ef103u,  /* movz x3, #0x7788 */
            0xf2aaacc3u,  /* movk x3, #0x5566, lsl #16 */
            0xf2c66883u,  /* movk x3, #0x3344, lsl #32 */
            0xf2e22443u,  /* movk x3, #0x1122, lsl #48 */
            0xf9000423u,  /* str x3, [x1, #8] */
        },
        .data = {
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
            0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        },
    };
    const uint64_t unaligned_store = 0x1122334455667788ull;
    check((uintptr_t)ldr_str.data - (uintptr_t)ldr_str.code,
          sizeof(ldr_str.code), "LDR/STR ADR fixture layout");
    check((uintptr_t)ldp_stp.data - (uintptr_t)ldp_stp.code,
          sizeof(ldp_stp.code), "LDP/STP ADR fixture layout");
    check((uintptr_t)unaligned.data - (uintptr_t)unaligned.code,
          sizeof(unaligned.code), "unaligned ADR fixture layout");

    disable_optional_units();
    latx_bare_uart_puts("lata-isa: scalar memory matrix begin\n");

    run_block(ldr_str.code, 4, "ldr-str-aligned");
    check(lata_result, 0x0123456789abcdefull, "LDR aligned loaded value");
    check(lata_result_x3, 0xcafe, "STR aligned source value");
    check(ldr_str.data[0], 0x0123456789abcdefull,
          "STR aligned preserves preceding word");
    check(ldr_str.data[1], 0xcafe, "STR aligned stored value");
    check(ldr_str.data[2], 0xbbbbbbbbbbbbbbbbull,
          "STR aligned preserves adjacent word");

    run_block(ldp_stp.code, 3, "ldp-stp-aligned");
    check(lata_result, 0x0123456789abcdefull, "LDP first loaded value");
    check(lata_result_x3, 0x1020304050607080ull,
          "LDP second loaded value");
    check(ldp_stp.data[0], 0x0123456789abcdefull,
          "STP preserves source first word");
    check(ldp_stp.data[1], 0x1020304050607080ull,
          "STP preserves source second word");
    check(ldp_stp.data[2], 0x0123456789abcdefull,
          "STP first stored value");
    check(ldp_stp.data[3], 0x1020304050607080ull,
          "STP second stored value");
    check(ldp_stp.data[4], 0xccccccccccccccccull,
          "STP preserves following word");
    check(ldp_stp.data[5], 0xddddddddddddddddull,
          "STP preserves final guard word");

    run_block(unaligned.code, 7, "ldr-str-unaligned");
    check(lata_result, 0x1817161514131211ull,
          "unaligned LDR loaded value");
    check(lata_result_x3, unaligned_store, "unaligned STR source value");
    for (size_t i = 0; i < sizeof(unaligned.data); i++) {
        uint8_t want = (uint8_t)(0x10u + i);
        if (i >= 9 && i < 17) {
            want = (uint8_t)(unaligned_store >> ((i - 9) * 8));
        }
        check(unaligned.data[i], want,
              i >= 9 && i < 17 ? "unaligned STR written byte"
                              : "unaligned STR preserves adjacent byte");
    }

    latx_bare_uart_puts("lata-isa: scalar memory matrix done\n");
}

static void run_exclusive_atomic_matrix(void)
{
    /* Assembled with clang --target=aarch64-none-elf; LDXR/STXR are base A64. */
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } stxr_success = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xc85f7c22u,  /* ldxr x2, [x1] */
            0xd2824684u,  /* movz x4, #0x1234 */
            0xc8037c24u,  /* stxr w3, x4, [x1] */
        },
        .data = {0x1111222233334444ull, 0xaaaaaaaaaaaaaaaaull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[6];
        uint64_t data[2];
    } stxr_failed_after_store = {
        .code = {
            0x100000c1u,  /* adr x1, data */
            0xc85f7c22u,  /* ldxr x2, [x1] */
            0xd28acf04u,  /* movz x4, #0x5678 */
            0xf9000024u,  /* str x4, [x1]: clear the local reservation */
            0xd2802465u,  /* movz x5, #0x123 */
            0xc8037c25u,  /* stxr w3, x5, [x1] */
        },
        .data = {0x0102030405060708ull, 0xbbbbbbbbbbbbbbbbull},
    };
    /* LSE words assembled with .arch armv8.1-a+lse and objdump-checked. */
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } cas_success = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd2824682u,  /* movz x2, #0x1234 */
            0xd28acf03u,  /* movz x3, #0x5678 */
            0xc8a27c23u,  /* cas x2, x3, [x1] */
        },
        .data = {0x1234, 0xccccccccccccccccull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } cas_failure = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd2935782u,  /* movz x2, #0x9abc */
            0xd28acf03u,  /* movz x3, #0x5678 */
            0xc8a27c23u,  /* cas x2, x3, [x1] */
        },
        .data = {0x1234, 0xddddddddddddddddull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } ldadd = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd28000e2u,  /* movz x2, #7 */
            0xf8220023u,  /* ldadd x2, x3, [x1] */
            0xd503201fu,  /* nop */
        },
        .data = {0x1111222233334444ull, 0xeeeeeeeeeeeeeeeeull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } ldadd_alias = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd28000a2u,  /* movz x2, #5 */
            0xf2c00022u,  /* movk x2, #1, lsl #32 */
            0xf8220022u,  /* ldadd x2, x2, [x1] */
        },
        .data = {0x1122334455667788ull, 0xaaaaaaaaaaaaaaaaull},
    };
    ARMISAState zero_isar = {0};

    check(arm_isar_has_aa64_lse(&zero_isar), 0,
          "zero-initialized compat CPU lacks FEAT_LSE");
    check((uintptr_t)stxr_success.data - (uintptr_t)stxr_success.code,
          sizeof(stxr_success.code), "LDXR/STXR success fixture layout");
    check((uintptr_t)stxr_failed_after_store.data -
              (uintptr_t)stxr_failed_after_store.code,
          sizeof(stxr_failed_after_store.code),
          "LDXR/STXR failure fixture layout");
    check((uintptr_t)cas_success.data - (uintptr_t)cas_success.code,
          sizeof(cas_success.code), "CAS success fixture layout");
    check((uintptr_t)cas_failure.data - (uintptr_t)cas_failure.code,
          sizeof(cas_failure.code), "CAS failure fixture layout");
    check((uintptr_t)ldadd.data - (uintptr_t)ldadd.code,
          sizeof(ldadd.code), "LDADD fixture layout");
    check((uintptr_t)ldadd_alias.data - (uintptr_t)ldadd_alias.code,
          sizeof(ldadd_alias.code), "LDADD alias fixture layout");

    disable_optional_units();
    latx_bare_uart_puts("lata-isa: exclusive atomics begin\n");

    run_block(stxr_success.code, 4, "ldxr-stxr-success");
    check(lata_result, 0x1111222233334444ull, "LDXR loaded value");
    check(lata_result_x3, 0, "STXR successful status");
    check(stxr_success.data[0], 0x1234, "STXR successful store");
    check(stxr_success.data[1], 0xaaaaaaaaaaaaaaaaull,
          "STXR success preserves adjacent word");

    run_block(stxr_failed_after_store.code, 6, "ldxr-stxr-failed");
    check(lata_result, 0x0102030405060708ull,
          "LDXR value before reservation-breaking STR");
    check(lata_result_x3, 1, "STXR failed status after intervening STR");
    check(stxr_failed_after_store.data[0], 0x5678,
          "failed STXR preserves intervening STR value");
    check(stxr_failed_after_store.data[1], 0xbbbbbbbbbbbbbbbbull,
          "failed STXR preserves adjacent word");
    check(arm_isar_has_aa64_lse(&env.arm_cpu.isar), 0,
          "LDXR/STXR execute without FEAT_LSE");

    test_aa64_lse = true;
    run_block(cas_success.code, 4, "cas-success");
    check(arm_isar_has_aa64_lse(&env.arm_cpu.isar), 1,
          "CAS fixture explicitly enables FEAT_LSE");
    check(lata_result, 0x1234, "CAS success returns old memory value");
    check(lata_result_x3, 0x5678, "CAS success preserves desired value");
    check(cas_success.data[0], 0x5678, "CAS success stores desired value");
    check(cas_success.data[1], 0xccccccccccccccccull,
          "CAS success preserves adjacent word");

    run_block(cas_failure.code, 4, "cas-failure");
    check(lata_result, 0x1234, "CAS failure returns actual memory value");
    check(lata_result_x3, 0x5678, "CAS failure preserves desired value");
    check(cas_failure.data[0], 0x1234, "CAS failure leaves memory unchanged");
    check(cas_failure.data[1], 0xddddddddddddddddull,
          "CAS failure preserves adjacent word");

    run_block(ldadd.code, 3, "ldadd");
    check(lata_result, 7, "LDADD preserves source addend");
    check(lata_result_x3, 0x1111222233334444ull,
          "LDADD returns old memory value");
    check(ldadd.data[0], 0x111122223333444bull,
          "LDADD atomically stores sum");
    check(ldadd.data[1], 0xeeeeeeeeeeeeeeeeull,
          "LDADD preserves adjacent word");

    run_block(ldadd_alias.code, 4, "ldadd-alias-rd-rs");
    check(lata_result, 0x1122334455667788ull,
          "aliased LDADD returns full old value");
    check(ldadd_alias.data[0], 0x112233455566778dull,
          "aliased LDADD stores full-width sum");
    check(ldadd_alias.data[1], 0xaaaaaaaaaaaaaaaaull,
          "aliased LDADD preserves adjacent word");
    test_aa64_lse = false;


    latx_bare_uart_puts("lata-isa: exclusive atomics done\n");
}

static bool append_sve_whilelo(DisasContext *s, uint8_t pd, uint8_t rn,
                               uint8_t rm, uint8_t element_bits,
                               uint8_t scalar_bits, const char *what)
{
    s->insn_type = AARCH64_A64_SVE_WHILELO;
    s->arg.f_sve_whilelo = (arg_sve_whilelo){
        .pd = pd,
        .rn = rn,
        .rm = rm,
        .element_bits = element_bits,
        .scalar_bits = scalar_bits,
        .effects = {.writes_nzcv = true},
    };
    bool ok = trans_sve_whilelo(s);
    check(ok, 1, what);
    return ok;
}

static void append_sve_nzcv_capture(uint64_t *results, unsigned index)
{
    la_armmfflag(a1_ir2_opnd, 0x39);
    li_d(a0_ir2_opnd, (int64_t)(uintptr_t)results);
    la_st_d(a1_ir2_opnd, a0_ir2_opnd, (int)(index * sizeof(*results)));
}


static void run_sve_integer_chunk_runtime(uint16_t vl_bits, unsigned zd,
                                          unsigned zm, unsigned pg,
                                          PvirOpcode opcode)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / 32u;
    size_t active_lanes = lanes / 2u + 1u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;
    bool subtract = opcode == PVIR_OP_SUB;
    bool multiply = opcode == PVIR_OP_MUL;

    check(arm_sve_set_vl(&env, vl_bits), 1,
          "SVE integer /M chunk VL is valid");
    check(arm_sve_active_vl_bytes(&env), vl_bytes,
          "SVE integer /M chunk state matches VL");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x6b;
        source[byte] = 0x39;
    }
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t old_value =
            UINT32_C(0x10000000) + ((uint32_t)vl_bits << 8) +
            (uint32_t)lane * 37u;
        uint32_t rhs =
            UINT32_C(0x01000000) + ((uint32_t)vl_bits << 4) +
            (uint32_t)lane * 101u;

        if (subtract && lane == 0) {
            old_value = 5;
            rhs = 9;
        } else if (multiply && lane == 0) {
            old_value = UINT32_C(0x80000001);
            rhs = 3;
        }
        for (unsigned byte = 0; byte < 4; ++byte) {
            destination[lane * 4u + byte] =
                (uint8_t)(old_value >> (byte * 8u));
            source[lane * 4u + byte] = (uint8_t)(rhs >> (byte * 8u));
        }
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, 32, 64,
                            "WHILELO integer chunk predicate lowers")) {
        tr_fini();
        return;
    }

    if (multiply) {
        s.insn_type = AARCH64_A64_SVE_MUL;
        s.arg.f_sve_mul = (arg_sve_mul){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_mul(&s);
    } else if (subtract) {
        s.insn_type = AARCH64_A64_SVE_SUB;
        s.arg.f_sve_sub = (arg_sve_sub){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_sub(&s);
    } else {
        s.insn_type = AARCH64_A64_SVE_ADD;
        s.arg.f_sve_add = (arg_sve_add){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_add(&s);
    }
    if (!lowered) {
        check(0, 1, multiply ? "SVE MUL /M chunk lowers"
                             : subtract ? "SVE SUB /M chunk lowers"
                                        : "SVE ADD /M chunk lowers");
        tr_fini();
        return;
    }
    check(1, 1, multiply ? "SVE MUL /M chunk lowers"
                         : subtract ? "SVE SUB /M chunk lowers"
                                    : "SVE ADD /M chunk lowers");

    latx_bare_uart_puts(multiply ? "sve-pvir: MUL /M chunk TB begin\n"
                                 : subtract ? "sve-pvir: SUB /M chunk TB begin\n"
                                            : "sve-pvir: ADD /M chunk TB begin\n");
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts(multiply ? "sve-pvir: MUL /M chunk TB done\n"
                                 : subtract ? "sve-pvir: SUB /M chunk TB done\n"
                                            : "sve-pvir: ADD /M chunk TB done\n");

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t old_value =
            UINT32_C(0x10000000) + ((uint32_t)vl_bits << 8) +
            (uint32_t)lane * 37u;
        uint32_t rhs =
            UINT32_C(0x01000000) + ((uint32_t)vl_bits << 4) +
            (uint32_t)lane * 101u;

        if (subtract && lane == 0) {
            old_value = 5;
            rhs = 9;
        } else if (multiply && lane == 0) {
            old_value = UINT32_C(0x80000001);
            rhs = 3;
        }
        uint32_t arithmetic =
            multiply ? old_value * rhs
                     : subtract ? old_value - rhs : old_value + rhs;
        uint32_t expected =
            lane < active_lanes ? arithmetic : old_value;
        size_t predicate_bit = lane * 4u;
        uint8_t predicate_value =
            (uint8_t)((arm_sve_preg(&env, pg)[predicate_bit / 8u] >>
                       (predicate_bit % 8u)) &
                      1u);

        check(read_le32(destination + lane * 4u), expected,
              "SVE integer /M chunk lane and merge result");
        check(predicate_value, lane < active_lanes,
              "SVE integer /M chunk predicate lane");
    }
    if (multiply) {
        check(read_le32(destination), UINT32_C(0x80000003),
              "SVE MUL /M active product wraps modulo 32 bits");
    }
    if (vl_bytes < ARM_SVE_MAX_VL_BYTES) {
        check(destination[vl_bytes], 0x6b,
              "SVE integer /M preserves first byte above VL");
        check(destination[ARM_SVE_MAX_VL_BYTES - 1u], 0x6b,
              "SVE integer /M preserves Z storage tail");
    }
}


static void run_sve_predicate_mask_cache_runtime(void)
{
    const size_t vl_bytes = ARM_SVE_MAX_VL_BYTES;
    const size_t lanes = vl_bytes / sizeof(uint32_t);
    const size_t first_active = 33;
    const size_t changed_active = 17;
    uint8_t *add_destination = arm_sve_zreg(&env, 24);
    uint8_t *add_source = arm_sve_zreg(&env, 25);
    uint8_t *sub_destination = arm_sve_zreg(&env, 26);
    uint8_t *sub_source = arm_sve_zreg(&env, 27);
    uint8_t *mul_destination = arm_sve_zreg(&env, 28);
    uint8_t *mul_source = arm_sve_zreg(&env, 29);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, ARM_SVE_MAX_VL_BITS), 1,
          "SVE predicate-cache fixture sets maximum VL");
    check(arm_sve_active_vl_bytes(&env), vl_bytes,
          "SVE predicate-cache fixture uses full mask storage");
    env.xregs[13] = 0;
    env.xregs[14] = first_active;
    env.xregs[15] = changed_active;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t add_old = UINT32_C(0x1000) + (uint32_t)lane * 3u;
        uint32_t add_rhs = UINT32_C(0x200) + (uint32_t)lane * 11u;
        uint32_t sub_old = UINT32_C(0x10000) + (uint32_t)lane * 29u;
        uint32_t sub_rhs = 100u + (uint32_t)lane * 2u;
        uint32_t mul_old = UINT32_C(0x10000) + (uint32_t)lane * 13u;
        uint32_t mul_rhs = 5u + (uint32_t)lane * 2u;

        if (lane == 0) {
            mul_old = UINT32_C(0x80000001);
            mul_rhs = 3;
        }
        write_le32(add_destination + lane * 4u, add_old);
        write_le32(add_source + lane * 4u, add_rhs);
        write_le32(sub_destination + lane * 4u, sub_old);
        write_le32(sub_source + lane * 4u, sub_rhs);
        write_le32(mul_destination + lane * 4u, mul_old);
        write_le32(mul_source + lane * 4u, mul_rhs);
    }

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, 7, 13, 14, 32, 64,
                            "WHILELO initial cache predicate lowers")) {
        tr_fini();
        return;
    }

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 24, .pg = 7, .zm = 25, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "SVE cache first ADD lowers");
        tr_fini();
        return;
    }
    check(1, 1, "SVE cache first ADD lowers");

    s.insn_type = AARCH64_A64_SVE_SUB;
    s.arg.f_sve_sub = (arg_sve_sub){
        .zd = 26, .pg = 7, .zm = 27, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_sub(&s)) {
        check(0, 1, "SVE cache same-version SUB lowers");
        tr_fini();
        return;
    }
    check(1, 1, "SVE cache same-version SUB lowers");
    check(builder.expanded_mask_chunks_materialized, 8,
          "same-predicate operations materialize one mask per LASX chunk");
    check(builder.expanded_mask_chunks_reused, 8,
          "second operation loads all same-version mask chunks from scratch");

    if (!append_sve_whilelo(&s, 7, 13, 15, 32, 64,
                            "WHILELO changed-version cache predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = AARCH64_A64_SVE_MUL;
    s.arg.f_sve_mul = (arg_sve_mul){
        .zd = 28, .pg = 7, .zm = 29, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_mul(&s)) {
        check(0, 1, "SVE cache changed-version MUL lowers");
        tr_fini();
        return;
    }
    check(1, 1, "SVE cache changed-version MUL lowers");
    check(builder.expanded_mask_chunks_materialized, 16,
          "new predicate version forces per-chunk re-expansion");
    check(builder.expanded_mask_chunks_reused, 8,
          "changed predicate version does not reuse stale chunks");
    check(builder.expanded_mask_key.source.version,
          builder.p_versions[7],
          "cache key records the current predicate version");

    latx_bare_uart_puts("sve-pvir: predicate-mask cache TB begin\n");
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: predicate-mask cache TB done\n");

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t add_old = UINT32_C(0x1000) + (uint32_t)lane * 3u;
        uint32_t add_rhs = UINT32_C(0x200) + (uint32_t)lane * 11u;
        uint32_t sub_old = UINT32_C(0x10000) + (uint32_t)lane * 29u;
        uint32_t sub_rhs = 100u + (uint32_t)lane * 2u;
        uint32_t mul_old = UINT32_C(0x10000) + (uint32_t)lane * 13u;
        uint32_t mul_rhs = 5u + (uint32_t)lane * 2u;
        uint32_t expected_mul;
        size_t predicate_bit = lane * 4u;
        uint8_t predicate_value;

        if (lane == 0) {
            mul_old = UINT32_C(0x80000001);
            mul_rhs = 3;
        }
        expected_mul =
            lane < changed_active ? mul_old * mul_rhs : mul_old;
        predicate_value =
            (uint8_t)((arm_sve_preg(&env, 7)[predicate_bit / 8u] >>
                       (predicate_bit % 8u)) &
                      1u);
        check(read_le32(add_destination + lane * 4u),
              lane < first_active ? add_old + add_rhs : add_old,
              "same-version ADD lane and merge result");
        check(read_le32(sub_destination + lane * 4u),
              lane < first_active ? sub_old - sub_rhs : sub_old,
              "same-version SUB lane and merge result");
        check(read_le32(mul_destination + lane * 4u), expected_mul,
              "changed-version MUL lane and merge result");
        check(predicate_value, lane < changed_active,
              "changed-version packed predicate lane");
    }
    check(read_le32(mul_destination), UINT32_C(0x80000003),
          "cache fixture MUL wraps modulo 32 bits");
    for (size_t byte = 0; byte < vl_bytes; ++byte) {
        uint8_t expected_mask =
            byte < changed_active * sizeof(uint32_t) ? UINT8_MAX : 0;

        check(env.sve_expanded_mask_scratch[byte], expected_mask,
              "scratch holds expanded mask for latest predicate version");
    }
}

static void run_sve_sparse_memory_runtime(void)
{
    const uint32_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    volatile uint8_t *sparse_ram =
        (volatile uint8_t *)(uintptr_t)UINT64_C(0x0ffffff8);
    uint8_t *loaded = arm_sve_zreg(&env, 9);
    uint8_t *store_source = arm_sve_zreg(&env, 10);
    uint8_t *predicate = arm_sve_preg(&env, 3);
    A64SVEIR1Instruction decoded_load;
    A64SVEIR1Instruction decoded_store;
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    /* Lane 8 is the first unmapped byte; active lanes are 0, 2, and 7. */
    check(arm_sve_set_vl(&env, vl_bits), 1,
          "sparse SVE memory fixture sets 256-bit VL");
    for (size_t i = 0; i < 8; ++i) {
        sparse_ram[i] = (uint8_t)(0x30u + i);
    }
    for (size_t i = 0; i < vl_bytes; ++i) {
        loaded[i] = 0xcc;
        store_source[i] = (uint8_t)(0xa0u + i);
    }
    for (size_t i = 0; i < ARM_SVE_MAX_PRED_BYTES; ++i) {
        predicate[i] = i < vl_bytes / 8u ? 0 : UINT8_MAX;
    }
    predicate[0] = 0x85;
    env.xregs[26] = (uintptr_t)sparse_ram;

    if (a64_sve_decode(0xa400af49u, &decoded_load) !=
            A64_SVE_DECODE_MATCH ||
        decoded_load.insn_type != AARCH64_A64_SVE_LD1B) {
        check(0, 1, "sparse LD1B encoding decodes");
        return;
    }
    check(1, 1, "sparse LD1B encoding decodes");
    if (a64_sve_decode(0xe400ef4au, &decoded_store) !=
            A64_SVE_DECODE_MATCH ||
        decoded_store.insn_type != AARCH64_A64_SVE_ST1B) {
        check(0, 1, "sparse ST1B encoding decodes");
        return;
    }
    check(1, 1, "sparse ST1B encoding decodes");

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);

    s.insn_type = decoded_load.insn_type;
    s.arg = decoded_load.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "sparse LD1B lowers");
        tr_fini();
        return;
    }
    check(1, 1, "sparse LD1B lowers");
    s.insn_type = decoded_store.insn_type;
    s.arg = decoded_store.arg;
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "sparse ST1B lowers");
        tr_fini();
        return;
    }
    check(1, 1, "sparse ST1B lowers");

    latx_bare_uart_puts("sve-pvir: sparse memory TB begin\n");
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: sparse memory TB done\n");

    check(predicate[0], 0x85, "sparse predicate remains unchanged");
    check(1, 1, "sparse LD1B/ST1B skip inactive unmapped lane 8");
    for (size_t i = 0; i < vl_bytes; ++i) {
        bool active = i == 0 || i == 2 || i == 7;
        uint8_t expected_load = active ? (uint8_t)(0x30u + i) : 0;

        check(loaded[i], expected_load,
              "sparse LD1B result is active bytes plus zero lanes");
    }
    for (size_t i = 0; i < 8; ++i) {
        bool active = i == 0 || i == 2 || i == 7;
        uint8_t expected_store =
            active ? (uint8_t)(0xa0u + i) : (uint8_t)(0x30u + i);

        check(sparse_ram[i], expected_store,
              active ? "sparse ST1B writes active lane"
                     : "sparse ST1B preserves inactive memory");
    }
}


static void run_sve_pvir_lasx_codegen(void)
{
    const size_t vl_bytes = ARM_SVE_MAX_VL_BYTES;
    uint8_t *z0;
    uint8_t *z1;
    uint8_t *z2;
    uint8_t *z3;
    uint8_t *z4;
    uint8_t *z5;
    uint8_t *z7;
    uint8_t *z8;
    volatile uint8_t *ram_edge;
    A64SVEIR1Instruction decoded_memory;
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    static uint64_t whilelo_flags[4];
    static volatile uint8_t sve_memory_input[ARM_SVE_MAX_VL_BYTES];
    static volatile uint8_t sve_memory_output[ARM_SVE_MAX_VL_BYTES];

    for (size_t i = 0; i < sizeof(env); ++i) {
        ((uint8_t *)&env)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(cpu); ++i) {
        ((uint8_t *)&cpu)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(tb); ++i) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    lata_tr_data_init(&env);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    check(arm_sve_set_vl(&env, ARM_SVE_MAX_VL_BITS), 1,
          "SVE PVIR fixture sets maximum VL");
    for (size_t i = 0; i < vl_bytes; ++i) {
        sve_memory_input[i] = (uint8_t)(i * 13u + 3u);
        sve_memory_output[i] = 0xa5;
    }

    z0 = arm_sve_zreg(&env, 0);
    z1 = arm_sve_zreg(&env, 1);
    z2 = arm_sve_zreg(&env, 2);
    z3 = arm_sve_zreg(&env, 3);
    z4 = arm_sve_zreg(&env, 4);
    z5 = arm_sve_zreg(&env, 5);
    z7 = arm_sve_zreg(&env, 7);
    z8 = arm_sve_zreg(&env, 8);
    for (size_t i = 0; i < vl_bytes; ++i) {
        z0[i] = (uint8_t)(i * 3u + 1u);
        z1[i] = 7;
        z4[i] = (uint8_t)(0xa0u + i);
        z5[i] = (uint8_t)(i * 5u + 9u);
        z8[i] = 0x44;
    }
    for (size_t lane = 0; lane < vl_bytes / 4; ++lane) {
        uint32_t lhs = 0x1000u + (uint32_t)lane;
        uint32_t rhs = 0x10u + (uint32_t)lane;
        for (unsigned byte = 0; byte < 4; ++byte) {
            z2[lane * 4 + byte] = (uint8_t)(lhs >> (byte * 8));
            z3[lane * 4 + byte] = (uint8_t)(rhs >> (byte * 8));
        }
    }

    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);

    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 0, .element_bits = 8, .pattern = 31,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, "PTRUE all lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "PTRUE all lowers to LASX");

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 0, .pg = 0, .zm = 1, .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD /M lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "ADD /M lowers to LASX");

    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 1, .element_bits = 32, .pattern = 4,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, "PTRUE VL4 lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "PTRUE VL4 lowers to LASX");

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 2, .pg = 1, .zm = 3, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD /M VL4 lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "ADD /M VL4 lowers to LASX");
    env.xregs[11] = 0;
    env.xregs[12] = vl_bytes;
    env.xregs[13] = 0;
    env.xregs[14] = 4;
    env.xregs[15] = 5;
    env.xregs[16] = 5;
    env.xregs[17] = 7;
    env.xregs[18] = 6;
    for (size_t i = 0; i < sizeof(whilelo_flags) / sizeof(whilelo_flags[0]);
         ++i) {
        whilelo_flags[i] = 0;
    }
    if (!append_sve_whilelo(&s, 3, 11, 12, 8, 32,
                            "WHILELO all-active codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 0);
    if (!append_sve_whilelo(&s, 4, 13, 14, 16, 64,
                            "WHILELO prefix codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 1);
    if (!append_sve_whilelo(&s, 5, 15, 16, 32, 32,
                            "WHILELO none-active codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 2);
    if (!append_sve_whilelo(&s, 6, 17, 18, 64, 64,
                            "WHILELO start>=limit codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 3);

    s.insn_type = AARCH64_A64_SVE_ADD;

    /* P2 is the zeroed entry-state predicate: inactive lanes must merge. */
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 4, .pg = 2, .zm = 5, .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD /M all-false lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "ADD /M all-false lowers to LASX");

    latx_bare_uart_puts("sve-pvir: arithmetic emit done\n");
    latx_bare_uart_puts("sve-pvir: arithmetic assemble begin\n");
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    latx_bare_uart_puts("sve-pvir: arithmetic assemble done\n");
    __asm__ volatile("ibar 0" ::: "memory");
    enable_sxe_asxe_only();
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: arithmetic TB done\n");
    run_sve_integer_chunk_runtime(256, 10, 11, 7, PVIR_OP_ADD);
    run_sve_integer_chunk_runtime(512, 12, 13, 7, PVIR_OP_ADD);
    run_sve_integer_chunk_runtime(1024, 14, 15, 7, PVIR_OP_ADD);
    run_sve_integer_chunk_runtime(256, 16, 17, 7, PVIR_OP_SUB);
    run_sve_integer_chunk_runtime(1024, 18, 19, 7, PVIR_OP_SUB);
    run_sve_integer_chunk_runtime(256, 20, 21, 7, PVIR_OP_MUL);
    run_sve_integer_chunk_runtime(1024, 22, 23, 7, PVIR_OP_MUL);
    run_sve_predicate_mask_cache_runtime();

    run_sve_sparse_memory_runtime();



    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    tr_init(&tb);
    s.vl = 32;
    check(arm_sve_set_vl(&env, 256), 1,
          "SVE memory fixture sets 256-bit VL");
    for (size_t i = 0; i < vl_bytes; ++i) {
        arm_sve_zreg(&env, 6)[i] = 0x88;
    }
    a64_sve_pvir_builder_init(&builder);
    latx_bare_uart_puts("sve-pvir: memory translator ready\n");
    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 3, .element_bits = 8, .pattern = 31,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, "PTRUE all initializes memory predicate");
        tr_fini();
        return;
    }
    check(1, 1, "PTRUE all initializes memory predicate");
    latx_bare_uart_puts("sve-pvir: memory PTRUE done\n");
    for (size_t i = 0; i < ARM_SVE_MAX_PRED_BYTES; ++i) {
        arm_sve_ffr(&env)[i] = 0x66;
        arm_sve_preg(&env, 7)[i] = 0x44;
    }
    if (a64_sve_decode(0x252c9000u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_SETFFR) {
        check(0, 1, "SETFFR encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "SETFFR encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_setffr(&s)) {
        check(0, 1, "SETFFR lowers to FFR all-true state");
        tr_fini();
        return;
    }
    check(1, 1, "SETFFR lowers to FFR all-true state");
    if (a64_sve_decode(0x2519f007u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_RDFFR) {
        check(0, 1, "RDFFR encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "RDFFR encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_rdffr(&s)) {
        check(0, 1, "RDFFR copies FFR into packed P7");
        tr_fini();
        return;
    }
    check(1, 1, "RDFFR copies FFR into packed P7");
    latx_bare_uart_puts("sve-pvir: FFR operations lowered\n");
    /* An all-false predicate must suppress accesses to this invalid base. */
    env.xregs[27] = UINT64_C(0x1ffffffd);
    env.xregs[13] = 1;
    env.xregs[14] = 0;
    if (!append_sve_whilelo(&s, 2, 13, 14, 8, 32,
                            "WHILELO all-false invalid-base predicate")) {
        tr_fini();
        return;
    }
    if (a64_sve_decode(0xa400ab66u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_LD1B) {
        check(0, 1, "LD1B invalid-base encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B invalid-base encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "LD1B all-false invalid-base lowering");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B all-false invalid-base lowering");
    latx_bare_uart_puts("sve-pvir: invalid-base inactive load lowered\n");

    /*
     * QEMU virt with 512 MiB has RAM at [0, 0x10000000), followed by an
     * unmapped gap. Keep the three active bytes at the very end of RAM.
     */
    ram_edge = (volatile uint8_t *)(uintptr_t)UINT64_C(0x0ffffffd);
    env.xregs[25] = (uintptr_t)ram_edge;
    env.xregs[11] = 0;
    env.xregs[12] = 3;
    for (unsigned lane = 0; lane < 3; ++lane) {
        ram_edge[lane] = (uint8_t)(0x5a + lane);
    }
    latx_bare_uart_puts("sve-pvir: QEMU RAM edge seeded\n");
    if (!append_sve_whilelo(&s, 0, 11, 12, 8, 32,
                            "WHILELO boundary-prefix memory predicate")) {
        tr_fini();
        return;
    }
    latx_bare_uart_puts("sve-pvir: memory WHILELO done\n");
    if (a64_sve_decode(0xa400a327u, &decoded_memory) !=
        A64_SVE_DECODE_MATCH) {
        check(0, 1, "LD1B boundary encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B boundary encoding decodes");
    if (decoded_memory.insn_type != AARCH64_A64_SVE_LD1B) {
        tr_fini();
        return;
    }
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "LD1B staged per-lane load lowers");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B staged per-lane load lowers");
    latx_bare_uart_puts("sve-pvir: boundary LD1B lowered\n");

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 8, .pg = 0, .zm = 7, .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD consumes staged LD1B result");
        tr_fini();
        return;
    }
    check(1, 1, "ADD consumes staged LD1B result");

    if (a64_sve_decode(0xe400e327u, &decoded_memory) !=
        A64_SVE_DECODE_MATCH) {
        check(0, 1, "ST1B boundary encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B boundary encoding decodes");
    if (decoded_memory.insn_type != AARCH64_A64_SVE_ST1B) {
        tr_fini();
        return;
    }
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "ST1B active-lane store lowers");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B active-lane store lowers");
    latx_bare_uart_puts("sve-pvir: boundary ST1B lowered\n");

    /*
     * Exercise every active lane with distinct bytes so the scalar guest
     * accesses and ascending stores cover the complete contiguous range.
     */
    env.xregs[26] = (uintptr_t)sve_memory_input;
    env.xregs[18] = (uintptr_t)sve_memory_output;
    if (a64_sve_decode(0xa400af49u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_LD1B) {
        check(0, 1, "LD1B contiguous active-lane encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B contiguous active-lane encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "LD1B lowers for every active lane");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B lowers for every active lane");
    latx_bare_uart_puts("sve-pvir: contiguous LD1B lowered\n");

    if (a64_sve_decode(0xe400ee49u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_ST1B) {
        check(0, 1, "ST1B contiguous active-lane encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B contiguous active-lane encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "ST1B lowers for every active lane");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B lowers for every active lane");
    latx_bare_uart_puts("sve-pvir: contiguous ST1B lowered\n");

    latx_bare_uart_puts("sve-pvir: memory emit done\n");
    latx_bare_uart_puts("sve-pvir: memory assemble begin\n");
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    latx_bare_uart_puts("sve-pvir: memory assemble done\n");
    __asm__ volatile("ibar 0" ::: "memory");
    enable_sxe_asxe_only();
    latx_bare_uart_puts("sve-pvir: memory TB begin\n");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: memory TB done\n");
    for (size_t i = 0; i < ARM_SVE_MAX_PRED_BYTES; ++i) {
        check(arm_sve_ffr(&env)[i], i < 4 ? UINT8_MAX : 0x66,
              "SETFFR fills active VL bits and preserves FFR tail");
        check(arm_sve_preg(&env, 7)[i], i < 4 ? UINT8_MAX : 0x44,
              "RDFFR copies active packed bits and preserves P tail");
    }
    for (size_t i = 0; i < vl_bytes; ++i) {
        check(arm_sve_zreg(&env, 0)[i],
              (uint8_t)(i * 3u + 1u + 7u), "PTRUE all + ADD /M byte");
        check(arm_sve_zreg(&env, 4)[i], (uint8_t)(0xa0u + i),
              "ADD /M all-false preserves destination");
    }
    for (size_t lane = 0; lane < vl_bytes / 4; ++lane) {
        uint32_t want = 0x1000u + (uint32_t)lane;
        if (lane < 4) {
            want += 0x10u + (uint32_t)lane;
        }
        check(read_le32(arm_sve_zreg(&env, 2) + lane * 4), want,
              "PTRUE VL4 + ADD /M lane");
    }
    for (size_t i = 0; i < vl_bytes / 8; ++i) {
        check(arm_sve_preg(&env, 3)[i], UINT8_MAX,
              "WHILELO all-active packed predicate");
        check(arm_sve_preg(&env, 4)[i], i == 0 ? 0x55 : 0,
              "WHILELO prefix packed predicate");
        check(arm_sve_preg(&env, 5)[i], 0,
              "WHILELO none-active packed predicate");
        check(arm_sve_preg(&env, 6)[i], 0,
              "WHILELO start>=limit packed predicate");
        check(arm_sve_preg(&env, 2)[i], 0,
              "WHILELO all-false invalid-base predicate");
    }
    check((uint32_t)whilelo_flags[0], UINT32_C(0x80000000),
          "WHILELO all-active NZCV");
    check((uint32_t)whilelo_flags[1], UINT32_C(0xa0000000),
          "WHILELO prefix NZCV");
    check(whilelo_flags[2], UINT64_C(0x60000000),
          "WHILELO none-active NZCV");
    check(whilelo_flags[3], UINT64_C(0x60000000),
          "WHILELO start>=limit NZCV");
    for (size_t i = 0; i < vl_bytes; ++i) {
        check(z7[i], i < 3 ? (uint8_t)(0x5a + i) : 0,
              "LD1B loads active prefix and zeroes inactive lanes");
        check(z8[i], i < 3 ? (uint8_t)(0x9e + i) : 0x44,
              "ADD consumes LD1B result with merge policy");
        check(arm_sve_zreg(&env, 6)[i], i < 32 ? 0 : 0x88,
              "LD1B zeroes all-false result without invalid-base access");
    }
    for (size_t i = 0; i < vl_bytes / 8; ++i) {
        check(arm_sve_preg(&env, 0)[i],
              i == 0 ? 0x07 : i < 4 ? 0 : UINT8_MAX,
              "WHILELO activates mapped boundary prefix");
    }
    for (unsigned lane = 0; lane < 3; ++lane) {
        check(ram_edge[lane], (uint8_t)(0x5a + lane),
              "ST1B writes active bytes before unmapped tail");
    }
    for (size_t i = 0; i < vl_bytes; ++i) {
        check(arm_sve_zreg(&env, 9)[i],
              i < 32 ? sve_memory_input[i] : 0,
              "LD1B loads every active contiguous byte lane");
        check(sve_memory_output[i],
              i < 32 ? sve_memory_input[i] : 0xa5,
              "ST1B writes every active contiguous byte lane");
    }
}


void kmain(void)
{
    latx_bare_uart_puts("lata-isa: start\n");

    /*
     * Every block ends with its result in x2.
     *
     * The environment is not the observation point here: this harness (like
     * lata_smoke) enters the translated block directly and leaves through a
     * trampoline, so no epilogue writes the register file back -- env.xregs
     * stays zero for a block that computed the right answer.
     *
     * x2 is where the result has to be: the AArch64 register map allocates
     * x2 to $a2 (target/arm/lata/lata.c), which is the register the
     * trampoline captures.  x0 is $ra and would be clobbered by the call.
     */

    /* --- integer add ------------------------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(16))) = {
            0xd28000a1u,  /* movz x1, #5       */
            0x91001c22u,  /* add  x2, x1, #7   */
        };
        run_block(g, 2, "add");
        check(lata_result, 12, "add x2, x1, #7");
    }

    /* --- NZCV: SUBS then CSET ---------------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(16))) = {
            0xd28000a1u,  /* movz x1, #5       */
            0x91001c22u,  /* add  x2, x1, #7   */
            0xf1003043u,  /* subs x3, x2, #12  */
            0x9a9f17e2u,  /* cset x2, eq       */
        };
        run_block(g, 4, "nzcv");
        check(lata_result, 1, "cset x2, eq after subs giving zero");
    }

    /* --- 32-bit operand clears the upper half ------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(16))) = {
            0xd28000a1u,  /* movz x1, #5         */
            0x12001c22u,  /* and  w2, w1, #0xff   */
        };
        run_block(g, 2, "w-form");
        check(lata_result, 5, "and w2, w1, #0xff");
    }

    /* --- shift -------------------------------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(16))) = {
            0xd28000a1u,  /* movz x1, #5      */
            0xd37cec22u,  /* lsl  x2, x1, #4  */
            0xd342fc42u,  /* lsr  x2, x2, #2  */
        };
        run_block(g, 3, "shift");
        check(lata_result, 0x14, "lsl then lsr (5<<4)>>2");
    }

    /* --- MOVK writes one half only ----------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(16))) = {
            0xd2800002u,  /* movz x2, #0                  */
            0xf2a24682u,  /* movk x2, #0x1234, lsl #16    */
        };
        /*
         * MOVK writes one 16-bit field and leaves the rest of the register
         * alone, so the case has to establish the rest first -- otherwise the
         * expected value depends on whatever the previous block left behind,
         * and the test would be measuring block ordering rather than MOVK.
         */
        run_block(g, 2, "movk");
        check(lata_result, 0x12340000ull, "movk x2, #0x1234, lsl #16");
    }

    /* --- carry out of ADDS ------------------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(16))) = {
            0xd29fffe1u,  /* mov  x1, #0xffff            */
            0xf2bfffe1u,  /* movk x1, #0xffff, lsl #16   */
            0xf2dfffe1u,  /* movk x1, #0xffff, lsl #32   */
            0xf2ffffe1u,  /* movk x1, #0xffff, lsl #48   */
            0xab010021u,  /* adds x1, x1, x1             */
            0x9a9f37e2u,  /* cset x2, hs                 */
        };
        /*
         * x1 must be all ones before ADDS can carry: 0xffffffffffff doubled
         * fits in 64 bits and sets no carry, so a shorter literal would make
         * this case assert the wrong thing about the translator.
         */
        run_block(g, 6, "carry");
        check(lata_result, 1, "cset x2, hs after ADDS carry");
    }

    run_integer_nzcv_matrix();
    run_scalar_memory_matrix();
    run_exclusive_atomic_matrix();

    /* --- A64 NEON integer add through the LSX-only runtime path -------- */
    {
        static uint32_t neon_low[] __attribute__((aligned(16))) = {
            0x528000a1u,  /* movz w1, #5           */
            0x528000e3u,  /* movz w3, #7           */
            0x4e040c20u,  /* dup v0.4s, w1         */
            0x4e040c61u,  /* dup v1.4s, w3         */
            0x4ea18402u,  /* add v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0]      */
        };
        static uint32_t neon_high[] __attribute__((aligned(16))) = {
            0x528000a1u,  /* movz w1, #5           */
            0x528000e3u,  /* movz w3, #7           */
            0x4e040c20u,  /* dup v0.4s, w1         */
            0x4e040c61u,  /* dup v1.4s, w3         */
            0x4ea18402u,  /* add v2.4s, v0.4s, v1.4s */
            0x4e183c42u,  /* umov x2, v2.d[1]      */
        };
        const uint64_t expected_pair = 0x0000000c0000000cull;

        check(latx_cpu_has_lsx(), 1, "LSX feature API reports LSX");
        if (latx_cpu_has_lasx()) {
            latx_bare_uart_puts("lata-isa: NEON host reports LASX available\n");
        } else {
            latx_bare_uart_puts("lata-isa: NEON host is LSX-only\n");
        }
        enable_sxe_only();
        latx_bare_uart_puts("lata-isa: neon-lsx begin\n");
        run_block(neon_low, 6, "neon-lsx-low");
        check(lata_result, expected_pair, "NEON ADD lanes 0-1");
        run_block(neon_high, 6, "neon-lsx-high");
        check(lata_result, expected_pair, "NEON ADD lanes 2-3");
        latx_bare_uart_puts("lata-isa: neon-lsx done\n");
    }
    /* Integer vector logical operations, consumed through architectural UMOV. */
    {
        static uint32_t neon_logic_low[] __attribute__((aligned(16))) = {
            0x52800aa1u,  /* movz w1, #0x55 */
            0x528001e3u,  /* movz w3, #0x0f */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x4e211c02u,  /* and v2.16b, v0.16b, v1.16b */
            0x4ea11c03u,  /* orr v3.16b, v0.16b, v1.16b */
            0x6e231c44u,  /* eor v4.16b, v2.16b, v3.16b */
            0x4e083c82u,  /* umov x2, v4.d[0] */
        };
        static uint32_t neon_logic_high[] __attribute__((aligned(16))) = {
            0x52800aa1u,  /* movz w1, #0x55 */
            0x528001e3u,  /* movz w3, #0x0f */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x4e211c02u,  /* and v2.16b, v0.16b, v1.16b */
            0x4ea11c03u,  /* orr v3.16b, v0.16b, v1.16b */
            0x6e231c44u,  /* eor v4.16b, v2.16b, v3.16b */
            0x4e183c82u,  /* umov x2, v4.d[1] */
        };
        const uint64_t expected_pair = 0x0000005a0000005aull;

        latx_bare_uart_puts("lata-isa: neon-lsx logic begin\n");
        run_block(neon_logic_low, 8, "neon-lsx-logic-low");
        check(lata_result, expected_pair, "NEON AND/ORR/EOR lanes 0-1");
        run_block(neon_logic_high, 8, "neon-lsx-logic-high");
        check(lata_result, expected_pair, "NEON AND/ORR/EOR lanes 2-3");
        latx_bare_uart_puts("lata-isa: neon-lsx logic done\n");
    }
    /* Integer comparison results remain visible after both UMOV lane reads. */
    {
        static uint32_t neon_compare_equal[] __attribute__((aligned(16))) = {
            0x528000a1u,  /* movz w1, #5 */
            0x528000a3u,  /* movz w3, #5 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x6ea18c02u,  /* cmeq v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
            0x8a030042u,  /* and x2, x2, x3 */
        };
        static uint32_t neon_compare_unequal[] __attribute__((aligned(16))) = {
            0x528000a1u,  /* movz w1, #5 */
            0x528000e3u,  /* movz w3, #7 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x6ea18c02u,  /* cmeq v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
            0xaa030042u,  /* orr x2, x2, x3 */
        };

        latx_bare_uart_puts("lata-isa: neon-lsx compare begin\n");
        run_block(neon_compare_equal, 8, "neon-lsx-cmeq-equal");
        check(lata_result, UINT64_MAX,
              "NEON CMEQ equal lanes through both UMOV halves");
        run_block(neon_compare_unequal, 8, "neon-lsx-cmeq-unequal");
        check(lata_result, 0,
              "NEON CMEQ unequal lanes through both UMOV halves");
        latx_bare_uart_puts("lata-isa: neon-lsx compare done\n");
    }
    /* Cross-checked with clang --target=aarch64-none-elf / llvm-objdump. */
    {
        static uint32_t neon_sub[] __attribute__((aligned(16))) = {
            0x52800221u,  /* movz w1, #17 */
            0x528000a3u,  /* movz w3, #5 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x6ea18402u,  /* sub v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
        };
        static uint32_t neon_mul[] __attribute__((aligned(16))) = {
            0x52800221u,  /* movz w1, #17 */
            0x528000a3u,  /* movz w3, #5 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x4ea19c02u,  /* mul v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
        };
        static uint32_t neon_shl[] __attribute__((aligned(16))) = {
            0x52800221u,  /* movz w1, #17 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4f225402u,  /* shl v2.4s, v0.4s, #2 */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
        };
        const uint64_t sub_pair = 0x0000000c0000000cull;
        const uint64_t mul_pair = 0x0000005500000055ull;
        const uint64_t shl_pair = 0x0000004400000044ull;

        latx_bare_uart_puts("lata-isa: neon-lsx arithmetic begin\n");
        run_block(neon_sub, 7, "neon-lsx-sub");
        check(lata_result, sub_pair, "NEON SUB lanes 0-1");
        check(lata_result_x3, sub_pair, "NEON SUB lanes 2-3");
        run_block(neon_mul, 7, "neon-lsx-mul");
        check(lata_result, mul_pair, "NEON MUL lanes 0-1");
        check(lata_result_x3, mul_pair, "NEON MUL lanes 2-3");
        run_block(neon_shl, 5, "neon-lsx-shl-immediate");
        check(lata_result, shl_pair, "NEON SHL lanes 0-1");
        check(lata_result_x3, shl_pair, "NEON SHL lanes 2-3");
        latx_bare_uart_puts("lata-isa: neon-lsx arithmetic done\n");
    }
    /* Q-register loads/stores use the actual A64 vector memory handlers. */
    {
        static struct __attribute__((aligned(16))) {
            uint32_t code[4];
            uint64_t data[6];
        } neon_q_mem = {
            .code = {
                0x10000081u,  /* adr x1, data */
                0x3dc00020u,  /* ldr q0, [x1] */
                0x3d800420u,  /* str q0, [x1, #16] */
                0xd503201fu,  /* nop */
            },
            .data = {
                0x0123456789abcdefull, 0xfedcba9876543210ull,
                0xcafebabedeadbeefull, 0xfeedface01234567ull,
                0xaaaaaaaa55555555ull, 0x1122334455667788ull,
            },
        };

        check((uintptr_t)neon_q_mem.data - (uintptr_t)neon_q_mem.code,
              sizeof(neon_q_mem.code), "NEON Q LDR/STR ADR layout");
        check((uintptr_t)neon_q_mem.data & 15u, 0,
              "NEON Q LDR/STR data is 16-byte aligned");
        latx_bare_uart_puts("lata-isa: neon-q-ldr-str begin\n");
        run_block(neon_q_mem.code, 4, "neon-q-ldr-str");
        check(neon_q_mem.data[0], 0x0123456789abcdefull,
              "Q LDR/STR preserves source low 64 bits");
        check(neon_q_mem.data[1], 0xfedcba9876543210ull,
              "Q LDR/STR preserves source high 64 bits");
        check(neon_q_mem.data[2], 0x0123456789abcdefull,
              "Q STR writes low 64 bits");
        check(neon_q_mem.data[3], 0xfedcba9876543210ull,
              "Q STR writes high 64 bits");
        check(neon_q_mem.data[4], 0xaaaaaaaa55555555ull,
              "Q STR preserves adjacent guard");
        check(neon_q_mem.data[5], 0x1122334455667788ull,
              "Q STR preserves following guard");
        latx_bare_uart_puts("lata-isa: neon-q-ldr-str done\n");
    }


    latx_bare_uart_puts("lata-isa: context begin\n");
    run_context_vector_save_restore();
    latx_bare_uart_puts("lata-isa: context done\n");
    if (latx_cpu_has_lasx()) {
        latx_bare_uart_puts("sve-pvir: codegen begin\n");
        run_sve_pvir_lasx_codegen();
        latx_bare_uart_puts("sve-pvir: codegen done\n");
    } else {
        latx_bare_uart_puts("lata-isa: LSX-only run; skip LASX tests\n");
    }


    latx_bare_uart_puts("lata-isa: checks=");
    putdec((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    putdec((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts(fails ? "lata-isa: FAIL\n" : "lata-isa: PASS\n");
    latx_bare_uart_puts("lata-isa: done\n");
}
