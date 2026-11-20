/*
 * Bare-metal LATM stage-1 test: translate MIPS64r2 integer code to LoongArch,
 * execute the generated block on the emulated CPU, and compare the guest
 * environment.  The prologue/epilogue are exercised too, so this checks the
 * same context-switch shape a real TB entry uses.
 */
#include "qemu/osdep.h"
#include <stdio.h>
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "cpu.h"
#include "latm.h"
#include "translate.h"
#include "fpu/softfloat.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s)
{
    while (*s) {
        latx_bare_uart_putc(*s++);
    }
}

static void puthex(uint64_t v)
{
    char b[17];
    for (int i = 15; i >= 0; i--) {
        b[15 - i] = "0123456789abcdef"[(v >> (i * 4)) & 0xf];
    }
    b[16] = 0;
    latx_bare_uart_puts(b);
}

static int checks, fails;

static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        latx_bare_uart_puts("latm-isa: FAIL ");
        latx_bare_uart_puts(what);
        latx_bare_uart_puts(" got=");
        puthex(got);
        latx_bare_uart_puts(" want=");
        puthex(want);
        latx_bare_uart_puts("\n");
    }
}

uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];

#define CODE_BUF_SIZE (1 << 16)
static uint8_t code_buf[CODE_BUF_SIZE] __attribute__((aligned(4096)));
#define PROLOGUE_OFFSET 0x1000
#define EPILOGUE_OFFSET 0x3000
#define TB_OFFSET       0x5000

static TranslationBlock tb;
static CPUMIPSState env;

/* daddiu $2,$0,5; daddiu $3,$0,7; daddu $4,$2,$3; dsubu $5,$4,$2;
 * dsll $6,$5,3; dsra $7,$6,1 */
static uint32_t arith_code[] = {
    0x64020005, 0x64030007, 0x0043202d, 0x0082282f, 0x000530f8, 0x0006387b,
};

/* daddiu $2,$0,1; daddiu $3,$0,1; beq $2,$3,target;
 * daddiu $4,$0,0x22 (delay); daddiu $5,$0,0x33 (skipped);
 * target: daddiu $6,$0,0x44 */
static uint32_t branch_taken_code[] = {
    0x64020001, 0x64030001, 0x10430002, 0x64040022, 0x64050033, 0x64060044,
};

/* daddiu $8,$0,1; daddiu $9,$0,1; bne $8,$9,skip;
 * daddiu $10,$0,0x22 (delay); daddiu $11,$0,0x33 (fallthrough); skip: nop */
static uint32_t branch_not_code[] = {
    0x64080001, 0x64090001, 0x15090002, 0x640a0022, 0x640b0033, 0x00000000,
};

/* daddiu $2,$0,6; daddiu $3,$0,7; dmultu $2,$3; mflo $4; mfhi $5;
 * daddiu $6,$0,0x123; rotr $7,$6,8; drotr $8,$6,12 */
static uint32_t hilo_shift_code[] = {
    0x64020006, 0x64030007, 0x0043001d, 0x00002012, 0x00002810,
    0x64060123, 0x00263a02, 0x0026433a,
};

/* jal target; daddiu $9,$0,0x22 (delay); daddiu $10,$0,0x33 (skipped);
 * target: daddiu $11,$0,0x44 */
static uint32_t jal_code[] = {
    0x64020001, 0x0c000000, 0x64090022, 0x640a0033, 0x640b0044,
};

/* beql $2,$2,target; daddiu $13,$0,0x22 (delay); daddiu $14,$0,0x33;
 * target: daddiu $15,$0,0x44 */
static uint32_t likely_taken_code[] = {
    0x50420002, 0x640d0022, 0x640e0033, 0x640f0044,
};

/* beql $2,$3,target; daddiu $16,$0,0x22 (must be skipped);
 * daddiu $17,$0,0x33 (fallthrough); target: nop */
static uint32_t likely_not_code[] = {
    0x64020001, 0x64030002, 0x50430002, 0x64100022, 0x64110033, 0x00000000,
};

/* Store/load word, LWL+LWR, store/load double, LDL+LDR, SDL+LD. */
static uint32_t mem_code[] = {
    0x64020400, 0x3c031122, 0x34633344, 0xac430000, 0x64040000,
    0x98440000, 0x88440003, 0x3c051122, 0x34a53344, 0x00052c38,
    0x34a55566, 0x00052c38, 0x34a57788, 0xfc450008, 0x64060000,
    0x6c460008, 0x6846000f, 0xb4450010, 0xdc470010,
};

/* 1.5 + 2.25 through LDC1/add.d/SDC1. */
static uint32_t fpu_code[] = {
    0x64020400, 0x3c033ff8, 0x0003183c, 0xfc430000,
    0x3c054002, 0x0005283c, 0xfc450008, 0xd4410000,
    0xd4420008, 0x46220800, 0xf4400010, 0xdc440010,
};

/* lui/ori load the address of jr_code+4 into $2; jr $2; delay. */
static uint32_t jr_code[] = {
    0x3c020000, 0x34420000, 0x00400008, 0x64030022,
};

static void enable_fpu(void)
{
    uint64_t euen;
    __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
    euen |= 1u << 0; /* EUEN.FPE */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
}

extern void lata_tr_data_init(void *env);
extern int lata_gen_prologue(TCGContext *tcg_ctx);
extern int lata_gen_epilogue(TCGContext *tcg_ctx);
extern int tr_ir2_assemble(const void *code_start_addr);
extern bool tr_ir2_generate(TranslationBlock *tb);
extern void tr_init(void *tb);
extern void tr_fini(void);
extern uint64_t context_switch_bt_to_native;

static void run_block(uint32_t *guest, int insns, const char *name)
{
    uint8_t *tb_code = code_buf + TB_OFFSET;

    latx_bare_uart_puts("latm-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" begin\n");

    for (size_t i = 0; i < sizeof(env); i++) {
        ((uint8_t *)&env)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    for (size_t i = 0; i < CODE_BUF_SIZE; i++) {
        code_buf[i] = 0;
    }

    env.active_tc.PC = (uint64_t)(uintptr_t)guest;

    /* The prologue/epilogue live in their own slots; the TB gets a clean one. */
    tcg_ctx->code_ptr = code_buf + PROLOGUE_OFFSET;
    lata_gen_prologue(tcg_ctx);
    tcg_ctx->code_ptr = code_buf + EPILOGUE_OFFSET;
    lata_gen_epilogue(tcg_ctx);

    tcg_ctx->code_ptr = tb_code;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;

    tb.pc = (uint64_t)(uintptr_t)guest;
    tb.tc.ptr = tb_code;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;

    int max_insns = insns;
    target_disasm(&tb, &max_insns, NULL);
    if (tb.icount == 0) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" decoded=0\n");
        return;
    }

    if (!tr_ir2_generate(&tb)) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" ir2_generate\n");
        tr_fini();
        return;
    }
    int n = tr_ir2_assemble(tb_code);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    if (n <= 0) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" assemble\n");
        return;
    }

    ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(
        &env, tb_code);

    latx_bare_uart_puts("latm-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" done\n");
}

void kmain(void)
{
    latx_bare_uart_puts("latm-isa: begin\n");

    lata_tr_data_init(&env);

    run_block(arith_code, 6, "arith");
    check(env.active_tc.gpr[4], 12, "arith daddu");
    check(env.active_tc.gpr[5], 7, "arith dsubu");
    check(env.active_tc.gpr[6], 56, "arith dsll");
    check(env.active_tc.gpr[7], 28, "arith dsra");

    run_block(branch_taken_code, 6, "branch-taken");
    check(env.active_tc.gpr[4], 0x22, "branch delay slot executed");
    check(env.active_tc.gpr[5], 0, "branch skipped instruction not executed");
    check(env.active_tc.PC,
          (uint64_t)(uintptr_t)(branch_taken_code + 5), "branch target PC");

    run_block(branch_not_code, 6, "branch-not-taken");
    check(env.active_tc.gpr[10], 0x22, "not-taken delay slot executed");
    check(env.active_tc.gpr[11], 0, "not-taken fallthrough not in TB");
    check(env.active_tc.PC,
          (uint64_t)(uintptr_t)(branch_not_code + 4), "not-taken PC");

    run_block(hilo_shift_code, 8, "hilo-shift");
    check(env.active_tc.gpr[4], 42, "dmultu lo");
    check(env.active_tc.gpr[5], 0, "dmultu hi");
    check(env.active_tc.gpr[7], 0x23000001u, "rotr 32");
    check(env.active_tc.gpr[8], 0x1230000000000000ull, "drotr 64");

    /* JAL's 26-bit field is relative to the guest image, so patch it. */
    {
        uint32_t pc = (uint32_t)(uintptr_t)jal_code;
        uint32_t target = (uint32_t)(uintptr_t)(jal_code + 4);
        jal_code[1] = 0x0c000000u | ((target >> 2) & 0x03ffffffu);
        (void)pc;
    }
    run_block(jal_code, 5, "jal");
    check(env.active_tc.gpr[9], 0x22, "jal delay slot executed");
    check(env.active_tc.gpr[10], 0, "jal skipped instruction");
    check(env.active_tc.gpr[31], (uint64_t)(uintptr_t)(jal_code + 3),
          "jal link = branch+8");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(jal_code + 4),
          "jal target PC");

    run_block(likely_taken_code, 4, "likely-taken");
    check(env.active_tc.gpr[13], 0x22, "likely taken delay slot");
    check(env.active_tc.gpr[14], 0, "likely taken skip");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(likely_taken_code + 3),
          "likely taken PC");

    run_block(likely_not_code, 6, "likely-not-taken");
    check(env.active_tc.gpr[16], 0, "likely not-taken delay slot skipped");
    check(env.active_tc.gpr[17], 0, "likely not-taken fallthrough not in TB");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(likely_not_code + 4),
          "likely not-taken PC");

    run_block(mem_code, 19, "memory");
    check(env.active_tc.gpr[4], 0x11223344u, "lwl+lwr word");
    check(env.active_tc.gpr[6], 0x1122334455667788ull, "ldl+ldr double");
    check(env.active_tc.gpr[7], 0x1122334455667788ull, "sdl+ld double");

    {
        uint32_t target = (uint32_t)(uintptr_t)(jr_code + 4);
        jr_code[0] = 0x3c020000u | ((target >> 16) & 0xffffu);
        jr_code[1] = 0x34420000u | (target & 0xffffu);
    }
    run_block(jr_code, 4, "jr");
    check(env.active_tc.gpr[3], 0x22, "jr delay slot executed");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(jr_code + 4), "jr target PC");

    enable_fpu();
    run_block(fpu_code, 12, "fpu-add");
    check(env.active_tc.gpr[4], 0x400e000000000000ull, "1.5 + 2.25");

    latx_bare_uart_puts("latm-isa: checks=");
    puthex(checks);
    latx_bare_uart_puts(" fails=");
    puthex(fails);
    if (fails == 0) {
        latx_bare_uart_puts(" PASS\n");
    } else {
        latx_bare_uart_puts(" FAIL\n");
    }
    latx_bare_uart_puts("latm-isa: done\n");
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");

static char kstack[65536] __attribute__((aligned(16), used));
struct CPUState;
void cpu_loop_exit(struct CPUState *cs) { (void)cs; latx_bare_uart_puts("latm-isa: cpu_loop_exit\n"); for (;;) { } }
FILE *stderr;
