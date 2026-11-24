/* Bare-metal smoke test: translate two AArch64 instructions with LATA,
 * assemble the IR2 to LoongArch machine code, and execute it on the CPU.
 * Avoids the LATA prologue/epilogue (which use LBT_ARM/FCSR opcodes that
 * QEMU does not implement) by entering the TB directly and returning from
 * the TB exit branch through a plain trampoline. */
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

volatile uint64_t lata_result;
uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];

/* movz x1, #5 ; add x2, x1, #7  -> x2 == 12 */
static uint32_t guest_code[2] __attribute__((aligned(16))) = { 0xd28000a1u, 0x91001c22u };
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
void lata_exit_tramp(void);

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

/* Strong override of the weak guest-fetch hook: identity-mapped code. */
uint32_t lata_guest_ldl(uint64_t addr)
{
    return *(const uint32_t *)(uintptr_t)addr;
}

/* Minimal qsort for the TU path. */
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
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(kstack)\n  addi.d $sp, $sp, %pc_lo12(kstack)\n  lu12i.w $t0, 16\n  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");

static char kstack[65536] __attribute__((aligned(16), used));
FILE *stderr;

void kmain(void)
{
    latx_bare_uart_puts("lata-smoke: start\n");

    for (size_t i = 0; i < sizeof(env); i++) { ((uint8_t *)&env)[i] = 0; }
    for (size_t i = 0; i < sizeof(cpu); i++) { ((uint8_t *)&cpu)[i] = 0; }
    for (size_t i = 0; i < sizeof(tb); i++) { ((uint8_t *)&tb)[i] = 0; }

    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)guest_code;

    lata_tr_data_init(&env);
    context_switch_native_to_bt = (uint64_t)(uintptr_t)lata_exit_tramp;
    context_switch_native_to_bt_ret_0 = (uint64_t)(uintptr_t)lata_exit_tramp;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;

    tb.pc = (uint64_t)(uintptr_t)guest_code;
    tb.tc.ptr = code_buf;
    tb.cflags = 0;

    int max_insns = 2;
    target_disasm(&tb, &max_insns, &cpu);
    latx_bare_uart_puts("lata-smoke: decoded insns=");
    puthex(tb.icount);
    latx_bare_uart_puts(" size=");
    puthex(tb.size);
    latx_bare_uart_puts("\n");

    tr_init(&tb);
    bool ok = tr_ir2_generate(&tb);
    int n = tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");

    latx_bare_uart_puts("lata-smoke: ir2_ok=");
    puthex(ok ? 1 : 0);
    latx_bare_uart_puts(" la_insns=");
    puthex((uint64_t)n);
    latx_bare_uart_puts(" code0=");
    puthex(((uint32_t *)code_buf)[0]);
    latx_bare_uart_puts(" code1=");
    puthex(((uint32_t *)code_buf)[1]);
    latx_bare_uart_puts("\n");

    latx_bare_uart_puts("lata-smoke: run\n");
    lata_enter_tb(&env, code_buf);

    latx_bare_uart_puts("lata-smoke: x2=");
    puthex(lata_result);
    latx_bare_uart_puts(" env.x2=");
    puthex(env.xregs[2]);
    latx_bare_uart_puts(" pc=");
    puthex(env.pc);
    latx_bare_uart_puts("\n");

    latx_bare_uart_puts((lata_result == 12) ?
        "lata-smoke: PASS\n" : "lata-smoke: FAIL\n");
    latx_bare_uart_puts("lata-smoke: done\n");
}
