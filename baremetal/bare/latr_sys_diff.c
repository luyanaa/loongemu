/*
 * LATR side of the RISC-V virt system differential:
 *   embedded guest ELF -> LATR system board -> UART log.
 */
#include <stdint.h>
#include "latr.h"
#include "loongemu-perf.h"
#include "latr-system.h"
#include "translate.h"
#include "latr_system_guest_elf.c"

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

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (uint8_t)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s) {
        latx_bare_uart_putc(*s++);
    }
}


static void putdec(unsigned v)
{
    char b[11];
    int i = 10;

    b[i] = 0;
    do {
        b[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && i > 0);
    latx_bare_uart_puts(&b[i]);
}

void kmain(void);

__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(latr_sys_stack)\n  addi.d $sp, $sp, %pc_lo12(latr_sys_stack)\n"
        "  lu12i.w $t0, 128\n  add.d $sp, $sp, $t0\n  bl kmain\n1: b 1b\n");
static uint8_t latr_sys_stack[512u << 10] __attribute__((aligned(16), used));

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t code_buf[1u << 18] __attribute__((aligned(4096)));
static uint8_t cache_arena[1u << 20] __attribute__((aligned(4096)));
static LatrSystem sys;
static LatrTbCache cache;
static TranslationBlock tb;
static CPUState cpu;

static void perf_put_hex(uint64_t value)
{
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (unsigned)((value >> shift) & 0xfu);

        latx_bare_uart_putc(digit < 10u ? (char)('0' + digit)
                                        : (char)('a' + digit - 10u));
    }
}

void kmain(void)
{
    uint64_t entry = 0;
    int halted = 0;

    latx_bare_uart_puts("latr-sys: begin\n");
    if (!latr_system_init(&sys, ram, sizeof(ram), 0)) {
        latx_bare_uart_puts("latr-sys: FAIL init\n");
        return;
    }
    latr_tr_data_init(&sys.env);
    latr_tb_cache_init(&cache, cache_arena, sizeof(cache_arena));
    latr_tb_cache_register(&cache);
    if (!latr_system_load_elf(&sys, latr_system_guest_elf,
                              latr_system_guest_elf_len, &entry)) {
        latx_bare_uart_puts("latr-sys: FAIL elf\n");
        return;
    }
    memset(&cpu, 0, sizeof(cpu));
    cpu.env_ptr = &sys.env;

    for (int tbs = 0; tbs < 20000; tbs++) {
        LatrTbKey key;
        LatrTbEntry *ent;
        uint64_t code;

        latr_system_update(&sys);
        if (latr_deliver_interrupt(&sys.env)) {
            sys.env.exception = 0;
        }
        key = latr_tb_cache_key(&sys.env);
        ent = latr_tb_cache_lookup(&cache, sys.env.pc, key);
        if (ent == NULL) {
            int limit = 64;
            int words;
            memset(&tb, 0, sizeof(tb));
            tb.pc = sys.env.pc;
            tb.tc.ptr = code_buf;
            tb.cflags = 0;
            target_disasm(&tb, &limit, &cpu);
            if (tb.icount == 0) {
                break;
            }
            tr_init(&tb);
            if (!tr_ir2_generate(&tb)) {
                tr_fini();
                break;
            }
            words = tr_ir2_assemble(code_buf);
            ent = latr_tb_cache_insert(&cache, tb.pc, key, code_buf,
                                       (uint32_t)words * 4, tb.icount);
            if (ent != NULL) {
                latr_tb_insn_map_rebase(code_buf, ent->code);
            }
            tr_fini();
        }
        code = ent != NULL ? (uint64_t)(uintptr_t)ent->code
                           : (uint64_t)(uintptr_t)code_buf;
        latr_enter_tb(&sys.env, (void *)(uintptr_t)code);
        if (sys.env.sfence_pending) {
            latr_tb_shootdown_asid(&cache, (uint32_t)sys.env.sfence_asid);
            sys.env.sfence_pending = 0;
        }
        if (sys.env.fence_i_pending) {
            latr_tb_shootdown_all(&cache);
            sys.env.fence_i_pending = 0;
        }
        if (sys.env.wfi_pending) {
            latr_idle_advance(&sys.env);
            sys.env.wfi_pending = 0;
        }
        if (sys.env.exception) {
            sys.env.exception = 0;
        }
        if (sys.exit_request) {
            halted = 1;
            break;
        }
    }
    latx_bare_uart_puts(" latr-sys: guest=");
    latx_bare_uart_puts(latr_system_uart_log(&sys));
    latx_bare_uart_puts(halted ? "\nlatr-sys: PASS\n" : "\nlatr-sys: FAIL\n");
    {
        /* Same accounting as the ISA harness, on a guest that boots: the
         * translation-time cost of the register cache for design P0. */
        LatrRegCacheStats rc = latr_regcache_stats();

        latx_bare_uart_puts("latr-regcache: flushes=");
        putdec((unsigned)rc.flushes);
        latx_bare_uart_puts(" writebacks=");
        putdec((unsigned)rc.writebacks);
        latx_bare_uart_puts(" reloads=");
        putdec((unsigned)rc.reloads);
        latx_bare_uart_puts(" stores=");
        putdec((unsigned)rc.stores);
        latx_bare_uart_puts(" evictions=");
        putdec((unsigned)rc.evictions);
        latx_bare_uart_puts("\n");
    }
#ifdef LOONGEMU_PERF
    {
        /* A paged workload with real MMIO and traps: the numbers the P1
         * fast-path work has to move, as opposed to the ISA suite's mix. */
        static const char *const names[] = {
            "tlb-hit", "tlb-miss", "mmio", "mem-fault", "plan-hit",
            "fast-hit",
        };
        const unsigned report = (unsigned)(sizeof(names) / sizeof(names[0]));

        latx_bare_uart_puts("latr-perf:");
        for (unsigned i = 0; i < report; i++) {
            latx_bare_uart_putc(' ');
            latx_bare_uart_puts(names[i]);
            latx_bare_uart_putc('=');
            perf_put_hex(loongemu_perf_read(i));
        }
        latx_bare_uart_puts(" helper-calls=");
        perf_put_hex(loongemu_perf_read(LOONGEMU_PERF_HELPER_CALLS));
        latx_bare_uart_puts(" mmio-ticks=");
        perf_put_hex(loongemu_perf_read(LOONGEMU_PERF_MMIO_TICKS));
        latx_bare_uart_puts(" recovery-ticks=");
        perf_put_hex(loongemu_perf_read(LOONGEMU_PERF_RECOVERY_TICKS));
        {
            uint64_t hot_page = 0;
            uint32_t hot = latr_tb_hot_busiest(&cache, &hot_page);

            latx_bare_uart_puts(" hot-translations=");
            perf_put_hex(hot);
            latx_bare_uart_puts(" hot-page=");
            perf_put_hex(hot_page);
        }
        latx_bare_uart_puts("\n");
    }
#endif
    latx_bare_uart_puts("latr-sys: done\n");
}
