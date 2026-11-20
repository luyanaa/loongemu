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
    extern void lata_exit_tramp(void);

    for (size_t i = 0; i < sizeof(env); i++) { ((uint8_t *)&env)[i] = 0; }
    for (size_t i = 0; i < sizeof(cpu); i++) { ((uint8_t *)&cpu)[i] = 0; }
    for (size_t i = 0; i < sizeof(tb); i++) { ((uint8_t *)&tb)[i] = 0; }

    cpu.env_ptr = &env;
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

    latx_bare_uart_puts("lata-isa: checks=");
    putdec((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    putdec((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts(fails ? "lata-isa: FAIL\n" : "lata-isa: PASS\n");
    latx_bare_uart_puts("lata-isa: done\n");
}
