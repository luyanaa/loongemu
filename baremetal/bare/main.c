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

/* ---------------- software LBT (QEMU has no LBT execution) --------------- */
typedef struct { uint64_t scr[4]; uint32_t eflags; uint32_t top; int tm; } SwLbt;
static SwLbt sw_lbt;

#define EF_CF (1u << 0)
#define EF_PF (1u << 2)
#define EF_AF (1u << 4)
#define EF_ZF (1u << 6)
#define EF_SF (1u << 7)
#define EF_OF (1u << 11)

static uint32_t pack_flags(uint32_t eflags, uint32_t mask)
{
    uint32_t r = 0;

    if (mask & 1u) r |= (eflags & EF_CF);
    if (mask & 2u) r |= (eflags & EF_PF);
    if (mask & 4u) r |= (eflags & EF_AF);
    if (mask & 8u) r |= (eflags & EF_ZF);
    if (mask & 16u) r |= (eflags & EF_SF);
    if (mask & 32u) r |= (eflags & EF_OF);
    return r;
}

static void unpack_flags(uint32_t *eflags, uint64_t v, uint32_t mask)
{
    uint32_t f = *eflags;

    if (mask & 1u)  f = (f & ~EF_CF) | (uint32_t)(v & EF_CF);
    if (mask & 2u)  f = (f & ~EF_PF) | (uint32_t)(v & EF_PF);
    if (mask & 4u)  f = (f & ~EF_AF) | (uint32_t)(v & EF_AF);
    if (mask & 8u)  f = (f & ~EF_ZF) | (uint32_t)(v & EF_ZF);
    if (mask & 16u) f = (f & ~EF_SF) | (uint32_t)(v & EF_SF);
    if (mask & 32u) f = (f & ~EF_OF) | (uint32_t)(v & EF_OF);
    *eflags = f;
}

static int is_lbt_word(uint32_t w)
{
    uint32_t b0 = w & 0xff, b1 = (w >> 8) & 0xff;
    uint32_t b2 = (w >> 16) & 0xff, b3 = (w >> 24) & 0xff;

    if (b3 != 0) return 0;
    if (b2 == 0x5c) return (b1 & 3) == 0;
    if (b2 != 0x00) return 0;
    if (b1 == 0x70) return (b0 & 0x1f) == 0;
    if (b1 == 0x74) return (b0 & 0xe0) == 0;
    if (b1 == 0x80) return b0 == 0x09 || b0 == 0x29 || b0 == 0x08 || b0 == 0x28;
    return 0;
}

extern void lbt_entry(void);

void lbt_handle(uint64_t *regs)
{
    uint64_t era, estat;
    uint32_t w, b0, b1, b2, rd, imm;
    int handled = 0;

    __asm__ volatile("csrrd %0, 0x6" : "=r"(era));
    __asm__ volatile("csrrd %0, 0x5" : "=r"(estat));
    if (((estat >> 16) & 0x3f) != 0x0d) {
        latx_bare_uart_puts("trap is=");
        puthex((estat >> 16) & 0x3f);
        puthex(estat);
        puthex(era);
        latx_bare_uart_puts("\n");
        for (;;) {
        }
    }
    w = *(volatile uint32_t *)(uintptr_t)era;
    b0 = w & 0xff; b1 = (w >> 8) & 0xff; b2 = (w >> 16) & 0xff;
    rd = b0 & 0x1f;
    imm = (b1 >> 2) & 0x3f;

    if (b2 == 0x5c) {
        if ((b0 & 0x20) == 0) { regs[rd] = pack_flags(sw_lbt.eflags, imm); handled = 1; }
        else { unpack_flags(&sw_lbt.eflags, regs[rd], imm); handled = 1; }
    } else if (b2 == 0x00 && b1 == 0x74) { regs[rd] = sw_lbt.top; handled = 1; }
    else if (b2 == 0x00 && b1 == 0x70) { sw_lbt.top = (b0 >> 5) & 7; handled = 1; }
    else if (b2 == 0x00 && b1 == 0x80) {
        if (b0 == 0x09) { sw_lbt.top = (sw_lbt.top + 1) & 7; handled = 1; }
        else if (b0 == 0x29) { sw_lbt.top = (sw_lbt.top - 1) & 7; handled = 1; }
        else if (b0 == 0x08) { sw_lbt.tm = 1; handled = 1; }
        else if (b0 == 0x28) { sw_lbt.tm = 0; handled = 1; }
    }
    if (!handled) {
        latx_bare_uart_puts("lbt unsupported era=");
        puthex(era);
        latx_bare_uart_puts(" w=");
        puthex(w);
        latx_bare_uart_puts("\n");
    }
    __asm__ volatile("csrwr %0, 0x6" :: "r"(era + 4));
}

__asm__(
".text\n.globl lbt_entry\nlbt_entry:\n"
"  addi.d $sp,$sp,-264\n"
"  st.d $ra,$sp,8\n  st.d $tp,$sp,16\n  st.d $r3,$sp,24\n  st.d $r4,$sp,32\n"
"  st.d $r5,$sp,40\n  st.d $r6,$sp,48\n  st.d $r7,$sp,56\n  st.d $r8,$sp,64\n"
"  st.d $r9,$sp,72\n  st.d $r10,$sp,80\n  st.d $r11,$sp,88\n  st.d $r12,$sp,96\n"
"  st.d $r13,$sp,104\n st.d $r14,$sp,112\n st.d $r15,$sp,120\n st.d $r16,$sp,128\n"
"  st.d $r17,$sp,136\n st.d $r18,$sp,144\n st.d $r19,$sp,152\n st.d $r20,$sp,160\n"
"  st.d $r21,$sp,168\n st.d $r22,$sp,176\n st.d $r23,$sp,184\n st.d $r24,$sp,192\n"
"  st.d $r25,$sp,200\n st.d $r26,$sp,208\n st.d $r27,$sp,216\n st.d $r28,$sp,224\n"
"  st.d $r29,$sp,232\n st.d $r30,$sp,240\n st.d $r31,$sp,248\n"
"  move $a0,$sp\n  bl lbt_handle\n"
"  ld.d $ra,$sp,8\n  ld.d $tp,$sp,16\n  ld.d $r3,$sp,24\n  ld.d $r4,$sp,32\n"
"  ld.d $r5,$sp,40\n  ld.d $r6,$sp,48\n  ld.d $r7,$sp,56\n  ld.d $r8,$sp,64\n"
"  ld.d $r9,$sp,72\n  ld.d $r10,$sp,80\n  ld.d $r11,$sp,88\n  ld.d $r12,$sp,96\n"
"  ld.d $r13,$sp,104\n ld.d $r14,$sp,112\n ld.d $r15,$sp,120\n ld.d $r16,$sp,128\n"
"  ld.d $r17,$sp,136\n ld.d $r18,$sp,144\n ld.d $r19,$sp,152\n ld.d $r20,$sp,160\n"
"  ld.d $r21,$sp,168\n ld.d $r22,$sp,176\n ld.d $r23,$sp,184\n ld.d $r24,$sp,192\n"
"  ld.d $r25,$sp,200\n ld.d $r26,$sp,208\n ld.d $r27,$sp,216\n ld.d $r28,$sp,224\n"
"  ld.d $r29,$sp,232\n ld.d $r30,$sp,240\n ld.d $r31,$sp,248\n"
"  addi.d $sp,$sp,264\n  ertn\n"
);

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
        __asm__ volatile("csrwr %0, 0xc" :: "r"((uint64_t)lbt_entry));
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
