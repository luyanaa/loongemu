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
#include "env.h"

#ifndef LINUX_TB_BUDGET
#define LINUX_TB_BUDGET 200000u
#endif
/*
 * `rdtime.d` for throughput comparisons - **not** a wall clock here. Measured on
 * this QEMU: 1,000,000 translated blocks advanced the counter by 26,003,624
 * ticks while the shell measured ~205 s, i.e. ~127 kHz, so any seconds derived
 * from it are off by orders of magnitude. The harness therefore prints raw ticks
 * and ticks-per-block (both frequency-free and comparable between runs) and
 * leaves wall-clock seconds to whoever times the process.
 */


static uint64_t now_ticks(void)
{
    uint64_t v;

    __asm__ volatile("rdtime.d %0, $zero" : "=r"(v));
    return v;
}
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
static uint64_t tb_translated;
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

/* Write watchpoint: which guest store touches the frame slot that later feeds
 * `ld $ra,0x18($sp)`? The slot is around physical 0x1e9ae00 with the $sp seen in
 * the post-mortem, so watch that page and log the first few writers. */
static LatmPlatform watch_base;
static unsigned watch_hits;
static uint64_t watch_pc[8];
static uint64_t watch_gpa[8];
static uint64_t watch_val[8];

static int watch_write(CPUMIPSState *env, uint64_t gpa, uint64_t value,
                       uint32_t size)
{
    if (gpa >= 0x1e9adc0ull && gpa < 0x1e9ae00ull && watch_hits < 16u) {
        watch_pc[watch_hits] = env ? env->active_tc.PC : 0;
        watch_gpa[watch_hits] = gpa;
        watch_val[watch_hits] = value;
        watch_hits++;
    }
    return watch_base.mem_write ? watch_base.mem_write(env, gpa, value, size)
                               : LATM_MEM_OK;
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
    {
        const LatmPlatform *cur = latm_platform_get();

        if (cur) {
            watch_base = *cur;
            {
                static LatmPlatform watch_plat;

                watch_plat = watch_base;
                watch_plat.mem_write = watch_write;
                latm_platform_set(&watch_plat);
            }
        }
    }
    latm_exc_tap_enable = 1;
    latm_system_apply_boot_args(&sys, &env);
    env.active_tc.PC = linux_entry;
    lata_tr_data_init(&env);
    tcg_ctx->code_ptr = code_buf + 0x1000;
    {
        uint8_t *before = (uint8_t *)tcg_ctx->code_ptr;

        lata_gen_prologue_env(&env, tcg_ctx);
        latx_bare_uart_puts("latm-linux: prologue-bytes=");
        { char b[17]; uint64_t d = (uint64_t)((uint8_t *)tcg_ctx->code_ptr - before);
          for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(d>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_putc('\n');
    }
    tcg_ctx->code_ptr = code_buf + 0x3000;
    {
        uint8_t *before = (uint8_t *)tcg_ctx->code_ptr;

        lata_gen_epilogue_env(&env, tcg_ctx);
        latx_bare_uart_puts("latm-linux: epilogue-bytes=");
        { char b[17]; uint64_t d = (uint64_t)((uint8_t *)tcg_ctx->code_ptr - before);
          for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(d>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_putc('\n');
    }

    uint64_t t_start = now_ticks();
    for (unsigned i = 0; i < LINUX_TB_BUDGET; i++) {
        /*
         * Watch the registers that feed jalr/jr. A value that is not a canonical
         * kernel/stack address cannot be right for either, so the first time one
         * appears the dispatch that produced it is still the last one executed -
         * which names the block to look at far better than a post-mortem dump.
         */
        for (int g = 25; g <= 31; g += 6) {
            uint64_t v = env.active_tc.gpr[g];

            if (v != 0 && (v < 0xffffffff80000000ull ||
                           v >= 0xffffffff80000000ull + sizeof(ram))) {
                latx_bare_uart_puts("latm-linux: poisoned gpr[");
                { char b[9]; for (int k=7;k>=0;k--) b[7-k]="0123456789abcdef"[(g>>(k*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_puts("]=");
                { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(v>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_puts(" after pc=");
                { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(last_pc>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_puts(" sp=");
                { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env.active_tc.gpr[29]>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                {
                    uint64_t pa = env.active_tc.gpr[29] + 0x18u;

                    if (pa >= 0xffffffff80000000ull &&
                        pa < 0xffffffff80000000ull + sizeof(ram)) {
                        uint64_t mem = 0;

                        memcpy(&mem, ram + (pa - 0xffffffff80000000ull), 8);
                        latx_bare_uart_puts(" slot=");
                        { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(mem>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                    }
                }
                latx_bare_uart_puts(" at tbs=");
                { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(tb_ran>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_putc('\n');
                i = LINUX_TB_BUDGET;
                break;
            }
        }
        if (i >= LINUX_TB_BUDGET) {
            break;
        }
        /* A wild PC is where this kernel currently ends: report it instead of
         * trying to translate whatever the register holds. */
        if (env.active_tc.PC < 0xffffffff80000000ull ||
            env.active_tc.PC >= 0xffffffff80000000ull + sizeof(ram)) {
            latx_bare_uart_puts("latm-linux: bad pc=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env.active_tc.PC>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" at tbs=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(tb_ran>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" last-good=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(tb_ran>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_putc('\n');
            for (int g = 0; g < 32; g++) {
                latx_bare_uart_puts("latm-linux: gpr[");
                { char b[9]; for (int k=7;k>=0;k--) b[7-k]="0123456789abcdef"[(g>>(k*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_puts("]=");
                { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env.active_tc.gpr[g]>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_putc('\n');
            }
            break;
        }
        stop_pc = env.active_tc.PC;
        if (tb_ran && (tb_ran % 10000u) == 0) {
            latx_bare_uart_puts("latm-linux: progress tbs=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(tb_ran>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" pc=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env.active_tc.PC>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" ticks=");
            { char b[17]; uint64_t d = now_ticks() - t_start;
              for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(d>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
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
    latx_bare_uart_puts(" ticks=");
    { char b[17]; uint64_t d = now_ticks() - t_start;
      for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(d>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_puts(" ticks-per-tb=");
    { char b[17]; uint64_t d = now_ticks() - t_start;
      uint64_t n = tb_ran ? d / tb_ran : 0;
      for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(n>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    {
        uint64_t hl = 0, hs = 0, hu = 0;

        latm_helper_stats_get(&hl, &hs, &hu);
        latx_bare_uart_puts(" translations=");
        { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(tb_translated>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_puts(" helper-loads=");
        { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(hl>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_puts(" helper-stores=");
        { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(hs>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_puts(" per-block=");
        { char b[17]; uint64_t per = tb_ran ? ((hl + hs) * 1000u) / tb_ran : 0;
          for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(per>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    }
    latx_bare_uart_puts(" uart-tx=");
    { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(sys.uart.tx_count>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
    latx_bare_uart_putc('\n');
    /* The wrapper waits for this token, so a completed run must say so before
     * leaving: without it every run burns the whole QEMU timeout. */
    for (unsigned w = 0; w < watch_hits; w++) {
        latx_bare_uart_puts("latm-linux: watch write gpa=");
        { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(watch_gpa[w]>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_puts(" pc=");
        { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(watch_pc[w]>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_puts(" val=");
        { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(watch_val[w]>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
        latx_bare_uart_putc('\n');
    }
    {
        unsigned n = 0;
        uint64_t pc[8] = { 0 }, epc[8] = { 0 }, bad[8] = { 0 }, code[8] = { 0 };

        latm_exc_tap_get(&n, pc, epc, bad, code, 8);
        for (unsigned k = 0; k < 8 && k < n; k++) {
            latx_bare_uart_puts("latm-linux: exc pc=");
            { char b[17]; for (int q=15;q>=0;q--) b[15-q]="0123456789abcdef"[(pc[k]>>(q*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" epc=");
            { char b[17]; for (int q=15;q>=0;q--) b[15-q]="0123456789abcdef"[(epc[k]>>(q*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" bad=");
            { char b[17]; for (int q=15;q>=0;q--) b[15-q]="0123456789abcdef"[(bad[k]>>(q*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" code=");
            { char b[9]; for (int q=7;q>=0;q--) b[7-q]="0123456789abcdef"[(code[k]>>(q*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_putc('\n');
        }
    }
    latx_bare_uart_puts("latm-linux: done\n");
    *(volatile uint32_t *)(uintptr_t)0x100000 = 0x5555u; /* LVZ exit device */
    for (;;) {
    }
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
        /* 32 aborts the translator on the kernel's post-BSS function
         * (out of IR2 itemps); 16 translates it. See roadmap. */
        int max = 16;
        int n;

        if (tb_translated < 64u) {
            latx_bare_uart_puts("latm-linux: translate pc=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env->active_tc.PC>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_putc('\n');
        }
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
#ifdef LINUX_WATCH_GPR31
        {
            TRANSLATION_DATA *td = (TRANSLATION_DATA *)env->latm_tr_data;
            unsigned st31 = 0, st_env = 0;

            for (IR2_INST *inst = td->first_ir2; inst != NULL;
                 inst = ir2_next(inst)) {
                if (ir2_opcode(inst) != LISA_ST_D || inst->op_count != 3 ||
                    inst->_opnd[1]._reg_num != env_ir2_opnd._reg_num) {
                    continue;
                }
                st_env++;
                if (inst->_opnd[2]._imm32 == env_offset_gpr(31)) {
                    st31++;
                }
            }
            if (env->active_tc.PC == 0xffffffff8149051cull) {
                unsigned shown = 0;

                for (IR2_INST *inst = td->first_ir2; inst != NULL && shown < 300u;
                     inst = ir2_next(inst), shown++) {
                    int op = (int)ir2_opcode(inst);

                    if (op == LISA_ST_D && inst->op_count == 3 &&
                        inst->_opnd[2]._imm32 == env_offset_gpr(31)) {
                        latx_bare_uart_puts("latm-linux: st31 ir2#");
                        { char b[9]; for (int k=7;k>=0;k--) b[7-k]="0123456789abcdef"[(shown>>(k*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
                        latx_bare_uart_puts(" val=r");
                        { char b[9]; for (int k=7;k>=0;k--) b[7-k]="0123456789abcdef"[(inst->_opnd[0]._reg_num>>(k*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
                        latx_bare_uart_putc('\n');
                    }
                }
            }
            if (st_env > 0 || env->active_tc.PC == 0xffffffff8149051cull) {
                latx_bare_uart_puts("latm-linux: ir2 env-stores=");
                { char b[9]; for (int k=7;k>=0;k--) b[7-k]="0123456789abcdef"[(st_env>>(k*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_puts(" to-gpr31=");
                { char b[9]; for (int k=7;k>=0;k--) b[7-k]="0123456789abcdef"[(st31>>(k*4))&15]; b[8]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_puts(" at pc=");
                { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env->active_tc.PC>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
                latx_bare_uart_putc('\n');
            }
        }
#endif
        n = tr_ir2_assemble(host);
        tb_translated++;
        tr_fini();
        if (n <= 0) {
            return 0;
        }
#ifdef LINUX_DUMP_BLOCK
        if (tb_ran == 0) {
            const uint32_t *w = (const uint32_t *)host;
            for (int k = 0; k < n; k++) {
                char b[9];
                for (int q = 0; q < 8; q++) b[q] = "0123456789abcdef"[(w[k] >> (q * 4)) & 15];
                b[8] = 0;
                latx_bare_uart_puts(b);
                latx_bare_uart_putc(k + 1 == n ? '\n' : ' ');
            }
        }
#endif
        if (tb_ran < 4) {
            latx_bare_uart_puts("latm-linux: block pc=");
            { char b[17]; for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(env->active_tc.PC>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" guest-insns=");
            { char b[17]; uint64_t g = tb_live.icount;
              for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(g>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_puts(" host-words=");
            { char b[17]; uint64_t w = (uint64_t)n;
              for (int k=15;k>=0;k--) b[15-k]="0123456789abcdef"[(w>>(k*4))&15]; b[16]=0; latx_bare_uart_puts(b); }
            latx_bare_uart_putc('\n');
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
#ifndef LINUX_NO_EXEC
    exit_active = 1;
    if (smoke_setjmp(exit_jmp) == 0) {
        ((void (*)(void *, void *))(uintptr_t)
         ((TRANSLATION_DATA *)env->latm_tr_data)->cs_bt_to_native)(env, host);
    }
    exit_active = 0;
#endif
    tb_ran++;
    return 1;
}

void kmain(void);
static int linux_run_one(CPUMIPSState *env);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(kstack)\n  addi.d $sp, $sp, %pc_lo12(kstack)\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");
static char kstack[65536] __attribute__((aligned(16), used));
FILE *stderr;
