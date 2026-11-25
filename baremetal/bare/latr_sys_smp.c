/*
 * Sys-mode SMP without the machine: the two-hart guest that `latr-smp-run`
 * boots, driven by an embedder that owns the scheduler.
 *
 * This is the contract `latr_machine_run_hart()` implements, written out by
 * hand so every piece is visible: one LatrTranslatorContext and TB cache per
 * hart (bound around each dispatch), one shared LatrSystem for memory and
 * devices, and the after-block duties - SFENCE.VMA invalidation, FENCE.I
 * invalidation of *both* caches, WFI idle advance, interrupt delivery. A
 * cross-core FENCE.I that only invalidates the issuer's cache passes every
 * single-hart test and fails the guest's "hart 1 sees the rewritten target"
 * word, which is this image's reason to exist beside latr-smp-run.
 */
#include <stdint.h>
#include "latr.h"
#include "latr-system.h"
#include "translate.h"

extern const uint8_t latr_smp_guest[];
extern const uint8_t latr_smp_guest_end[];
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

#define NHARTS 2u
#define DATA_PAGE 0x80001000u
#define EXIT_OK 0x5555u
#define STEP_BUDGET 4000u

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)
static void uart_putc(int c) { *UART0_THR = (uint8_t)c; }
static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

static void puthex(uint64_t v)
{
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned d = (unsigned)((v >> shift) & 0xfu);

        uart_putc(d < 10u ? (int)('0' + d) : (int)('a' + d - 10u));
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
    uart_puts(&b[i]);
}

static int checks;
static int failures;

static void check_u64(const char *what, uint64_t got, uint64_t want)
{
    checks++;
    if (got != want) {
        failures++;
        uart_puts("latr-sys-smp: FAIL ");
        uart_puts(what);
        uart_puts(" got=");
        puthex(got);
        uart_puts(" want=");
        puthex(want);
        uart_puts("\n");
    }
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(latr_sys_smp_stack)\n"
        "  addi.d $sp, $sp, %pc_lo12(latr_sys_smp_stack)\n"
        "  lu12i.w $t0, 256\n  add.d $sp, $sp, $t0\n  bl kmain\n1: b 1b\n");
static uint8_t latr_sys_smp_stack[1u << 20] __attribute__((aligned(16), used));

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t arena[NHARTS][1u << 18] __attribute__((aligned(4096)));
/* Translation scratch, one per hart and separate from the cache arena: the
 * assembler writes here first and the cache copies from it, so aliasing the two
 * makes every new translation overwrite code that is still cached and live. */
static uint8_t code_buf[NHARTS][1u << 16] __attribute__((aligned(4096)));
static LatrTbCache cache[NHARTS];
static LatrTranslatorContext *ctx[NHARTS];
static TranslationBlock tb;
static CPUState cpu;
static LatrSystem sys;
static CPUArchState env1;

static uint64_t read64(uint64_t pa)
{
    const uint64_t *p = (const uint64_t *)(const void *)(
        ram + (pa - LATR_SYSTEM_RAM_BASE));

    return *p;
}

/* One dispatch for one hart. Returns 1 when a block was executed. */
static int step_hart(uint32_t hart, CPUArchState *env)
{
    LatrTranslatorContext *saved;
    LatrTbKey key;
    LatrTbEntry *ent;
    uint64_t code;
    int limit = 64;
    int words;

    latr_system_update_hart(&sys, env, hart);
    (void)latr_deliver_interrupt(env);
    env->exception = 0;

    saved = latr_context_bind(ctx[hart]);
    key = latr_tb_cache_key(env);
    ent = latr_tb_cache_lookup(&cache[hart], env->pc, key);
    if (ent == NULL) {
        memset(&tb, 0, sizeof(tb));
        tb.pc = env->pc;
        tb.tc.ptr = code_buf[hart];
        cpu.env_ptr = env;
        target_disasm(&tb, &limit, &cpu);
        if (tb.icount == 0) {
            latr_context_restore(saved);
            return 0;
        }
        tr_init(&tb);
        if (!tr_ir2_generate(&tb)) {
            tr_fini();
            latr_context_restore(saved);
            return 0;
        }
        words = tr_ir2_assemble(code_buf[hart]);
        tr_fini();
        ent = latr_tb_cache_insert(&cache[hart], tb.pc, key, code_buf[hart],
                                   (uint32_t)words * 4, tb.icount);
        if (ent != NULL) {
            latr_tb_insn_map_rebase(code_buf[hart], ent->code);
        }
    }
    code = ent != NULL ? (uint64_t)(uintptr_t)ent->code
                       : (uint64_t)(uintptr_t)code_buf[hart];
    latr_enter_tb(env, (void *)(uintptr_t)code);
    latr_context_restore(saved);

    if (env->sfence_pending) {
        latr_tb_shootdown_asid(&cache[hart], (uint32_t)env->sfence_asid);
        env->sfence_pending = 0;
    }
    if (env->fence_i_pending) {
        for (uint32_t i = 0; i < NHARTS; i++) {
            latr_tb_shootdown_all(&cache[i]);
        }
        env->fence_i_pending = 0;
    }
    if (env->wfi_pending) {
        latr_idle_advance(env);
        env->wfi_pending = 0;
    }
    env->exception = 0;
    env->mem_fault_cause = 0;
    env->fetch_faulted = 0;
    return 1;
}

void kmain(void)
{
    CPUArchState *envs[NHARTS];
    uint64_t dispatched[NHARTS] = { 0, 0 };
    uint64_t len = (uint64_t)(latr_smp_guest_end - latr_smp_guest);

    uart_puts("latr-sys-smp: begin\n");
    if (!latr_system_init_harts(&sys, ram, sizeof(ram), NHARTS, 0)) {
        uart_puts("latr-sys-smp: FAIL board\n");
        return;
    }
    for (uint32_t i = 0; i < NHARTS; i++) {
        latr_tb_cache_init(&cache[i], arena[i], sizeof(arena[i]));
        latr_tb_cache_register(&cache[i]);
    }
    if (len == 0 || len > sizeof(ram)) {
        uart_puts("latr-sys-smp: FAIL firmware\n");
        return;
    }
    memcpy(ram, latr_smp_guest, (size_t)len);
    /* Start from the board's env so mem_map, the TLB and the feature bits are
     * the board's; only the hart identity and the entry differ. A zeroed env
     * has no mem_map and its branch exits produce a zero pc. */
    env1 = sys.env;
    env1.privilege = LATR_RISCV_PRIV_M;
    env1.hartid = 1;
    env1.pc = LATR_SYSTEM_RAM_BASE;
    env1.gpr[10] = 1;
    sys.env.pc = LATR_SYSTEM_RAM_BASE;
    sys.env.hartid = 0;
    sys.env.gpr[10] = 0;
    envs[0] = &sys.env;
    envs[1] = &env1;
    for (uint32_t i = 0; i < NHARTS; i++) {
        ctx[i] = latr_context_create(envs[i], &cache[i], code_buf[i],
                                     (uint32_t)sizeof(code_buf[i]));
        if (ctx[i] == NULL) {
            uart_puts("latr-sys-smp: FAIL context\n");
            return;
        }
    }
    uart_puts("latr-sys-smp: setup ok pc0=");
    puthex(envs[0]->pc);
    uart_puts(" pc1=");
    puthex(envs[1]->pc);
    uart_puts("\n");

    for (unsigned step = 0; step < STEP_BUDGET && sys.exit_request != EXIT_OK;
         step++) {
        for (uint32_t h = 0; h < NHARTS; h++) {
            dispatched[h] += (uint64_t)step_hart(h, envs[h]);
            if (sys.exit_request == EXIT_OK) {
                break;
            }
        }
    }

    uart_puts("latr-sys-smp: tbs0=");
    putdec((unsigned)dispatched[0]);
    uart_puts(" tbs1=");
    putdec((unsigned)dispatched[1]);
    uart_puts(" exit=");
    putdec((unsigned)sys.exit_request);
    uart_puts("\n");
    uart_puts("latr-sys-smp: data counter=");
    puthex(read64(DATA_PAGE + 0x08u));
    uart_puts(" lrsc=");
    puthex(read64(DATA_PAGE + 0x10u));
    uart_puts(" h1=");
    puthex(read64(DATA_PAGE + 0x30u));
    uart_puts("\n");

    check_u64("guest powered off through the sys exit device",
              sys.exit_request, EXIT_OK);
    check_u64("both harts dispatched blocks",
              (dispatched[0] > 0 && dispatched[1] > 0) ? 1 : 0, 1);
    check_u64("AMO counter is 32", read64(DATA_PAGE + 0x08u), 32);
    check_u64("LR/SC incremented 16 times", read64(DATA_PAGE + 0x10u), 16);
    check_u64("hart 1 called the old target", read64(DATA_PAGE + 0x18u), 1);
    check_u64("one software interrupt delivered", read64(DATA_PAGE + 0x20u), 1);
    check_u64("hart 0 sees the rewritten target", read64(DATA_PAGE + 0x28u), 2);
    check_u64("hart 1 sees the rewritten target", read64(DATA_PAGE + 0x30u), 2);
    check_u64("the sys device layer never failed a lock acquisition",
              latr_system_lock_failures(&sys), 0);
    check_u64("no translator borrow leaks in this run",
              latr_regcache_stats().borrow_leaks, 0);

    uart_puts(failures == 0 ? "latr-sys-smp: PASS checks="
                            : "latr-sys-smp: FAIL checks=");
    putdec((unsigned)checks);
    uart_puts(" fails=");
    putdec((unsigned)failures);
    uart_puts("\nlatr-sys-smp: done\n");
}
