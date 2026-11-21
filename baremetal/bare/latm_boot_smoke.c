/*
 * Minimal 3A4000 boot smoke:
 *   ELF load -> first translated TB -> guest UART write -> break.
 *
 * This is deliberately not a Linux kernel.  It exercises the same LatmSystem
 * platform, ELF loader and translator entry used by a future vmlinux boot.
 */
#include "qemu/osdep.h"
#include <stdio.h>
#include <string.h>
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "cpu.h"
#include "internal.h"
#include "latm.h"
#include "latm-helpers.h"
#include "latm-system-board.h"
#include "translate.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s) latx_bare_uart_putc(*s++);
}

static uint8_t ram[32u << 20] __attribute__((aligned(4096)));
static uint8_t code_buf[65536] __attribute__((aligned(4096)));
static CPUMIPSTLBContext tlb;
static uint8_t dtb[8192], dtb_struct[8192], dtb_strings[4096];
static uint64_t exit_jmp[16] __attribute__((aligned(16)));
static volatile int exit_active;
uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];

__asm__(
    ".text\n"
    ".globl smoke_setjmp\n"
    ".type smoke_setjmp,@function\n"
    "smoke_setjmp:\n"
    "  st.d $ra, $a0, 0\n"
    "  st.d $sp, $a0, 8\n"
    "  st.d $fp, $a0, 16\n"
    "  st.d $s0, $a0, 24\n"
    "  st.d $s1, $a0, 32\n"
    "  st.d $s2, $a0, 40\n"
    "  st.d $s3, $a0, 48\n"
    "  st.d $s4, $a0, 56\n"
    "  st.d $s5, $a0, 64\n"
    "  st.d $s6, $a0, 72\n"
    "  st.d $s7, $a0, 80\n"
    "  st.d $s8, $a0, 88\n"
    "  move $a0, $zero\n"
    "  jirl $zero, $ra, 0\n"
    ".size smoke_setjmp, .-smoke_setjmp\n"
    ".globl smoke_longjmp\n"
    ".type smoke_longjmp,@function\n"
    "smoke_longjmp:\n"
    "  ld.d $ra, $a0, 0\n"
    "  ld.d $sp, $a0, 8\n"
    "  ld.d $fp, $a0, 16\n"
    "  ld.d $s0, $a0, 24\n"
    "  ld.d $s1, $a0, 32\n"
    "  ld.d $s2, $a0, 40\n"
    "  ld.d $s3, $a0, 48\n"
    "  ld.d $s4, $a0, 56\n"
    "  ld.d $s5, $a0, 64\n"
    "  ld.d $s6, $a0, 72\n"
    "  ld.d $s7, $a0, 80\n"
    "  ld.d $s8, $a0, 88\n"
    "  li.d $a0, 1\n"
    "  jirl $zero, $ra, 0\n"
    ".size smoke_longjmp, .-smoke_longjmp\n"
);
extern int smoke_setjmp(uint64_t *);
extern void smoke_longjmp(uint64_t *);

void cpu_loop_exit(struct CPUState *cs)
{
    (void)cs;
    if (exit_active) smoke_longjmp(exit_jmp);
    for (;;) { }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static size_t build_elf(uint8_t *elf, const uint32_t *insns, size_t count)
{
    const size_t phoff = 64;
    const size_t code_off = 64 + 56;
    memset(elf, 0, code_off + count * 4);
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    elf[16] = 2; elf[18] = 8; elf[20] = 1;
    put64(elf + 24, 0x2000);
    put64(elf + 32, phoff);
    put64(elf + 40, 0);
    put32(elf + 20, 64);
    elf[52] = 64; elf[54] = 56; elf[56] = 1;
    put32(elf + phoff + 0, 1);
    put32(elf + phoff + 4, 5);
    put64(elf + phoff + 8, code_off);
    put64(elf + phoff + 16, 0x2000);
    put64(elf + phoff + 24, 0x2000);
    put64(elf + phoff + 32, count * 4);
    put64(elf + phoff + 40, count * 4);
    put64(elf + phoff + 48, 0x1000);
    for (size_t i = 0; i < count; i++) {
        put32(elf + code_off + i * 4, insns[i]);
    }
    return code_off + count * 4;
}

void kmain(void)
{
    /* KSEG1 UART (0xbfe001e0 maps to GPA 0x1fe001e0):
     * lui $t0,0xbfe0; ori $t0,$t0,0x1e0; ori $t1,$0,'K';
     * sb $t1,0($t0); break. */
    static const uint32_t insns[] = {
        0x3c08bfe0, 0x350801e0, 0x3409004b, 0xa1090000, 0x0000000d,
    };
    static uint8_t elf[128];
    LatmSystem sys;
    CPUMIPSState env;
    TranslationBlock tb;
    uint64_t entry = 0;

    latx_bare_uart_puts("latm-boot-smoke: begin\n");
    if (!latm_system_init_3a4000(&sys, ram, 0x100000000ull, sizeof(ram),
                                 0x100000000ull, 0x400000ull,
                                 0x100400000ull, 0x400000ull)) {
        latx_bare_uart_puts("latm-boot-smoke: FAIL system init\n");
        return;
    }
    latm_set_system_memory(true);
    size_t dtb_len = latm_system_build_dtb(&sys, dtb, sizeof(dtb),
                                           dtb_struct, sizeof(dtb_struct),
                                           dtb_strings, sizeof(dtb_strings));
    if (!dtb_len) {
        latx_bare_uart_puts("latm-boot-smoke: FAIL DTB\n");
        return;
    }
    memcpy(ram + 0x100000, dtb, dtb_len);
    latm_system_set_boot_args(&sys, 0x100000, 0, 0,
                              "console=ttyS0 earlycon");
    size_t elf_len = build_elf(elf, insns, sizeof(insns) / sizeof(insns[0]));
    if (!latm_system_load_elf(&sys, elf, elf_len, &entry)) {
        latx_bare_uart_puts("latm-boot-smoke: FAIL ELF load\n");
        return;
    }
    latx_bare_uart_puts("latm-boot-smoke: ELF loaded\n");
    latx_bare_uart_puts("latm-boot-smoke: entry=");
    {
        char b[17];
        for (int i = 15; i >= 0; i--) b[15 - i] = "0123456789abcdef"[(entry >> (i * 4)) & 15];
        b[16] = 0;
        latx_bare_uart_puts(b);
    }
    latx_bare_uart_putc('\n');

    memset(&env, 0, sizeof(env));
    env.SEGBITS = 42;
    env.PABITS = 36;
    latm_tlb_init(&env, &tlb, 16);
    env.exception_base = 0xffffffff80000000ull;
    env.CP0_IntCtl = 0xe0000000u;
    latm_system_apply_boot_args(&sys, &env);
    env.active_tc.PC = entry | 0xffffffff80000000ull;
    lata_tr_data_init(&env);
    tcg_ctx->code_ptr = code_buf + 0x1000;
    lata_gen_prologue_env(&env, tcg_ctx);
    tcg_ctx->code_ptr = code_buf + 0x3000;
    lata_gen_epilogue_env(&env, tcg_ctx);

    memset(&tb, 0, sizeof(tb));
    tb.pc = env.active_tc.PC;
    tb.tc.ptr = code_buf + 0x5000;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;
    tcg_ctx->code_ptr = tb.tc.ptr;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    int max = 16;
    target_disasm(&tb, &max, &env);
    if (!tb.icount || !tr_ir2_generate(&tb)) {
        tr_fini();
        latx_bare_uart_puts("latm-boot-smoke: FAIL first TB\n");
        return;
    }
    int n = tr_ir2_assemble(tb.tc.ptr);
    tr_fini();
    if (n <= 0) {
        latx_bare_uart_puts("latm-boot-smoke: FAIL assemble\n");
        return;
    }
    latx_bare_uart_puts("latm-boot-smoke: first TB\n");

    exit_active = 1;
    if (smoke_setjmp(exit_jmp) == 0) {
        ((void (*)(void *, void *))(uintptr_t)
         ((TRANSLATION_DATA *)env.latm_tr_data)->cs_bt_to_native)(
            &env, tb.tc.ptr);
    }
    exit_active = 0;

    latx_bare_uart_puts("latm-boot-smoke: UART tx=");
    {
        char b[17];
        for (int i = 15; i >= 0; i--) b[15 - i] = "0123456789abcdef"[(sys.uart.tx_count >> (i * 4)) & 15];
        b[16] = 0;
        latx_bare_uart_puts(b);
    }
    latx_bare_uart_putc('\n');
    if (sys.uart.tx_count != 1) {
        latx_bare_uart_puts("latm-boot-smoke: FAIL guest UART\n");
        return;
    }
    latx_bare_uart_puts("latm-boot-smoke: PASS\n");
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");
static char kstack[65536] __attribute__((aligned(16), used));
FILE *stderr;
