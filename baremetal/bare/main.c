/*
 * Bare-metal LoongArch execution harness (no UEFI, direct addressing).
 *
 * Boots with qemu-system-loongarch64 -M virt -kernel <elf>, sets up the minimal
 * state the LATX translator needs, generates the epilogue stub, translates a
 * small x86 TB, rewrites the TB linker slot to the epilogue, and runs the TB
 * on the real (emulated) CPU.  LBT instructions are emulated from the #INE
 * vector because QEMU does not execute them.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "ir1.h"
#include "ir2.h"
#include "translate.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)

void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s) { while (*s) { latx_bare_uart_putc(*s++); } }

static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int sh;

    latx_bare_uart_puts("0x");
    for (sh = 60; sh >= 0; sh -= 4) {
        latx_bare_uart_putc(d[(v >> sh) & 0xf]);
    }
}

static int checks, fails;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        latx_bare_uart_puts("FAIL ");
        latx_bare_uart_puts(what);
        latx_bare_uart_puts("\n");
    }
}

#include "lbt_emu.h"

/* ---------------- execution harness ---------------- */
static CPUX86State env;
static TCGContext tcg_storage;
static uint8_t code_cache[1 << 16] __attribute__((aligned(16)));
static uint8_t ir1_dt_area[8192] __attribute__((aligned(16)));
static IR1_INST ir1_list[8];
static struct separated_data ir1_sd;
static TranslationBlock smoke_tb;
static uint8_t jmp_cache[4096] __attribute__((aligned(16)));

unsigned char kstack[65536] __attribute__((aligned(16)));
void kmain(void);

__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");

extern uint64_t latx_run_body(void *tb, void *env, void *epi);

__asm__(
".text\n.globl latx_run_body\nlatx_run_body:\n"
"  addi.d $sp,$sp,-256\n"
"  st.d $ra,$sp,120\n  st.d $fp,$sp,112\n"
"  st.d $s0,$sp,40\n  st.d $s1,$sp,48\n  st.d $s2,$sp,56\n  st.d $s3,$sp,64\n"
"  st.d $s4,$sp,72\n  st.d $s5,$sp,80\n  st.d $s6,$sp,88\n  st.d $s7,$sp,96\n  st.d $s8,$sp,104\n"
"  move $s8,$a1\n"
"  move $t4,$a2\n"
"  ld.d $s0,$a1,0\n  ld.d $s3,$a1,24\n"
"  jirl $zero,$a0,0\n"
);

void kmain(void)
{
    static const uint8_t exec_bytes[] = {
        0x48, 0x89, 0xd8,       /* mov rax, rbx */
        0x48, 0x8d, 0x40, 0x01, /* lea rax,[rax+1] */
        0xeb, 0xf8,             /* jmp 0x1000 */
    };
    uint8_t code[32] __attribute__((aligned(8))) = { 0 };
    TRANSLATION_DATA tr;
    ENV lsenv_storage;
    uint8_t *epi_buf = code_cache + 0x2000;
    uint8_t *tb_buf = code_cache + 0x4000;
    int off = 0, n = 0, rc;

    latx_bare_uart_puts("bare: start\n");

    /* enable FPU/LSX/LASX units and disable interrupts */
    {
        uint64_t euen, crmd;

        __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
        euen |= 0x1 | 0x2 | 0x40 | 0x80;
        __asm__ volatile("csrwr %0, 0x2" :: "r"(euen));
        __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
        crmd &= ~(uint64_t)0x4;
        __asm__ volatile("csrwr %0, 0x0" :: "r"(crmd));
        lbt_emu_install();
    }

    memset(&env, 0, sizeof(env));
    memset(&tr, 0, sizeof(tr));
    memset(&lsenv_storage, 0, sizeof(lsenv_storage));
    memset(&tcg_storage, 0, sizeof(tcg_storage));
    memset(&ir1_sd, 0, sizeof(ir1_sd));
    memset(&smoke_tb, 0, sizeof(smoke_tb));
    memset(ir1_list, 0, sizeof(ir1_list));
    memset(code, 0, sizeof(code));
    memset(jmp_cache, 0, sizeof(jmp_cache));
    memset(&sw_lbt, 0, sizeof(sw_lbt));

    env.sys.codemode = 1;
    env.tb_jmp_cache_ptr = jmp_cache;
    env.regs[R_EAX] = 0x1111;
    env.regs[R_EBX] = 0x2222;
    env.eip = 0x1000;
    lsenv_storage.cpu_state = &env;
    lsenv_storage.tr_data = &tr;
    lsenv = &lsenv_storage;

    tcg_storage.code_gen_buffer = code_cache;
    tcg_storage.code_gen_buffer_size = sizeof(code_cache);
    tcg_storage.code_gen_ptr = code_cache;
    tcg_storage.code_gen_highwater = code_cache + sizeof(code_cache);
    tcg_ctx = &tcg_storage;
        tcg_storage.gen_insn_data = latx_tcg_gen_insn_data;

    latx_runtime_init();
    latx_bare_uart_puts("bare: epi=");
    puthex((uint64_t)(uintptr_t)epi_buf);
    puthex((uint64_t)(int64_t)latx_gen_epilogue(epi_buf));
    latx_bare_uart_puts("\n");

    for (unsigned b = 0; b < sizeof(exec_bytes); b++) {
        code[b] = exec_bytes[b];
    }
    while (off < (int)sizeof(exec_bytes) && n < 8) {
        ADDRX next = ir1_disasm(&ir1_list[n], code + off,
                                (ADDRX)(0x1000 + off), n, ir1_dt_area);
        if (next <= (ADDRX)(0x1000 + off)) {
            break;
        }
        off = (int)(next - 0x1000);
        n++;
    }
    ir1_sd.ir1 = ir1_list;
    ir1_sd.is_first_tb = 1;
    ir1_sd.next_pc = 0x1000 + off;
    smoke_tb.pc = 0x1000;
    smoke_tb.size = (uint16_t)off;
    smoke_tb.icount = (uint16_t)n;
    smoke_tb.tc.ptr = tb_buf;
    smoke_tb.s_data = &ir1_sd;

    rc = tr_translate_tb(&smoke_tb);
    latx_bare_uart_puts("bare: tb bytes=");
    puthex((uint64_t)(int64_t)rc);
    latx_bare_uart_puts("\n");

    /* rewrite the TB linker slot to `jirl $zero, $t4, 0`; the harness stub
     * puts the epilogue address in t4, so no branch-immediate math is needed. */
    if (rc >= 20) {
        uint32_t *w = (uint32_t *)tb_buf;

        w[2] = 0x4c000200u;  /* jr $t4 */
        w[3] = 0x03400000u;
        w[4] = 0x03400000u;
    }

    latx_bare_uart_puts("bare: tb=");
    puthex((uint64_t)(uintptr_t)tb_buf);
    latx_bare_uart_puts(" epi=");
    puthex((uint64_t)(uintptr_t)epi_buf);
    latx_bare_uart_puts(" epi_w0=");
    puthex((uint64_t)((const uint32_t *)epi_buf)[0]);
    latx_bare_uart_puts(" tb_w2=");
    puthex((uint64_t)((const uint32_t *)tb_buf)[2]);
    latx_bare_uart_puts("\nbare: run\n");
    (void)latx_run_body(tb_buf, &env, epi_buf);
    latx_bare_uart_puts("bare: rax=");
    puthex(env.regs[R_EAX]);
    latx_bare_uart_puts(" rbx=");
    puthex(env.regs[R_EBX]);
    latx_bare_uart_puts(" eip=");
    puthex(env.eip);
    latx_bare_uart_puts("\n");

    check(env.regs[R_EAX] == 0x2223, "mov+lea result");
    check(env.regs[R_EBX] == 0x2222, "rbx preserved");
    check(env.eip == 0x1000, "exit pc");

    latx_bare_uart_puts("bare: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("bare: done\n");
    for (;;) {
    }
}
