/*
 * Loongson-3 Linux boot attempt (2.3 P0).
 *
 * Boots a real Debian `linux-image-*-loongson-3` vmlinux: the ELF is embedded by
 * `latm_linux_blob.S` from whatever `$(LOONGSON_KERNEL)` points at, loaded into
 * 256 MiB of RAM through the same LatmSystem/ELF path the smoke test uses, and
 * then driven by a translate-and-run loop with a TB cache, so the kernel can run
 * for as long as it makes progress. Success is *not* asserted here: the harness
 * reports the serial output, the number of translated blocks and where the CPU
 * stopped, which is what the next step needs.
 *
 * The kernel is not vendored: the image is built from a path the caller
 * provides, so this target is local-only and never part of CI.
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
#include "loongemu/tb_cache.h"

extern const uint8_t latm_linux_elf[];
extern const uint8_t latm_linux_elf_end[];
#include "translate.h"

#define LINUX_TB_BUDGET 5000000u
#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s) latx_bare_uart_putc(*s++);
}

static uint8_t ram[64u << 20] __attribute__((aligned(4096)));
static uint8_t tb_pool[1u << 20] __attribute__((aligned(4096)));
static uint32_t tb_next;
static TbCacheEntry tb_entries[512];
static uint32_t tb_buckets[512];
static TbCache tb_cache;
static TranslationBlock tb_live;
static uint64_t tb_ran;
static uint64_t linux_entry;
static uint64_t stop_pc;
static uint64_t stop_cause;
/* Last dispatch, to skip the hash lookup when a block branches to itself - the
 * common case in a tight loop and the entire cost driver for a 3-instruction
 * block. */
static uint64_t last_pc;
static uint64_t last_asid;
static uint8_t *last_host;
static uint8_t last_mode;
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

static int linux_run_one(CPUMIPSState *env);

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

    latx_bare_uart_puts("latm-linux: begin\n");
    if (!latm_system_init_3a4000(&sys, ram, 0x100000000ull, sizeof(ram),
                                 0x100000000ull, 0x400000ull,
                                 0x100400000ull, 0x400000ull)) {
        latx_bare_uart_puts("latm-linux: FAIL system init\n");
        return;
    }
    latm_set_system_memory(true);
    size_t dtb_len = latm_system_build_dtb(&sys, dtb, sizeof(dtb),
                                           dtb_struct, sizeof(dtb_struct),
                                           dtb_strings, sizeof(dtb_strings));
    if (!dtb_len) {
        latx_bare_uart_puts("latm-linux: FAIL DTB\n");
        return;
    }
    memcpy(ram + 0x100000, dtb, dtb_len);
    latm_system_set_boot_args(&sys, 0x100000, 0, 0,
                              "console=ttyS0 earlycon");
    if (!tbc_init(&tb_cache, tb_entries, tb_buckets, 512)) {
        latx_bare_uart_puts("latm-linux: FAIL tb cache\n");
        return;
    }
    if (!latm_system_load_elf(&sys, latm_linux_elf,
                              (uint64_t)(latm_linux_elf_end - latm_linux_elf),
                              &linux_entry)) {
        latx_bare_uart_puts("latm-linux: FAIL kernel ELF\n");
        return;
    }
    latx_bare_uart_puts("latm-linux: kernel loaded, entry=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(linux_entry>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_putc('\n');

    memset(&env, 0, sizeof(env));
    env.SEGBITS = 42;
    env.PABITS = 36;
    latm_tlb_init(&env, &tlb, 16);
    env.exception_base = 0xffffffff80000000ull;
    env.CP0_IntCtl = 0xe0000000u;
    latm_system_apply_boot_args(&sys, &env);
    env.active_tc.PC = linux_entry;
    lata_tr_data_init(&env);
    tcg_ctx->code_ptr = code_buf + 0x1000;
    lata_gen_prologue_env(&env, tcg_ctx);
    tcg_ctx->code_ptr = code_buf + 0x3000;
    lata_gen_epilogue_env(&env, tcg_ctx);

    for (unsigned i = 0; i < LINUX_TB_BUDGET; i++) {
        stop_pc = env.active_tc.PC;
        if (tb_ran && (tb_ran % 1000000u) == 0) {
            latx_bare_uart_puts("latm-linux: progress tbs=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(tb_ran>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" pc=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env.active_tc.PC>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_putc('\n');
        }
        if (!linux_run_one(&env)) {
            latx_bare_uart_puts("latm-linux: translation refused at pc=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(stop_pc>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_putc('\n');
            break;
        }
    }
    stop_cause = env.CP0_Cause;
    latx_bare_uart_puts("\nlatm-linux: tbs=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(tb_ran>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_puts(" pc=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(env.active_tc.PC>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_puts(" epc=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(env.CP0_EPC>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_puts(" cause=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(stop_cause>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_puts(" uart-tx=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(sys.uart.tx_count>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_putc('\n');
}

/* One dispatch: translate on a cache miss, run, count. Returns 0 when the
 * translator refuses a block, which is where a real boot currently stops. */
static int linux_run_one(CPUMIPSState *env)
{
    uint64_t asid = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    uint8_t mode = (uint8_t)(env->hflags &
                             (MIPS_HFLAG_UM | MIPS_HFLAG_SM | MIPS_HFLAG_M16));
    TbCacheEntry *ent;
    uint8_t *host;

    if (last_host && env->active_tc.PC == last_pc && asid == last_asid &&
        mode == last_mode) {
        host = last_host;
        goto run;
    }
    ent = tbc_lookup_mips(&tb_cache, env->active_tc.PC, (uint32_t)asid, mode);

    if (tb_next + 0x2000u > sizeof(tb_pool)) {
        tb_next = 0;
        tbc_flush_all(&tb_cache);
    }
    if (ent && ent->code) {
        host = (uint8_t *)(uintptr_t)ent->code;
        goto have_host;
    } else {
        int max = 32;
        int n;

        host = tb_pool + tb_next;
        tb_next += 0x2000u;
        memset(&tb_live, 0, sizeof(tb_live));
        tb_live.pc = env->active_tc.PC;
        tb_live.tc.ptr = host;
        tb_live.jmp_reset_offset[0] = 0xffff;
        tb_live.jmp_reset_offset[1] = 0xffff;
        tcg_ctx->code_ptr = host;
        tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
        target_disasm(&tb_live, &max, env);
        if (!tb_live.icount || !tr_ir2_generate(&tb_live)) {
            tr_fini();
            return 0;
        }
        n = tr_ir2_assemble(host);
        tr_fini();
        if (n <= 0) {
            return 0;
        }
        ent = tbc_insert_mips(&tb_cache, tb_live.pc, (uint32_t)asid, mode);
        if (ent) {
            ent->code = (uintptr_t)host;
            ent->code_size = (uint32_t)n;
            ent->guest_size = tb_live.size;
        }
    }
have_host:
    last_pc = env->active_tc.PC;
    last_asid = asid;
    last_mode = mode;
    last_host = host;
run:
    exit_active = 1;
    if (smoke_setjmp(exit_jmp) == 0) {
        ((void (*)(void *, void *))(uintptr_t)
         ((TRANSLATION_DATA *)env->latm_tr_data)->cs_bt_to_native)(env, host);
    }
    exit_active = 0;
    tb_ran++;
    return 1;
}

void kmain(void);
static int linux_run_one(CPUMIPSState *env);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(kstack)\n  addi.d $sp, $sp, %pc_lo12(kstack)\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");
static char kstack[65536] __attribute__((aligned(16), used));
FILE *stderr;
