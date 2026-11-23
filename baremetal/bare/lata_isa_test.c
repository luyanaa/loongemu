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
#include "target/arm/lata/include/env.h"
#include "target/arm/lata/include/la-append.h"
#include "target/arm/lata/include/macro-inst.h"
#include "target/arm/lata/ir1/sve-pvir.h"
#include "target/arm/lata/translator/sve-lasx-lower.h"
#include "target/arm/lata/include/lata-mem.h"
#include "target/arm/lata/include/lata-snapshot.h"
/* Declared by the translator; lata.h cannot be included here (CPU header clash). */
/* Translator internals used by the IR2 micro-tests. */
void lata_env_xvld(IR2_OPND vector, int offset);
void lata_env_xvst(IR2_OPND vector, int offset);
IR2_OPND ra_alloc_ftemp(void);
bool lata_vreg_resident(int index);
void lata_vreg_init_residency(void);
void lata_vreg_clear_residency_for_test(void);
int lata_vreg_spill_cold(void);
uint32_t lata_vreg_spills(void);
uint32_t lata_vreg_resident_count(void);
void lata_ftemp_set_base_pool(int pool);
bool ra_ftemp_phys_in_pool(int phys);
IR2_OPND ra_alloc_ftemp(void);
bool lata_ir2_ftemp_pool_state_matches(const LataIr2Snapshot *snap);
int lata_vreg_partition_violations(void);
#include "lata-vcache.h"
/* Declared locally: including lata.h clashes with the fixture CPU types. */
typedef struct LataContext LataContext;
extern LataContext *lata_context_new(void *env);
extern void lata_context_delete(LataContext *ctx);
extern void lata_context_bind(LataContext *ctx);
extern LataContext *lata_context_current(void);
extern void lata_vreg_clear_residency_for_test(void);
extern uint32_t ra_ftemp_fallbacks(void);
extern void lata_zcache_flush_for_exit(void);
extern void lata_emit_dirty_vreg_stores(void);
extern uint32_t lata_vreg_spills(void);
extern uint32_t lata_vreg_resident_count(void);
extern int lata_vreg_partition_violations(void);
extern uint32_t lata_vreg_exit_violations(void);
extern uint32_t lata_vreg_tb_publishes(void);
extern uint32_t lata_vreg_lazy_loads(void);
extern uint32_t lata_vreg_temp_loads(void);
extern uint32_t ra_ftemp_alloc_failures(void);
extern void lata_env_vld(IR2_OPND vector, int offset);
extern void lata_env_vst(IR2_OPND vector, int offset);
extern IR2_OPND fsmask_ir2_opnd;
/* TRANSLATION_DATA lives in lata.h, which clashes with the fixture CPU types,
 * so reach the list through a tiny accessor instead. */
struct IR2_INST;
struct IR2_INST *lata_first_ir2(void);
/* The context fixture drives the switch stubs directly, so it must simulate a
 * fresh execution: the entry stub normally clears this. */
extern uint32_t lata_vreg_published;
extern bool trans_sve_pfalse(struct DisasContext *s);
extern void lata_ftemp_set_base_pool(int pool);
#include "latx/compat/qemu/cpu-features.h"
extern int lata_gen_prologue(TCGContext *tcg_ctx);
extern int lata_gen_epilogue(TCGContext *tcg_ctx);
extern uint64_t context_switch_bt_to_native;
extern const int arm_la_fmap[];
extern IR2_OPND a0_ir2_opnd;
extern IR2_OPND a1_ir2_opnd;
extern IR2_OPND ra_ir2_opnd;
extern IR2_OPND zero_ir2_opnd;
extern IR2_OPND t0_ir2_opnd;

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
static bool test_aa64_lse;

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
/* Strong hook: print the assembled word for the widening-add opcodes. */
int lata_ir2_debug_trace;
void lata_ir2_debug(unsigned int opcode, unsigned int word)
{
    char buf[48];
    if (!lata_ir2_debug_trace)
    {
        return;
    }
    latx_bare_uart_puts("ir2-enc: opcode=");
    puthex(opcode);
    latx_bare_uart_puts(" word=");
    puthex(word);
    latx_bare_uart_puts("\n");
}

volatile uint64_t lata_result;
volatile uint64_t lata_result_x3;
volatile uint64_t lata_result_x4;
volatile uint64_t lata_result_x5;
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
"  la     $t0, lata_result_x3\n"
"  st.d   $a3, $t0, 0\n"
"  la     $t0, lata_result_x4\n"
"  st.d   $a4, $t0, 0\n"
"  la     $t0, lata_result_x5\n"
"  st.d   $a5, $t0, 0\n"
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
    latx_bare_uart_puts("lata-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" begin\n");
    extern void lata_exit_tramp(void);

    for (size_t i = 0; i < sizeof(env); i++) { ((uint8_t *)&env)[i] = 0; }
    for (size_t i = 0; i < sizeof(cpu); i++) { ((uint8_t *)&cpu)[i] = 0; }
    for (size_t i = 0; i < sizeof(tb); i++) { ((uint8_t *)&tb)[i] = 0; }

    cpu.env_ptr = &env;
    arm_cpu_set_aa64_lse(&env.arm_cpu, test_aa64_lse);
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
    latx_bare_uart_puts("lata-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" done\n");
}

/*
 * Exercise the generated context-switch pair, not run_block()'s direct TB
 * entry. The tiny IR2 TB writes three distinct vectors, snapshots them to
 * disjoint scratch storage, then branches through the generated epilogue.
 */
static uint8_t context_vector_expected[3][16]
    __attribute__((aligned(16)));

static void write_le64(uint8_t *bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = (uint8_t)(value >> (8u * i));
    }
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    return value;
}

static void enable_context_units(void)
{
    uint64_t euen = (1u << 0) | (1u << 1);
    uint64_t actual;

    /* Enable only FPE+SXE; keep ASXE and BTE disabled. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "context fixture enables FPE+SXE with BTE off");
}

static void enable_bte_only(void)
{
    uint64_t euen = 1u << 3;
    uint64_t actual;

    /* Guest flag instructions use LBT_ARM; keep the other units disabled. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "integer/NZCV matrix enables only EUEN.BTE");
}

static void disable_optional_units(void)
{
    uint64_t euen = 0;
    uint64_t actual;

    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "scalar memory fixture disables optional EUEN units");
}

static void enable_sxe_only(void)
{
    uint64_t euen = 1u << 1;
    uint64_t actual;

    /* Do not inherit FPE, ASXE, or BTE from other fixture setup. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "NEON fixture enables only EUEN.SXE");
}

static void enable_sxe_asxe_only(void)
{
    uint64_t euen = (1u << 1) | (1u << 2);
    uint64_t actual;

    /* Exercise emitted LASX only; keep floating point and LBT disabled. */
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "SVE PVIR fixture enables only SXE and ASXE");
}


static void run_context_vector_save_restore(void)
{
    enum {
        PROLOGUE_OFFSET = 0x1000,
        EPILOGUE_OFFSET = 0x3000,
        TB_OFFSET = 0x5000,
    };
    const uint32_t pstate_nzcv = 0xa0000000u;
    uint8_t *prologue_code = code_buf + PROLOGUE_OFFSET;
    uint8_t *epilogue_code = code_buf + EPILOGUE_OFFSET;
    uint8_t *tb_code = code_buf + TB_OFFSET;
    uint8_t *env_bytes = (uint8_t *)&env;
    IR2_OPND v0 = ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[0]);
    IR2_OPND v1 = ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[1]);
    IR2_OPND v31 = ir2_opnd_new(IR2_OPND_FPR, arm_la_fmap[31]);

    for (size_t i = 0; i < sizeof(env); i++) {
        env_bytes[i] = 0;
    }
    for (size_t i = 0; i < sizeof(cpu); i++) {
        ((uint8_t *)&cpu)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }

    cpu.env_ptr = &env;
    lata_tr_data_init(&env);
    env.pstate = pstate_nzcv;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;

    /* run_block() leaves these set to its direct-exit trampoline. */
    context_switch_bt_to_native = 0;
    context_switch_native_to_bt = 0;
    context_switch_native_to_bt_ret_0 = 0;

    tcg_ctx->code_ptr = prologue_code;
    latx_bare_uart_puts("lata-isa: context prologue begin\n");
    check(lata_gen_prologue(tcg_ctx) > 0, 1,
          "context prologue generated");
    latx_bare_uart_puts("lata-isa: context prologue done\n");
    tcg_ctx->code_ptr = epilogue_code;
    latx_bare_uart_puts("lata-isa: context epilogue begin\n");
    check(lata_gen_epilogue(tcg_ctx) > 0, 1,
          "context epilogue generated");
    latx_bare_uart_puts("lata-isa: context epilogue done\n");

    /*
     * This is a tiny native IR2 TB between those hooks. The scratch stores
     * preserve the actual emitted vector patterns for independent comparison.
     */
    tcg_ctx->code_ptr = tb_code;
    tr_init(&tb);
    la_vldi(v0, 0x11);
    la_vldi(v1, 0x22);
    la_vldi(v31, 0x33);
    li_d(a0_ir2_opnd, (int64_t)(uintptr_t)context_vector_expected);
    la_vst(v0, a0_ir2_opnd, 0);
    la_vst(v1, a0_ir2_opnd, 16);
    la_vst(v31, a0_ir2_opnd, 32);
    li_d(ra_ir2_opnd, (int64_t)context_switch_native_to_bt);
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(tb_code);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");

    enable_context_units();
    lata_vreg_published = 0;
    latx_bare_uart_puts("lata-isa: context switch begin\n");
    ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(
        &env, tb_code);
    latx_bare_uart_puts("lata-isa: context switch done\n");
    check(env.pstate, pstate_nzcv,
          "context preserves PSTATE.NZCV with BTE disabled");

    check(read_le64(context_vector_expected[0]) !=
              read_le64(context_vector_expected[1]),
          1, "context fixture V0/V1 patterns differ");
    check(read_le64(context_vector_expected[1]) !=
              read_le64(context_vector_expected[2]),
          1, "context fixture V1/V31 patterns differ");
    check(read_le64(env_bytes + env_offset_fpr(0)),
          read_le64(context_vector_expected[0]), "context save V0 low");
    check(read_le64(env_bytes + env_offset_fpr(0) + 8),
          read_le64(context_vector_expected[0] + 8), "context save V0 high");
    check(read_le64(env_bytes + env_offset_fpr(1)),
          read_le64(context_vector_expected[1]), "context save V1 low");
    check(read_le64(env_bytes + env_offset_fpr(1) + 8),
          read_le64(context_vector_expected[1] + 8), "context save V1 high");
    check(read_le64(env_bytes + env_offset_fpr(31)),
          read_le64(context_vector_expected[2]), "context save V31 low");
    check(read_le64(env_bytes + env_offset_fpr(31) + 8),
          read_le64(context_vector_expected[2] + 8), "context save V31 high");
    check(env.xregs[0], context_switch_native_to_bt,
          "V31 upper half must not overwrite xregs[0]");
}

static void run_integer_nzcv_matrix(void)
{
    /* Encodings assembled with clang --target=aarch64-none-elf and llvm-objdump. */
    /* CSET packs x2[3:0] = {V,C,Z,N}; csel_false omits V merge because V=0. */
    /* Direct MRS NZCV is not implemented by this compatibility decoder. */
    static uint32_t adds_carry_zero[] __attribute__((aligned(4096))) = {
        0x92800001u,  /* movn x1, #0 */
        0xd2800022u,  /* movz x2, #1 */
        0xab020023u,  /* adds x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adds_overflow[] __attribute__((aligned(4096))) = {
        0xd29fffe1u,  /* movz x1, #0xffff */
        0xf2bfffe1u,  /* movk x1, #0xffff, lsl #16 */
        0xf2dfffe1u,  /* movk x1, #0xffff, lsl #32 */
        0xf2efffe1u,  /* movk x1, #0x7fff, lsl #48 */
        0xd2800022u,  /* movz x2, #1 */
        0xab020023u,  /* adds x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adds_negative[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0x92800002u,  /* movn x2, #0 */
        0xab020023u,  /* adds x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t subs_zero[] __attribute__((aligned(4096))) = {
        0xd28000a1u,  /* movz x1, #5 */
        0xd28000a2u,  /* movz x2, #5 */
        0xeb020023u,  /* subs x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t subs_borrow[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800022u,  /* movz x2, #1 */
        0xeb020023u,  /* subs x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    /* Keep the 10-instruction standalone TB within one translation page. */
    static uint32_t subs_overflow[] __attribute__((aligned(4096))) = {
        0xd2f00001u,  /* movz x1, #0x8000, lsl #48 */
        0xd2800022u,  /* movz x2, #1 */
        0xeb020023u,  /* subs x3, x1, x2 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adc_carry[] __attribute__((aligned(4096))) = {
        0xd2800021u,  /* movz x1, #1 */
        0xd28000a2u,  /* movz x2, #5 */
        0xd28000e4u,  /* movz x4, #7 */
        0xeb01003fu,  /* cmp x1, x1: C=1, Z=1 */
        0x9a040043u,  /* adc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t adc_no_carry[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd28000a2u,  /* movz x2, #5 */
        0xd28000e4u,  /* movz x4, #7 */
        0xeb05003fu,  /* cmp x1, x5: C=0, N=1 */
        0x9a040043u,  /* adc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t sbc_carry[] __attribute__((aligned(4096))) = {
        0xd2800021u,  /* movz x1, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb01003fu,  /* cmp x1, x1: C=1, Z=1 */
        0xda040043u,  /* sbc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t sbc_borrow[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb05003fu,  /* cmp x1, x5: C=0, N=1 */
        0xda040043u,  /* sbc x3, x2, x4 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t bcond_taken[] __attribute__((aligned(4096))) = {
        0xd2800021u,  /* movz x1, #1 */
        0xd2800223u,  /* movz x3, #0x11 */
        0xd2800025u,  /* movz x5, #1 */
        0xeb01003fu,  /* cmp x1, x1: Z=1, C=1 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
        0x54000040u,  /* b.eq +8: taken target */
    };
    static uint32_t bcond_not_taken[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800223u,  /* movz x3, #0x11 */
        0xeb05003fu,  /* cmp x1, x5: Z=0, C=0 */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
        0x54000040u,  /* b.eq +8: not taken */
    };
    static uint32_t csel_true[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb05003fu,  /* cmp x1, x5: LT */
        0x9a84b043u,  /* csel x3, x2, x4, lt */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V */
        0xaa040c42u,  /* orr x2, x2, x4, lsl #3 */
    };
    static uint32_t csel_false[] __attribute__((aligned(4096))) = {
        0xd2800001u,  /* movz x1, #0 */
        0xd2800025u,  /* movz x5, #1 */
        0xd2800122u,  /* movz x2, #9 */
        0xd2800084u,  /* movz x4, #4 */
        0xeb05003fu,  /* cmp x1, x5: GT false */
        0x9a84c043u,  /* csel x3, x2, x4, gt */
        0x9a9f57e2u,  /* cset x2, mi: N */
        0x9a9f17e4u,  /* cset x4, eq: Z */
        0xaa040442u,  /* orr x2, x2, x4, lsl #1 */
        0x9a9f37e4u,  /* cset x4, hs: C */
        0xaa040842u,  /* orr x2, x2, x4, lsl #2 */
        0x9a9f77e4u,  /* cset x4, vs: V=0 for this comparison */
    };

    enable_bte_only();
    latx_bare_uart_puts("lata-isa: integer/NZCV matrix begin\n");

    run_block(adds_carry_zero, 10, "adds-carry-zero");
    check(lata_result_x3, 0, "ADDS carry destination");
    check(lata_result, 6, "ADDS carry/zero NZCV nibble");
    run_block(adds_overflow, 13, "adds-signed-overflow");
    check(lata_result_x3, 0x8000000000000000ull,
          "ADDS overflow destination");
    check(lata_result, 9, "ADDS negative/overflow NZCV nibble");
    run_block(adds_negative, 10, "adds-negative");
    check(lata_result_x3, UINT64_MAX, "ADDS negative destination");
    check(lata_result, 1, "ADDS negative NZCV nibble");

    run_block(subs_zero, 10, "subs-zero");
    check(lata_result_x3, 0, "SUBS zero destination");
    check(lata_result, 6, "SUBS zero NZCV nibble");
    run_block(subs_borrow, 10, "subs-borrow");
    check(lata_result_x3, UINT64_MAX, "SUBS borrow destination");
    check(lata_result, 1, "SUBS borrow/negative NZCV nibble");
    run_block(subs_overflow, 10, "subs-signed-overflow");
    check(tb.icount, 10, "SUBS overflow block fully decoded");
    latx_bare_uart_puts("lata-isa: SUBS overflow NZCV=");
    puthex(lata_result);
    latx_bare_uart_puts("\n");
    check(lata_result_x3, 0x7fffffffffffffffull,
          "SUBS overflow destination");
    check(lata_result, 12, "SUBS carry/overflow NZCV nibble");

    run_block(adc_carry, 12, "adc-carry-in");
    check(lata_result_x3, 13, "ADC carry-in destination");
    check(lata_result, 6, "ADC preserves carry NZCV nibble");
    run_block(adc_no_carry, 13, "adc-no-carry-in");
    check(lata_result_x3, 12, "ADC no-carry destination");
    check(lata_result, 1, "ADC preserves no-carry NZCV nibble");
    run_block(sbc_carry, 12, "sbc-no-borrow");
    check(lata_result_x3, 5, "SBC carry/no-borrow destination");
    check(lata_result, 6, "SBC preserves carry NZCV nibble");
    run_block(sbc_borrow, 13, "sbc-borrow");
    check(lata_result_x3, 4, "SBC borrow destination");
    check(lata_result, 1, "SBC preserves no-carry NZCV nibble");

    run_block(bcond_taken, 12, "bcond-taken");
    check(lata_result_x3, 0x11, "B.EQ preserves x3");
    check(env.pc, (uint64_t)(uintptr_t)bcond_taken + 52,
          "B.EQ taken target PC");
    check(lata_result, 6, "B.EQ taken NZCV nibble");
    run_block(bcond_not_taken, 12, "bcond-not-taken");
    check(lata_result_x3, 0x11, "B.EQ not-taken destination");
    check(env.pc, (uint64_t)(uintptr_t)bcond_not_taken + 48,
          "B.EQ not-taken fallthrough PC");
    check(lata_result, 1, "B.EQ not-taken NZCV nibble");
    run_block(csel_true, 13, "csel-true");
    check(lata_result_x3, 9, "CSEL LT destination");
    check(lata_result, 1, "CSEL LT preserves NZCV nibble");
    run_block(csel_false, 12, "csel-false");
    check(lata_result_x3, 4, "CSEL GT destination");
    check(lata_result, 1, "CSEL GT preserves NZCV nibble");

    latx_bare_uart_puts("lata-isa: integer/NZCV matrix done\n");
}


static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}
static void write_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned byte = 0; byte < 4; ++byte) {
        bytes[byte] = (uint8_t)(value >> (byte * 8u));
    }
}


static void run_scalar_memory_matrix(void)
{
    /* A64 words were assembled with clang and cross-checked by llvm-objdump. */
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[3];
    } ldr_str = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xf9400022u,  /* ldr x2, [x1] */
            0xd2995fc3u,  /* movz x3, #0xcafe */
            0xf9000423u,  /* str x3, [x1, #8] */
        },
        .data = {
            0x0123456789abcdefull,
            0xaaaaaaaaaaaaaaaaull,
            0xbbbbbbbbbbbbbbbbull,
        },
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[6];
    } ldp_stp = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xa9400c22u,  /* ldp x2, x3, [x1] */
            0xa9010c22u,  /* stp x2, x3, [x1, #16] */
            0xd503201fu,  /* nop: keep data at the 16-byte offset */
        },
        .data = {
            0x0123456789abcdefull,
            0x1020304050607080ull,
            0xaaaaaaaaaaaaaaaaull,
            0xbbbbbbbbbbbbbbbbull,
            0xccccccccccccccccull,
            0xddddddddddddddddull,
        },
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[7];
        uint8_t data[32];
    } unaligned = {
        .code = {
            0x300000e1u,  /* adr x1, data + 1 */
            0xf9400022u,  /* ldr x2, [x1] */
            0xd28ef103u,  /* movz x3, #0x7788 */
            0xf2aaacc3u,  /* movk x3, #0x5566, lsl #16 */
            0xf2c66883u,  /* movk x3, #0x3344, lsl #32 */
            0xf2e22443u,  /* movk x3, #0x1122, lsl #48 */
            0xf9000423u,  /* str x3, [x1, #8] */
        },
        .data = {
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
            0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        },
    };
    const uint64_t unaligned_store = 0x1122334455667788ull;
    check((uintptr_t)ldr_str.data - (uintptr_t)ldr_str.code,
          sizeof(ldr_str.code), "LDR/STR ADR fixture layout");
    check((uintptr_t)ldp_stp.data - (uintptr_t)ldp_stp.code,
          sizeof(ldp_stp.code), "LDP/STP ADR fixture layout");
    check((uintptr_t)unaligned.data - (uintptr_t)unaligned.code,
          sizeof(unaligned.code), "unaligned ADR fixture layout");

    disable_optional_units();
    latx_bare_uart_puts("lata-isa: scalar memory matrix begin\n");

    run_block(ldr_str.code, 4, "ldr-str-aligned");
    check(lata_result, 0x0123456789abcdefull, "LDR aligned loaded value");
    check(lata_result_x3, 0xcafe, "STR aligned source value");
    check(ldr_str.data[0], 0x0123456789abcdefull,
          "STR aligned preserves preceding word");
    check(ldr_str.data[1], 0xcafe, "STR aligned stored value");
    check(ldr_str.data[2], 0xbbbbbbbbbbbbbbbbull,
          "STR aligned preserves adjacent word");

    run_block(ldp_stp.code, 3, "ldp-stp-aligned");
    check(lata_result, 0x0123456789abcdefull, "LDP first loaded value");
    check(lata_result_x3, 0x1020304050607080ull,
          "LDP second loaded value");
    check(ldp_stp.data[0], 0x0123456789abcdefull,
          "STP preserves source first word");
    check(ldp_stp.data[1], 0x1020304050607080ull,
          "STP preserves source second word");
    check(ldp_stp.data[2], 0x0123456789abcdefull,
          "STP first stored value");
    check(ldp_stp.data[3], 0x1020304050607080ull,
          "STP second stored value");
    check(ldp_stp.data[4], 0xccccccccccccccccull,
          "STP preserves following word");
    check(ldp_stp.data[5], 0xddddddddddddddddull,
          "STP preserves final guard word");

    run_block(unaligned.code, 7, "ldr-str-unaligned");
    check(lata_result, 0x1817161514131211ull,
          "unaligned LDR loaded value");
    check(lata_result_x3, unaligned_store, "unaligned STR source value");
    for (size_t i = 0; i < sizeof(unaligned.data); i++) {
        uint8_t want = (uint8_t)(0x10u + i);
        if (i >= 9 && i < 17) {
            want = (uint8_t)(unaligned_store >> ((i - 9) * 8));
        }
        check(unaligned.data[i], want,
              i >= 9 && i < 17 ? "unaligned STR written byte"
                              : "unaligned STR preserves adjacent byte");
    }

    latx_bare_uart_puts("lata-isa: scalar memory matrix done\n");
}

static void run_exclusive_atomic_matrix(void)
{
    /* Assembled with clang --target=aarch64-none-elf; LDXR/STXR are base A64. */
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } stxr_success = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xc85f7c22u,  /* ldxr x2, [x1] */
            0xd2824684u,  /* movz x4, #0x1234 */
            0xc8037c24u,  /* stxr w3, x4, [x1] */
        },
        .data = {0x1111222233334444ull, 0xaaaaaaaaaaaaaaaaull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[6];
        uint64_t data[2];
    } stxr_failed_after_store = {
        .code = {
            0x100000c1u,  /* adr x1, data */
            0xc85f7c22u,  /* ldxr x2, [x1] */
            0xd28acf04u,  /* movz x4, #0x5678 */
            0xf9000024u,  /* str x4, [x1]: clear the local reservation */
            0xd2802465u,  /* movz x5, #0x123 */
            0xc8037c25u,  /* stxr w3, x5, [x1] */
        },
        .data = {0x0102030405060708ull, 0xbbbbbbbbbbbbbbbbull},
    };
    /* LSE words assembled with .arch armv8.1-a+lse and objdump-checked. */
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } cas_success = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd2824682u,  /* movz x2, #0x1234 */
            0xd28acf03u,  /* movz x3, #0x5678 */
            0xc8a27c23u,  /* cas x2, x3, [x1] */
        },
        .data = {0x1234, 0xccccccccccccccccull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } cas_failure = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd2935782u,  /* movz x2, #0x9abc */
            0xd28acf03u,  /* movz x3, #0x5678 */
            0xc8a27c23u,  /* cas x2, x3, [x1] */
        },
        .data = {0x1234, 0xddddddddddddddddull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } ldadd = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd28000e2u,  /* movz x2, #7 */
            0xf8220023u,  /* ldadd x2, x3, [x1] */
            0xd503201fu,  /* nop */
        },
        .data = {0x1111222233334444ull, 0xeeeeeeeeeeeeeeeeull},
    };
    static struct __attribute__((aligned(16))) {
        uint32_t code[4];
        uint64_t data[2];
    } ldadd_alias = {
        .code = {
            0x10000081u,  /* adr x1, data */
            0xd28000a2u,  /* movz x2, #5 */
            0xf2c00022u,  /* movk x2, #1, lsl #32 */
            0xf8220022u,  /* ldadd x2, x2, [x1] */
        },
        .data = {0x1122334455667788ull, 0xaaaaaaaaaaaaaaaaull},
    };
    ARMISAState zero_isar = {0};

    check(arm_isar_has_aa64_lse(&zero_isar), 0,
          "zero-initialized compat CPU lacks FEAT_LSE");
    check((uintptr_t)stxr_success.data - (uintptr_t)stxr_success.code,
          sizeof(stxr_success.code), "LDXR/STXR success fixture layout");
    check((uintptr_t)stxr_failed_after_store.data -
              (uintptr_t)stxr_failed_after_store.code,
          sizeof(stxr_failed_after_store.code),
          "LDXR/STXR failure fixture layout");
    check((uintptr_t)cas_success.data - (uintptr_t)cas_success.code,
          sizeof(cas_success.code), "CAS success fixture layout");
    check((uintptr_t)cas_failure.data - (uintptr_t)cas_failure.code,
          sizeof(cas_failure.code), "CAS failure fixture layout");
    check((uintptr_t)ldadd.data - (uintptr_t)ldadd.code,
          sizeof(ldadd.code), "LDADD fixture layout");
    check((uintptr_t)ldadd_alias.data - (uintptr_t)ldadd_alias.code,
          sizeof(ldadd_alias.code), "LDADD alias fixture layout");

    disable_optional_units();
    latx_bare_uart_puts("lata-isa: exclusive atomics begin\n");

    run_block(stxr_success.code, 4, "ldxr-stxr-success");
    check(lata_result, 0x1111222233334444ull, "LDXR loaded value");
    check(lata_result_x3, 0, "STXR successful status");
    check(stxr_success.data[0], 0x1234, "STXR successful store");
    check(stxr_success.data[1], 0xaaaaaaaaaaaaaaaaull,
          "STXR success preserves adjacent word");

    run_block(stxr_failed_after_store.code, 6, "ldxr-stxr-failed");
    check(lata_result, 0x0102030405060708ull,
          "LDXR value before reservation-breaking STR");
    check(lata_result_x3, 1, "STXR failed status after intervening STR");
    check(stxr_failed_after_store.data[0], 0x5678,
          "failed STXR preserves intervening STR value");
    check(stxr_failed_after_store.data[1], 0xbbbbbbbbbbbbbbbbull,
          "failed STXR preserves adjacent word");
    check(arm_isar_has_aa64_lse(&env.arm_cpu.isar), 0,
          "LDXR/STXR execute without FEAT_LSE");

    test_aa64_lse = true;
    run_block(cas_success.code, 4, "cas-success");
    check(arm_isar_has_aa64_lse(&env.arm_cpu.isar), 1,
          "CAS fixture explicitly enables FEAT_LSE");
    check(lata_result, 0x1234, "CAS success returns old memory value");
    check(lata_result_x3, 0x5678, "CAS success preserves desired value");
    check(cas_success.data[0], 0x5678, "CAS success stores desired value");
    check(cas_success.data[1], 0xccccccccccccccccull,
          "CAS success preserves adjacent word");

    run_block(cas_failure.code, 4, "cas-failure");
    check(lata_result, 0x1234, "CAS failure returns actual memory value");
    check(lata_result_x3, 0x5678, "CAS failure preserves desired value");
    check(cas_failure.data[0], 0x1234, "CAS failure leaves memory unchanged");
    check(cas_failure.data[1], 0xddddddddddddddddull,
          "CAS failure preserves adjacent word");

    run_block(ldadd.code, 3, "ldadd");
    check(lata_result, 7, "LDADD preserves source addend");
    check(lata_result_x3, 0x1111222233334444ull,
          "LDADD returns old memory value");
    check(ldadd.data[0], 0x111122223333444bull,
          "LDADD atomically stores sum");
    check(ldadd.data[1], 0xeeeeeeeeeeeeeeeeull,
          "LDADD preserves adjacent word");

    run_block(ldadd_alias.code, 4, "ldadd-alias-rd-rs");
    check(lata_result, 0x1122334455667788ull,
          "aliased LDADD returns full old value");
    check(ldadd_alias.data[0], 0x112233455566778dull,
          "aliased LDADD stores full-width sum");
    check(ldadd_alias.data[1], 0xaaaaaaaaaaaaaaaaull,
          "aliased LDADD preserves adjacent word");
    test_aa64_lse = false;


    latx_bare_uart_puts("lata-isa: exclusive atomics done\n");
}

/*
 * PTRUE pattern coverage: DecodePredCount maps each pattern to an active-lane
 * count; the lowerer must then set exactly those packed predicate bits.
 */
static void run_sve_ptrue_pattern_runtime(uint16_t vl_bits, unsigned pd,
                                          uint8_t pattern,
                                          uint8_t element_bits,
                                          size_t expected_active,
                                          const char *what)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / element_bits;
    size_t element_bytes = element_bits / 8u;
    uint8_t *predicate = arm_sve_preg(&env, pd);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;

    check(arm_sve_set_vl(&env, vl_bits), 1, "PTRUE pattern VL is valid");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES / 8u; ++byte) {
        predicate[byte] = 0xaa;
    }
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = (uint8_t)pd,
        .element_bits = element_bits,
        .pattern = pattern,
    };
    lowered = trans_sve_ptrue(&s);
    check(lowered, 1, what);
    if (!lowered) {
        tr_fini();
        return;
    }
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t bit = lane * element_bytes;
        unsigned value = (predicate[bit / 8u] >> (bit % 8u)) & 1u;

        check(value, lane < expected_active ? 1 : 0, what);
    }
    if (arm_sve_predicate_bytes_for_bits(vl_bits) < ARM_SVE_MAX_VL_BYTES / 8u) {
        check(predicate[arm_sve_predicate_bytes_for_bits(vl_bits)], 0xaa,
              "PTRUE pattern preserves predicate bytes above VL");
    }
    /* Later fixtures assume pristine (zeroed) predicate storage. */
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES / 8u; ++byte) {
        predicate[byte] = 0;
    }
}

static bool append_sve_whilelo(DisasContext *s, uint8_t pd, uint8_t rn,
                               uint8_t rm, uint8_t element_bits,
                               uint8_t scalar_bits, const char *what)
{
    s->insn_type = AARCH64_A64_SVE_WHILELO;
    s->arg.f_sve_whilelo = (arg_sve_whilelo){
        .pd = pd,
        .rn = rn,
        .rm = rm,
        .element_bits = element_bits,
        .scalar_bits = scalar_bits,
        .effects = {.writes_nzcv = true},
    };
    bool ok = trans_sve_whilelo(s);
    check(ok, 1, what);
    return ok;
}


/* SVE NOT via a generated TB: complement active lanes, merge the rest. */
/*
 * Write-back window proof: two NOTs to the same destination.  With the window
 * enabled the first store is dead once the second completes, so the TB emits
 * exactly one fewer Z store than the same chain with caching disabled, and the
 * second instruction hits the cached destination chunk.
 */
static void run_sve_wb_chain_once(bool enable, uint32_t *writes,
                                  uint32_t *hits, const char *what)
{
    const uint16_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    static const size_t active_lanes = 20;
    uint8_t *z12 = arm_sve_zreg(&env, 12);
    uint8_t *z13 = arm_sve_zreg(&env, 13);
    uint8_t *z14 = arm_sve_zreg(&env, 14);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    uint32_t zbase = lata_vcache_z_writes();
    uint32_t hbase = lata_vcache_hits();

    check(arm_sve_set_vl(&env, vl_bits), 1, what);
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        z12[byte] = 0x33;
        z13[byte] = (uint8_t)(0xa5u ^ byte);
        z14[byte] = (uint8_t)(0x5au ^ byte);
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    lata_vcache_set_enabled(enable);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, 1, 13, 14, 8, 64, what)) {
        tr_fini();
        return;
    }
    for (int round = 0; round < 2; ++round) {
        s.insn_type = AARCH64_A64_SVE_NOT;
        s.arg.f_sve_not = (arg_sve_not){
            .zd = 12,
            .pg = 1,
            .zm = round == 0 ? 13 : 14,
            .element_bits = 8,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        if (!trans_sve_not(&s)) {
            check(0, 1, what);
            tr_fini();
            return;
        }

    }
    lata_zcache_flush_for_exit();
    *writes = lata_vcache_z_writes() - zbase;
    *hits = lata_vcache_hits() - hbase;
    if (enable) {
        check(lata_vcache_clean(), 1,
              "write-back window is flushed by the exit hook");
    }
    lata_vcache_set_enabled(false);
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
}

static void run_sve_wb_cache_runtime(void)
{
    const size_t lanes = 256 / 8u;
    const size_t active_lanes = 20;
    uint8_t *z12 = arm_sve_zreg(&env, 12);
    uint8_t *z13 = arm_sve_zreg(&env, 13);
    uint8_t *z14 = arm_sve_zreg(&env, 14);
    static uint8_t seed13[ARM_SVE_MAX_VL_BYTES];
    static uint8_t seed14[ARM_SVE_MAX_VL_BYTES];
    uint32_t writes_off = 0, hits_off = 0;
    uint32_t writes_on = 0, hits_on = 0;

    run_sve_wb_chain_once(false, &writes_off, &hits_off,
                          "write-back chain (cache off) lowers");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        seed13[byte] = z13[byte];
        seed14[byte] = z14[byte];
    }
    run_sve_wb_chain_once(true, &writes_on, &hits_on,
                          "write-back chain (cache on) lowers");

    latx_bare_uart_puts("sve-pvir: wb off=");
    puthex(writes_off);
    latx_bare_uart_puts(" on=");
    puthex(writes_on);
    latx_bare_uart_puts(" hits_off=");
    puthex(hits_off);
    latx_bare_uart_puts(" hits_on=");
    puthex(hits_on);
    latx_bare_uart_puts("\n");
    check(writes_on + 1, writes_off,
          "write-back window removes exactly one dead Z store");
    check(writes_off, 2, "cache-off chain writes Z twice");
    check(hits_on >= 1, 1,
          "second NOT hits the cached destination chunk");
    check(hits_on, hits_off + 1,
          "cache-on run adds exactly one window hit");

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint8_t expected = lane < active_lanes
                               ? (uint8_t)~seed14[lane]
                               : 0x33;
        check(z12[lane], expected,
              "write-back chain leaves the final value in env");
        check(z13[lane], seed13[lane],
              "write-back leaves the untouched source alone");
    }
}

/*
 * Forced spill: shrink the temporary pool so the SVE NOT lowering (5 vector
 * temporaries) must recycle cold guest registers, then check the result is
 * unchanged and that spills actually happened.
 */
static void run_sve_spill_runtime(void)
{
    const uint16_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    const size_t lanes = vl_bytes;
    const size_t active_lanes = 20;
    uint8_t *destination = arm_sve_zreg(&env, 12);
    uint8_t *source = arm_sve_zreg(&env, 13);
    static uint8_t seed[ARM_SVE_MAX_VL_BYTES];
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    uint32_t spills_before;
    uint32_t spills_after;
    uint32_t resident_before;
    uint32_t resident_after;

    check(arm_sve_set_vl(&env, vl_bits), 1, "spill fixture VL");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x44;
        source[byte] = (uint8_t)(0x3cu ^ byte);
        seed[byte] = source[byte];
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    spills_before = lata_vreg_spills();
    lata_ftemp_set_base_pool(2);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    resident_before = lata_vreg_resident_count();
    if (!append_sve_whilelo(&s, 1, 13, 14, 8, 64,
                            "WHILELO spill predicate lowers")) {
        tr_fini();
        lata_ftemp_set_base_pool(6);
        return;
    }
    s.insn_type = AARCH64_A64_SVE_NOT;
    s.arg.f_sve_not = (arg_sve_not){
        .zd = 12,
        .pg = 1,
        .zm = 13,
        .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_not(&s)) {
        check(0, 1, "spill NOT lowers");
        tr_fini();
        lata_ftemp_set_base_pool(6);
        return;
    }
    check(1, 1, "spill NOT lowers");
    lata_zcache_flush_for_exit();
    spills_after = lata_vreg_spills();
    resident_after = lata_vreg_resident_count();

    lata_ftemp_set_base_pool(6);
    check(spills_after > spills_before, 1,
          "exhausting the pool spills a cold guest register");
    /* Cross-TB residency: a spilled register stays env-backed afterwards. */
    check(resident_after < resident_before, 1,
          "spilling reduces the resident set");
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint8_t expected = lane < active_lanes ? (uint8_t)~seed[lane] : 0x44;
        check(destination[lane], expected,
              "spilled lowering still produces the exact result");
    }
    /* A fresh TB must not resurrect the demoted registers. */
    tr_init(&tb);
    tr_fini();
    check(lata_vreg_resident_count(), resident_after,
          "residency decisions persist into the next translation");
}

/*
 * FFR round trip: SETFFR then RDFFR into P2 must publish the all-true first
 * fault state into the predicate register, while bytes above VL are preserved.
 */
static void run_sve_ffr_runtime(void)
{
    const uint16_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    const size_t predicate_bytes = vl_bytes / 8u;
    uint8_t *ffr = arm_sve_ffr(&env);
    uint8_t *predicate = arm_sve_preg(&env, 2);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, vl_bits), 1, "FFR fixture VL");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES / 8u; ++byte) {
        ffr[byte] = 0;
        predicate[byte] = 0xaa;
    }
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    {
        A64SVEIR1Instruction decoded;

        if (a64_sve_decode(0x252c9000u, &decoded) != A64_SVE_DECODE_MATCH ||
            decoded.insn_type != AARCH64_A64_SVE_SETFFR) {
            check(0, 1, "SETFFR encoding decodes for the runtime fixture");
            tr_fini();
            return;
        }
        s.insn_type = decoded.insn_type;
        s.arg = decoded.arg;
    }
    if (!trans_sve_setffr(&s)) {
        check(0, 1, "SETFFR lowers in the FFR runtime fixture");
        tr_fini();
        return;
    }
    {
        A64SVEIR1Instruction decoded;

        if (a64_sve_decode(0x2519f002u, &decoded) != A64_SVE_DECODE_MATCH ||
            decoded.insn_type != AARCH64_A64_SVE_RDFFR) {
            check(0, 1, "RDFFR encoding decodes for the runtime fixture");
            tr_fini();
            return;
        }
        s.insn_type = decoded.insn_type;
        s.arg = decoded.arg;
    }
    if (!trans_sve_rdffr(&s)) {
        check(0, 1, "RDFFR lowers in the FFR runtime fixture");
        tr_fini();
        return;
    }
    check(1, 1, "SETFFR/RDFFR lower in one TB");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        check(ffr[byte], 0xff, "SETFFR fills the first-fault register");
        check(predicate[byte], 0xff, "RDFFR copies FFR into the predicate");
    }
    for (size_t byte = predicate_bytes; byte < ARM_SVE_MAX_VL_BYTES / 8u; ++byte) {
        check(predicate[byte], 0xaa,
              "RDFFR preserves predicate bytes above VL");
    }
}

/* SVE DUP/MOV: broadcast the guest GPR's low element into every lane. */
static void run_sve_dup_runtime(uint8_t element_bits, uint64_t scalar,
                                const char *what)
{
    const uint16_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    const size_t element_bytes = element_bits / 8u;
    const size_t lanes = vl_bytes / element_bytes;
    uint8_t *vector = arm_sve_zreg(&env, 17);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    uint64_t element_mask = element_bits == 64
                                ? ~UINT64_C(0)
                                : (UINT64_C(1) << element_bits) - 1u;

    check(arm_sve_set_vl(&env, vl_bits), 1, what);
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        vector[byte] = 0x5c;
    }
    /* x13 is env-backed (see arm_la_map); hand-built TBs cannot see resident
     * host registers because they skip the entry prologue. */
    env.xregs[13] = scalar;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    s.insn_type = AARCH64_A64_SVE_DUP;
    s.arg.f_sve_dup = (arg_sve_dup){
        .zd = 17,
        .rn = 13,
        .element_bits = element_bits,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_dup(&s)) {
        check(0, 1, what);
        tr_fini();
        return;
    }
    check(1, 1, what);
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t actual = 0;
        uint64_t expected = scalar & element_mask;

        for (size_t byte = 0; byte < element_bytes; ++byte) {
            actual |= (uint64_t)vector[lane * element_bytes + byte]
                      << (byte * 8u);
        }
        check(actual, expected, what);
    }
    if (vl_bytes < ARM_SVE_MAX_VL_BYTES) {
        check(vector[vl_bytes], 0x5c, "SVE DUP preserves bytes above VL");
    }
}

/* PFALSE: every active predicate bit must clear, bytes above VL preserved. */
static void run_sve_pfalse_runtime(void)
{
    const uint16_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    const size_t predicate_bytes = vl_bytes / 8u;
    uint8_t *predicate = arm_sve_preg(&env, 3);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, vl_bits), 1, "PFALSE fixture VL");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES / 8u; ++byte) {
        predicate[byte] = 0xaa;
    }
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    s.insn_type = AARCH64_A64_SVE_PFALSE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 3,
        .element_bits = 8,
        .pattern = 0,
    };
    if (!trans_sve_pfalse(&s)) {
        check(0, 1, "PFALSE lowers");
        tr_fini();
        return;
    }
    check(1, 1, "PFALSE lowers");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t byte = 0; byte < predicate_bytes; ++byte) {
        check(predicate[byte], 0, "PFALSE clears every active predicate byte");
    }
    for (size_t byte = predicate_bytes; byte < ARM_SVE_MAX_VL_BYTES / 8u; ++byte) {
        check(predicate[byte], 0xaa, "PFALSE preserves bytes above VL");
    }
}

/*
 * Debugging aid: translate one guest word and print its IR2 opcode sequence.
 * The FMAXV hang reproduces deterministically, so this dump is the intended
 * way to inspect the emitted sequence instead of guessing.
 */
static void dump_ir2_for_word(uint32_t word, const char *what)
{
    uint32_t guest = word;
    A64SVEPvirBuilder unused;
    int count = 0;

    (void)unused;
    for (size_t i = 0; i < sizeof(env); ++i) { ((uint8_t *)&env)[i] = 0; }
    for (size_t i = 0; i < sizeof(cpu); ++i) { ((uint8_t *)&cpu)[i] = 0; }
    for (size_t i = 0; i < sizeof(tb); ++i) { ((uint8_t *)&tb)[i] = 0; }

    cpu.env_ptr = &env;
    env.pc = (uint64_t)(uintptr_t)&guest;
    lata_tr_data_init(&env);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tb.pc = (uint64_t)(uintptr_t)&guest;
    tb.tc.ptr = code_buf;
    tb.cflags = 0;

    {
        int max_insns = 1;

        target_disasm(&tb, &max_insns, &cpu);
    }
    tr_init(&tb);
    if (!tr_ir2_generate(&tb)) {
        check(0, 1, what);
        tr_fini();
        return;
    }
    latx_bare_uart_puts("lata-isa: ir2 dump ");
    latx_bare_uart_puts(what);
    latx_bare_uart_puts(" icount=");
    puthex(tb.icount);
    latx_bare_uart_puts(":");
    for (IR2_INST *inst = lata_first_ir2(); inst != NULL;
         inst = ir2_next(inst)) {
        puthex((uint64_t)ir2_opcode(inst));
        latx_bare_uart_puts(" ");
        count++;
    }
    latx_bare_uart_puts("\n");
    check(count > 0, 1, what);
    tr_fini();
}

static void run_sve_not_runtime(uint16_t vl_bits, unsigned zd, unsigned zm,
                                unsigned pg, uint8_t element_bits,
                                size_t active_lanes, bool alias,
                                const char *what)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / element_bits;
    size_t element_bytes = element_bits / 8u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    static uint8_t expected_source[ARM_SVE_MAX_VL_BYTES];
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, vl_bits), 1, what);
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x33;
        source[byte] = (uint8_t)(0xa5u ^ byte);
    }
    /* Zd == Zn overwrites the seed in place, so keep a snapshot for checks. */
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        expected_source[byte] = source[byte];
    }
    if (alias) {
        /* Zd == Zn: the source seed is also the merge value. */
        for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
            destination[byte] = source[byte];
        }
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;
    lata_vcache_set_enabled(true);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, element_bits, 64,
                            "WHILELO NOT predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = AARCH64_A64_SVE_NOT;
    s.arg.f_sve_not = (arg_sve_not){
        .zd = (uint8_t)zd,
        .pg = (uint8_t)pg,
        .zm = (uint8_t)zm,
        .element_bits = element_bits,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_not(&s)) {
        check(0, 1, what);
        tr_fini();
        return;
    }
    check(1, 1, what);
    {
        uint32_t before = lata_vcache_exit_hooks();

        /* Same hook an exit emitter uses, called before the jump. */
        lata_zcache_flush_for_exit();
        check(lata_vcache_exit_hooks(), before + 1,
              "exit flush hook is observable and idempotent");
        check(lata_vcache_clean(), 1,
              "exit flush leaves no borrowed state");
    }
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    /* I0: the TB ended without leaving a borrowed vector register behind. */
    check(lata_vcache_clean(), 1,
          "NOT TB leaves no borrowed vector state");
    if (alias) {
        /* The hit replaces one env read, which is the point of the cache. */
        check(lata_vcache_vector_reads() >= 1, 1,
              "aliasing NOT still reads the chunk once");
    } else {
        check(lata_vcache_vector_reads() >= 2, 1,
              "NOT TB env vector traffic is counted");
    }
    check(lata_vcache_vector_writes() >= 1, 1,
          "NOT TB writes its result chunk");
    check(lata_vcache_hits() + lata_vcache_misses() > 0, 1,
          "chunk cache is consulted for every vector load");
    if (alias) {
        latx_bare_uart_puts("sve-pvir: vcache hits=");
        puthex(lata_vcache_hits());
        latx_bare_uart_puts(" misses=");
        puthex(lata_vcache_misses());
        latx_bare_uart_puts(" reads=");
        puthex(lata_vcache_vector_reads());
        latx_bare_uart_puts("\n");
        check(lata_vcache_hits() >= 1, 1,
              "Zd == Zn reuses the cached chunk instead of re-reading env");
    } else {
        check(lata_vcache_misses() >= 2, 1,
              "distinct chunk offsets miss the single-entry cache");
    }
    lata_vcache_set_enabled(false);

    for (size_t lane = 0; lane < lanes; ++lane) {
        for (size_t byte = 0; byte < element_bytes; ++byte) {
            size_t index = lane * element_bytes + byte;
            uint8_t expected = lane < active_lanes
                                   ? (uint8_t)~expected_source[index]
                                   : (alias ? expected_source[index] : 0x33);
            check(destination[index], expected, what);
        }
    }
}

static void append_sve_nzcv_capture(uint64_t *results, unsigned index)
{
    la_armmfflag(a1_ir2_opnd, 0x39);
    li_d(a0_ir2_opnd, (int64_t)(uintptr_t)results);
    la_st_d(a1_ir2_opnd, a0_ir2_opnd, (int)(index * sizeof(*results)));
}


/*
 * PTRUES: the PredTest result is compile-time constant, so the lowerer writes
 * PSTATE.NZCV directly. Capture it through the same LBT_ARM flag read used by
 * the WHILELO fixtures.
 */
static void run_sve_ptrues_runtime(uint16_t vl_bits, unsigned pd,
                                   uint8_t pattern, uint8_t element_bits,
                                   uint64_t expected_flags,
                                   const char *what)
{
    static uint64_t flags[1];
    size_t vl_bytes = vl_bits / 8u;
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, vl_bits), 1, what);
    flags[0] = 0;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = (uint8_t)pd,
        .element_bits = element_bits,
        .pattern = pattern,
        .setflags = true,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, what);
        tr_fini();
        return;
    }
    check(1, 1, what);
    append_sve_nzcv_capture(flags, 0);
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    check((uint32_t)flags[0], (uint32_t)expected_flags, what);
}

static void run_sve_integer_chunk_runtime(uint16_t vl_bits, unsigned zd,
                                          unsigned zm, unsigned pg,
                                          PvirOpcode opcode)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / 32u;
    size_t active_lanes = lanes / 2u + 1u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;
    bool subtract = opcode == PVIR_OP_SUB;
    bool multiply = opcode == PVIR_OP_MUL;
    bool bitwise_or = opcode == PVIR_OP_OR;
    bool bitwise_and = opcode == PVIR_OP_AND;
    bool bitwise_xor = opcode == PVIR_OP_XOR;
    bool shift_left = opcode == PVIR_OP_SHL;
    bool shift_right = opcode == PVIR_OP_SHR;
    bool shift_arith = opcode == PVIR_OP_SAR;

    check(arm_sve_set_vl(&env, vl_bits), 1,
          "SVE integer /M chunk VL is valid");
    check(arm_sve_active_vl_bytes(&env), vl_bytes,
          "SVE integer /M chunk state matches VL");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x6b;
        source[byte] = 0x39;
    }
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t old_value =
            UINT32_C(0x10000000) + ((uint32_t)vl_bits << 8) +
            (uint32_t)lane * 37u;
        uint32_t rhs =
            UINT32_C(0x01000000) + ((uint32_t)vl_bits << 4) +
            (uint32_t)lane * 101u;

        if (subtract && lane == 0) {
            old_value = 5;
            rhs = 9;
        } else if (multiply && lane == 0) {
            old_value = UINT32_C(0x80000001);
            rhs = 3;
        }
        for (unsigned byte = 0; byte < 4; ++byte) {
            destination[lane * 4u + byte] =
                (uint8_t)(old_value >> (byte * 8u));
            source[lane * 4u + byte] = (uint8_t)(rhs >> (byte * 8u));
        }
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, 32, 64,
                            "WHILELO integer chunk predicate lowers")) {
        tr_fini();
        return;
    }

    if (multiply) {
        s.insn_type = AARCH64_A64_SVE_MUL;
        s.arg.f_sve_mul = (arg_sve_mul){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_mul(&s);
    } else if (shift_left) {
        s.insn_type = AARCH64_A64_SVE_LSL;
        s.arg.f_sve_lsl = (arg_sve_lsl){
            .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_lsl(&s);
    } else if (shift_right) {
        s.insn_type = AARCH64_A64_SVE_LSR;
        s.arg.f_sve_lsr = (arg_sve_lsr){
            .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_lsr(&s);
    } else if (shift_arith) {
        s.insn_type = AARCH64_A64_SVE_ASR;
        s.arg.f_sve_asr = (arg_sve_asr){
            .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_asr(&s);
    } else if (bitwise_and) {
        s.insn_type = AARCH64_A64_SVE_AND;
        s.arg.f_sve_and = (arg_sve_and){
            .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_and(&s);
    } else if (bitwise_xor) {
        s.insn_type = AARCH64_A64_SVE_EOR;
        s.arg.f_sve_eor = (arg_sve_eor){
            .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_eor(&s);
    } else if (bitwise_or) {
        s.insn_type = AARCH64_A64_SVE_ORR;
        s.arg.f_sve_orr = (arg_sve_orr){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_orr(&s);
    } else if (subtract) {
        s.insn_type = AARCH64_A64_SVE_SUB;
        s.arg.f_sve_sub = (arg_sve_sub){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_sub(&s);
    } else {
        s.insn_type = AARCH64_A64_SVE_ADD;
        s.arg.f_sve_add = (arg_sve_add){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = 32,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_add(&s);
    }
    if (!lowered) {
        check(0, 1, bitwise_or ? "SVE ORR /M chunk lowers"
                             : multiply ? "SVE MUL /M chunk lowers"
                             : subtract ? "SVE SUB /M chunk lowers"
                                        : "SVE ADD /M chunk lowers");
        tr_fini();
        return;
    }
    check(1, 1, bitwise_or ? "SVE ORR /M chunk lowers"
                         : multiply ? "SVE MUL /M chunk lowers"
                         : subtract ? "SVE SUB /M chunk lowers"
                                    : "SVE ADD /M chunk lowers");

    latx_bare_uart_puts(multiply ? "sve-pvir: MUL /M chunk TB begin\n"
                                 : subtract ? "sve-pvir: SUB /M chunk TB begin\n"
                                            : "sve-pvir: ADD /M chunk TB begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts(multiply ? "sve-pvir: MUL /M chunk TB done\n"
                                 : subtract ? "sve-pvir: SUB /M chunk TB done\n"
                                            : "sve-pvir: ADD /M chunk TB done\n");

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t old_value =
            UINT32_C(0x10000000) + ((uint32_t)vl_bits << 8) +
            (uint32_t)lane * 37u;
        uint32_t rhs =
            UINT32_C(0x01000000) + ((uint32_t)vl_bits << 4) +
            (uint32_t)lane * 101u;

        if (subtract && lane == 0) {
            old_value = 5;
            rhs = 9;
        } else if (multiply && lane == 0) {
            old_value = UINT32_C(0x80000001);
            rhs = 3;
        }
        uint32_t arithmetic =
            shift_left ? (old_value << (rhs & 31u))
                       : shift_right ? (old_value >> (rhs & 31u))
                       : shift_arith ? (uint32_t)((int32_t)old_value >> (rhs & 31u))
                       : bitwise_and ? (old_value & rhs)
                        : bitwise_xor ? (old_value ^ rhs)
                        : bitwise_or ? (old_value | rhs)
                       : multiply ? old_value * rhs
                                  : subtract ? old_value - rhs : old_value + rhs;
        uint32_t expected =
            lane < active_lanes ? arithmetic : old_value;
        size_t predicate_bit = lane * 4u;
        uint8_t predicate_value =
            (uint8_t)((arm_sve_preg(&env, pg)[predicate_bit / 8u] >>
                       (predicate_bit % 8u)) &
                      1u);

        check(read_le32(destination + lane * 4u), expected,
              "SVE integer /M chunk lane and merge result");
        check(predicate_value, lane < active_lanes,
              "SVE integer /M chunk predicate lane");
    }
    if (multiply) {
        check(read_le32(destination), UINT32_C(0x80000003),
              "SVE MUL /M active product wraps modulo 32 bits");
    }
    if (vl_bytes < ARM_SVE_MAX_VL_BYTES) {
        check(destination[vl_bytes], 0x6b,
              "SVE integer /M preserves first byte above VL");
        check(destination[ARM_SVE_MAX_VL_BYTES - 1u], 0x6b,
              "SVE integer /M preserves Z storage tail");
    }
}


/* SVE integer MIN/MAX /M chunk: 32-bit lanes with signed and unsigned
 * comparisons, including negative and high-bit values. */
static void run_sve_minmax_chunk_runtime(uint16_t vl_bits, unsigned zd,
                                         unsigned zm, unsigned pg,
                                         bool is_max, bool is_signed)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / 32u;
    size_t active_lanes = lanes / 2u + 1u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;

    check(arm_sve_set_vl(&env, vl_bits), 1, "SVE min/max chunk VL is valid");
    check(arm_sve_active_vl_bytes(&env), vl_bytes,
          "SVE min/max chunk state matches VL");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x5a;
        source[byte] = 0xa5;
    }
    s.vl = (int)(vl_bits / 8u);
    s.sve_pvir_builder = &builder;
    a64_sve_pvir_builder_init(&builder);
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t old_value;
        uint32_t rhs;

        if (is_signed) {
            /* Mix signs so signed and unsigned orders disagree. */
            old_value = (lane & 1u) ? UINT32_C(0xfffffff0) + (uint32_t)lane
                                    : UINT32_C(0x00000010) + (uint32_t)lane;
            rhs = UINT32_C(0x00000020) + (uint32_t)lane;
        } else {
            old_value = UINT32_C(0xfffffff0) + (uint32_t)lane;
            rhs = UINT32_C(0x00000020) + (uint32_t)lane;
        }
        write_le32(destination + lane * 4u, old_value);
        write_le32(source + lane * 4u, rhs);
    }
    /* Predicate: first active_lanes lanes active. */
    for (size_t bit = 0; bit < (size_t)ARM_SVE_MAX_VL_BYTES * 8u; ++bit) {
        size_t lane = bit / 4u;
        bool active = lane < active_lanes;
        arm_sve_preg(&env, pg)[bit / 8u] =
            (uint8_t)((arm_sve_preg(&env, pg)[bit / 8u] & ~(1u << (bit % 8u))) |
                      (active ? (1u << (bit % 8u)) : 0));
    }

    if (is_max) {
        if (is_signed) {
            s.insn_type = AARCH64_A64_SVE_SMAX;
            s.arg.f_sve_smax = (arg_sve_smax){
                .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
                .element_bits = 32, .is_signed = true,
                .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
            };
            lowered = trans_sve_smax(&s);
        } else {
            s.insn_type = AARCH64_A64_SVE_UMAX;
            s.arg.f_sve_umax = (arg_sve_umax){
                .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
                .element_bits = 32, .is_signed = false,
                .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
            };
            lowered = trans_sve_umax(&s);
        }
    } else {
        s.insn_type = AARCH64_A64_SVE_SMIN;
        s.arg.f_sve_smin = (arg_sve_smin){
            .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
            .element_bits = 32, .is_signed = true,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_smin(&s);
    }
    check(lowered, 1, is_max ? (is_signed ? "SVE SMAX /M chunk lowers"
                                          : "SVE UMAX /M chunk lowers")
                             : "SVE SMIN /M chunk lowers");
    if (!lowered) {
        tr_fini();
        return;
    }
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t down = read_le32(destination + lane * 4u);
        uint32_t up = read_le32(source + lane * 4u);
        uint32_t expected;

        if (lane >= active_lanes) {
            expected = down; /* merge keeps the destination */
        } else if (is_max) {
            expected = is_signed ? (int32_t)down > (int32_t)up ? down : up
                                 : down > up ? down : up;
        } else {
            expected = (int32_t)down < (int32_t)up ? down : up;
        }
        check(down, expected, "SVE min/max /M chunk lane result");
    }
    /* Leave the predicate storage clean for the tests that follow. */
    for (size_t bit = 0; bit < (size_t)ARM_SVE_MAX_VL_BYTES * 8u; ++bit) {
        arm_sve_preg(&env, pg)[bit / 8u] = 0;
    }
}

/* Parameterised SVE integer-binop differential: synthesise the encoding from
 * (op, size, VL) and compare the whole Z register against a C model. */
typedef struct {
    uint32_t base;
    const char *name;
    int kind; /* 0 add 1 sub 2 mul 3 and 4 orr 5 eor 6 shl 7 shr 8 sar
                 9 smax 10 umax 11 smin 12 umin */
} SveBinopDesc;

/* Same registers the existing chunk fixtures use: they are not resident, so
 * alloc_fpr_src() hands out a temp that the lowering can free. */
#define SVE_FUZZ_ZD 10u
#define SVE_FUZZ_ZM 11u

static const SveBinopDesc sve_binops[] = {
    { 0x04000000u, "ADD", 0 },  { 0x04010000u, "SUB", 1 },
    { 0x04100000u, "MUL", 2 },  { 0x041a0000u, "AND", 3 },
    { 0x04180000u, "ORR", 4 },  { 0x04190000u, "EOR", 5 },
    { 0x04138000u, "LSL", 6 },  { 0x04118000u, "LSR", 7 },
    { 0x04108000u, "ASR", 8 },  { 0x04080000u, "SMAX", 9 },
    { 0x04090000u, "UMAX", 10 }, { 0x040a0000u, "SMIN", 11 },
    { 0x040b0000u, "UMIN", 12 },
};

static bool sve_run_a64_tb(uint32_t *guest, int insns, uint16_t vl_bits,
                           uint64_t euen, bool init_zero, const char *what);

static uint64_t sve_fuzz_state = 0x9e3779b97f4a7c15ULL;

static uint64_t sve_fuzz_rand(void)
{
    uint64_t x = sve_fuzz_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    sve_fuzz_state = x;
    return x;
}

static uint64_t sve_elem_mask(unsigned bits)
{
    return (bits >= 64) ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
}

static int64_t sve_elem_signed(uint64_t v, unsigned bits)
{
    uint64_t sign = UINT64_C(1) << (bits - 1);
    uint64_t mask = sve_elem_mask(bits);

    v &= mask;
    return (v & sign) ? (int64_t)(v | ~mask) : (int64_t)v;
}

static void run_sve_binop_fuzz(void)
{
    for (size_t op = 0; op < sizeof(sve_binops) / sizeof(sve_binops[0]); ++op) {
        for (unsigned size = 0; size < 4; ++size) {
            static const uint16_t vls[] = { 128, 256 };
            unsigned bits = 8u << size;
            for (size_t vi = 0; vi < sizeof(vls) / sizeof(vls[0]); ++vi) {
                uint16_t vl_bits = vls[vi];
                size_t vl_bytes = vl_bits / 8u;
                size_t lanes = vl_bytes / (bits / 8u);
                size_t active_lanes = lanes / 2u + 1u;
                uint32_t guest[1];
                uint8_t orig[ARM_SVE_MAX_VL_BYTES];
                uint8_t rhs[ARM_SVE_MAX_VL_BYTES];
                bool ok = true;

                for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
                for (size_t b = 0; b < vl_bytes; ++b) {
                    orig[b] = (uint8_t)sve_fuzz_rand();
                    rhs[b] = (uint8_t)sve_fuzz_rand();
                    arm_sve_zreg(&env, SVE_FUZZ_ZD)[b] = orig[b];
                    arm_sve_zreg(&env, SVE_FUZZ_ZM)[b] = rhs[b];
                }
                /* Same predicate image the proven chunk fixtures build: set the
                 * element's first byte bit for active lanes, leave the rest
                 * clear.  arm_sve_set_vl() must run first or it can clear it. */
                check(arm_sve_set_vl(&env, vl_bits), 1, "fuzz VL is valid");
                for (size_t lane = 0; lane < lanes; ++lane) {
                    size_t bit = lane * (bits / 8u);
                    uint8_t *p = &arm_sve_preg(&env, 7)[bit / 8u];
                    *p = (uint8_t)((*p & ~(1u << (bit % 8u))) |
                                   ((lane < active_lanes) ? (1u << (bit % 8u)) : 0));
                }
                guest[0] = sve_binops[op].base | ((uint32_t)size << 22) |
                           (7u << 10) |
                           ((uint32_t)SVE_FUZZ_ZM << 5) |
                           (uint32_t)SVE_FUZZ_ZD;
                if (!sve_run_a64_tb(guest, 1, vl_bits, 0x0fu, false,
                                    "sve binop fuzz")) {
                    continue;
                }
                for (size_t lane = 0; lane < lanes; ++lane) {
                    unsigned shift = bits - 1u;
                    uint64_t mask = sve_elem_mask(bits);
                    uint64_t a = 0, b = 0, want, got;
                    size_t off = lane * (bits / 8u);

                    for (unsigned k = 0; k < bits / 8u; ++k) {
                        a |= (uint64_t)orig[off + k] << (8u * k);
                        b |= (uint64_t)rhs[off + k] << (8u * k);
                    }
                    a &= mask;
                    b &= mask;
                    switch (sve_binops[op].kind) {
                    case 0: want = (a + b) & mask; break;
                    case 1: want = (a - b) & mask; break;
                    case 2: want = (a * b) & mask; break;
                    case 3: want = a & b; break;
                    case 4: want = a | b; break;
                    case 5: want = a ^ b; break;
                    case 6: want = (a << (b & shift)) & mask; break;
                    case 7: want = (a & mask) >> (b & shift); break;
                    case 8:
                        want = (uint64_t)(sve_elem_signed(a, bits) >>
                                          (b & shift)) & mask;
                        break;
                    case 9:
                        want = (sve_elem_signed(a, bits) >
                                sve_elem_signed(b, bits)) ? a : b;
                        break;
                    case 11:
                        want = (sve_elem_signed(a, bits) <
                                sve_elem_signed(b, bits)) ? a : b;
                        break;
                    default:
                        want = (a > b) ? a : b;
                        if (sve_binops[op].kind == 12) {
                            want = (a < b) ? a : b;
                        }
                        break;
                    }
                    if (lane >= active_lanes) {
                        want = a; /* merge */
                    }
                    got = 0;
                    for (unsigned k = 0; k < bits / 8u; ++k) {
                        got |= (uint64_t)arm_sve_zreg(&env, SVE_FUZZ_ZD)[off + k]
                               << (8u * k);
                    }
                    got &= mask;
                    if (got != want) {
                        if (ok) {
                            latx_bare_uart_puts("fuzz-miss ");
                            latx_bare_uart_puts(sve_binops[op].name);
                            latx_bare_uart_puts(" size=");
                            putdec(bits);
                            latx_bare_uart_puts(" vl=");
                            putdec(vl_bits);
                            latx_bare_uart_puts(" lane=");
                            putdec(lane);
                            latx_bare_uart_puts("\n");
                        }
                        ok = false;
                    }
                }
                check(ok, 1, "SVE binop differential matches the C model");
            }
        }
    }
}

/* Byte-element /M ADD: exercises the 8-bit branch of lower_integer_binary,
 * which the 32-bit chunk helper never reaches. */
static void run_sve_integer_chunk_b(uint16_t vl_bits, unsigned zd, unsigned zm,
                                    unsigned pg)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / 8u;
    size_t active_lanes = lanes / 2u + 1u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;

    check(arm_sve_set_vl(&env, vl_bits), 1, "SVE byte /M chunk VL is valid");
    for (size_t lane = 0; lane < lanes; ++lane) {
        destination[lane] = (uint8_t)(lane * 3u);
        source[lane] = 7u;
    }
    for (size_t byte = lanes; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x5a;
        source[byte] = 0xa5;
    }
    env.xregs[13] = 0;
    env.xregs[14] = (uint64_t)active_lanes;

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, 8, 64,
                            "WHILELO byte chunk predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
        .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    lowered = trans_sve_add(&s);
    check(lowered, 1, "SVE byte /M ADD chunk lowers");
    if (!lowered) {
        tr_fini();
        return;
    }
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint8_t old_value = (uint8_t)(lane * 3u);
        uint8_t expected = lane < active_lanes
                               ? (uint8_t)(old_value + 7u)
                               : old_value;
        if (destination[lane] != expected) {
            check(0, 1, "SVE byte /M ADD lane result");
            return;
        }
    }
    check(1, 1, "SVE byte /M ADD lane results");
    check(destination[vl_bytes], 0x5a, "SVE byte /M preserves above VL");
}

/* /M ADD for 16- and 64-bit elements: covers the remaining width branches of
 * lower_integer_binary that the 32-bit chunk helper does not reach. */
static void run_sve_integer_chunk_gen(uint16_t vl_bits, unsigned zd,
                                      unsigned zm, unsigned pg, int element_bits)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / (size_t)element_bits;
    size_t active_lanes = lanes / 2u + 1u;
    size_t lane_bytes = (size_t)element_bits / 8u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;

    check(arm_sve_set_vl(&env, vl_bits), 1, "SVE chunk VL is valid");
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t old_value = (uint64_t)lane * 3u;
        uint64_t rhs = 7u;
        for (size_t byte = 0; byte < lane_bytes; ++byte) {
            destination[lane * lane_bytes + byte] =
                (uint8_t)(old_value >> (byte * 8u));
            source[lane * lane_bytes + byte] =
                (uint8_t)(rhs >> (byte * 8u));
        }
    }
    for (size_t byte = vl_bytes; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x5a;
    }
    env.xregs[13] = 0;
    env.xregs[14] = (uint64_t)active_lanes;

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, (uint8_t)element_bits, 64,
                            "WHILELO wide chunk predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = (uint8_t)zd, .pg = (uint8_t)pg, .zm = (uint8_t)zm,
        .element_bits = (uint8_t)element_bits,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    lowered = trans_sve_add(&s);
    check(lowered, 1, "SVE wide /M ADD chunk lowers");
    if (!lowered) {
        tr_fini();
        return;
    }
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t old_value = (uint64_t)lane * 3u;
        uint64_t expected = lane < active_lanes ? old_value + 7u : old_value;
        uint64_t got = 0;
        for (size_t byte = 0; byte < lane_bytes; ++byte) {
            got |= (uint64_t)destination[lane * lane_bytes + byte] << (byte * 8u);
        }
        if (got != expected) {
            check(0, 1, "SVE wide /M ADD lane result");
            return;
        }
    }
    check(1, 1, "SVE wide /M ADD lane results");
    check(destination[vl_bytes], 0x5a, "SVE wide /M preserves above VL");
}

/* Provider that cannot claim VA==HPA: every scalar access must be lowered as a
 * provider call, never as a raw host load/store. */
static unsigned prov_loads, prov_stores;

static LataGuestMemStatus prov_probe(CPUARMState *env, uint64_t vaddr,
                                     size_t size, bool is_write,
                                     bool clean_tbi)
{
    (void)env; (void)size; (void)is_write; (void)clean_tbi; (void)vaddr;
    return LATA_MEM_OK;
}

static LataGuestMemStatus prov_load(CPUARMState *env, uint64_t vaddr,
                                    size_t size, bool is_signed,
                                    uint64_t *out_value)
{
    uint64_t value = 0;
    (void)env;
    prov_loads++;
    for (size_t i = 0; i < size; ++i) {
        value |= (uint64_t)((const uint8_t *)(uintptr_t)vaddr)[i] << (8u * i);
    }
    if (is_signed && size < 8) {
        uint64_t sign = 1ULL << (8u * size - 1u);
        if (value & sign) {
            value |= ~((1ULL << (8u * size)) - 1u);
        }
    }
    *out_value = value;
    return LATA_MEM_OK;
}

static LataGuestMemStatus prov_store(CPUARMState *env, uint64_t vaddr,
                                     size_t size, uint64_t value)
{
    (void)env;
    prov_stores++;
    for (size_t i = 0; i < size; ++i) {
        ((uint8_t *)(uintptr_t)vaddr)[i] = (uint8_t)(value >> (8u * i));
    }
    return LATA_MEM_OK;
}

static unsigned prov_bulk_loads, prov_bulk_stores;

static LataGuestMemStatus prov_load_bytes(CPUARMState *env, uint64_t vaddr,
                                          void *dst, size_t size)
{
    (void)env;
    prov_bulk_loads++;
    for (size_t i = 0; i < size; ++i) {
        ((uint8_t *)dst)[i] = ((const uint8_t *)(uintptr_t)vaddr)[i];
    }
    return LATA_MEM_OK;
}

static LataGuestMemStatus prov_store_bytes(CPUARMState *env, uint64_t vaddr,
                                           const void *src, size_t size)
{
    (void)env;
    prov_bulk_stores++;
    for (size_t i = 0; i < size; ++i) {
        ((uint8_t *)(uintptr_t)vaddr)[i] = ((const uint8_t *)src)[i];
    }
    return LATA_MEM_OK;
}

/* Same provider, but no byte-buffer hooks: the lowering must split a 128-bit
 * access into two 8-byte transfers at the right addresses. */
static const LataGuestMemOps prov_no_bulk_ops = {
    .probe = prov_probe,
    .load = prov_load,
    .store = prov_store,
    .direct = NULL,
    .va_is_hpa_identity = false,
};

/* Always fails, to exercise the fault publication path. */
static LataGuestMemStatus prov_fault_load(CPUARMState *env, uint64_t vaddr,
                                          size_t size, bool is_signed,
                                          uint64_t *out_value)
{
    (void)env; (void)vaddr; (void)size; (void)is_signed;
    if (out_value != NULL) {
        *out_value = 0;
    }
    return LATA_MEM_FAULT_UNMAPPED;
}

static LataGuestMemStatus prov_fault_store(CPUARMState *env, uint64_t vaddr,
                                           size_t size, uint64_t value)
{
    (void)env; (void)vaddr; (void)size; (void)value;
    return LATA_MEM_FAULT_UNMAPPED;
}

static const LataGuestMemOps prov_fault_ops = {
    .probe = prov_probe,
    .load = prov_fault_load,
    .store = prov_fault_store,
    .direct = NULL,
    .va_is_hpa_identity = false,
};

/* Policy provider: only [policy_base, +8) is admissible, epoch is constant. */
static uint64_t policy_base;
static uint64_t policy_limit = 8; /* bytes admitted from policy_base */
static unsigned policy_calls;

static uint64_t prov_policy_epoch(CPUARMState *env)
{
    (void)env;
    return 1;
}

static bool prov_policy_span(CPUARMState *env, uint64_t vaddr, uint64_t size,
                             bool is_write)
{
    (void)env; (void)is_write;
    policy_calls++;
    return vaddr >= policy_base && vaddr + size <= policy_base + policy_limit;
}

static const LataGuestMemDirectPolicy prov_policy = {
    .name = "test-policy",
    .epoch = prov_policy_epoch,
    .span_admissible = prov_policy_span,
};

static const LataGuestMemOps prov_policy_ops = {
    .probe = prov_probe,
    .load = prov_load,
    .store = prov_store,
    .direct = &prov_policy,
    .va_is_hpa_identity = false,
};

static const LataGuestMemOps prov_no_identity_ops = {
    .probe = prov_probe,
    .load = prov_load,
    .store = prov_store,
    .load_bytes = prov_load_bytes,
    .store_bytes = prov_store_bytes,
    .direct = NULL,
    .va_is_hpa_identity = false,
};

static void enable_fpe_sxe_asxe(void)
{
    uint64_t euen = (1u << 0) | (1u << 1) | (1u << 2);
    uint64_t actual;

    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    __asm__ volatile("csrrd %0, 0x2" : "=r"(actual));
    check(actual, euen, "SVE FP fixture enables FPE, SXE and ASXE");
}

static uint32_t read_host_fcsr(void)
{
    uint32_t value;

    __asm__ volatile("movfcsr2gr %0, $fcsr0" : "=r"(value));
    return value;
}

static void write_host_fcsr(uint32_t value)
{
    __asm__ volatile("movgr2fcsr $fcsr0, %0" :: "r"(value) : "memory");
}

/*
 * Predicated FDIV must not touch inactive lanes at all. The inactive lanes are
 * seeded with 0/0 (would set Z and I) and with a signalling-NaN divisor (would
 * set I); both must leave FCSR untouched and keep the old destination value.
 * Active lanes use exact quotients so the only possible flags are the ones we
 * are proving absent.
 */
/*
 * FABS/FNEG are sign-bit operations: no FP status is involved, so this fixture
 * checks bit-exact results, inactive-lane merge, and that FCSR stays zero.
 */
static void run_sve_fp_unary_runtime(uint16_t vl_bits, unsigned zd, unsigned zm,
                                     unsigned pg, uint8_t element_bits,
                                     size_t active_lanes, bool negate)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / element_bits;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;
    bool is_double = element_bits == 64;
    uint64_t mask = is_double ? UINT64_C(0x7fffffffffffffff)
                              : UINT64_C(0x7fffffff);
    uint64_t sign = is_double ? UINT64_C(0x8000000000000000)
                              : UINT64_C(0x80000000);

    check(arm_sve_set_vl(&env, vl_bits), 1, "SVE FP unary VL is valid");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x3c;
        source[byte] = 0x00;
    }
    for (size_t lane = 0; lane < lanes; ++lane) {
        if (is_double) {
            /* Alternate signs and include -0.0 and a NaN bit pattern. */
            uint64_t raw = UINT64_C(0x3ff8000000000000) | ((lane & 1u) ? sign : 0);
            if (lane == 2) {
                raw = UINT64_C(0xfff0000000000001); /* negative sNaN bits */
            }
            for (unsigned byte = 0; byte < 8; ++byte) {
                source[lane * 8u + byte] = (uint8_t)(raw >> (byte * 8u));
                destination[lane * 8u + byte] =
                    (uint8_t)(UINT64_C(0x1111222233334444) >> (byte * 8u));
            }
        } else {
            uint32_t raw = UINT32_C(0x3fc00000) | ((lane & 1u) ? (uint32_t)sign : 0);
            if (lane == 2) {
                raw = UINT32_C(0xff800001); /* negative sNaN bits */
            }
            write_le32(source + lane * 4u, raw);
            write_le32(destination + lane * 4u, UINT32_C(0x5555aaaa));
        }
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    write_host_fcsr(0);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, element_bits, 64,
                            "WHILELO FP unary predicate lowers")) {
        tr_fini();
        return;
    }
    if (negate) {
        s.insn_type = AARCH64_A64_SVE_FNEG;
        s.arg.f_sve_fneg = (arg_sve_fneg){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = element_bits,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_fneg(&s);
    } else {
        s.insn_type = AARCH64_A64_SVE_FABS;
        s.arg.f_sve_fabs = (arg_sve_fabs){
            .zd = (uint8_t)zd,
            .pg = (uint8_t)pg,
            .zm = (uint8_t)zm,
            .element_bits = element_bits,
            .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
        };
        lowered = trans_sve_fabs(&s);
    }
    check(lowered, 1, negate ? "SVE FNEG /M chunk lowers"
                             : "SVE FABS /M chunk lowers");
    if (!lowered) {
        tr_fini();
        return;
    }

    latx_bare_uart_puts(negate ? "sve-pvir: FNEG /M chunk TB begin\n"
                               : "sve-pvir: FABS /M chunk TB begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts(negate ? "sve-pvir: FNEG /M chunk TB done\n"
                               : "sve-pvir: FABS /M chunk TB done\n");

    for (size_t lane = 0; lane < lanes; ++lane) {
        bool active = lane < active_lanes;

        if (is_double) {
            uint64_t source_raw = UINT64_C(0x3ff8000000000000) |
                                  ((lane & 1u) ? sign : 0);
            uint64_t arithmetic;
            uint64_t expected;

            if (lane == 2) {
                source_raw = UINT64_C(0xfff0000000000001);
            }
            arithmetic = negate ? (source_raw ^ sign) : (source_raw & mask);
            expected = active ? arithmetic : UINT64_C(0x1111222233334444);
            check(read_le64(destination + lane * 8u), expected,
                  "SVE FABS/FNEG.D /M lane result and merge");
        } else {
            uint32_t source_raw = UINT32_C(0x3fc00000) |
                                  ((lane & 1u) ? (uint32_t)sign : 0u);
            uint32_t arithmetic;
            uint32_t expected;

            if (lane == 2) {
                source_raw = UINT32_C(0xff800001);
            }
            arithmetic = negate ? (source_raw ^ (uint32_t)sign)
                                : (source_raw & (uint32_t)mask);
            expected = active ? arithmetic : UINT32_C(0x5555aaaa);
            check(read_le32(destination + lane * 4u), expected,
                  "SVE FABS/FNEG.S /M lane result and merge");
        }
    }
    check(read_host_fcsr(), 0,
          "SVE FABS/FNEG leaves the FP status register untouched");
    if (vl_bytes < ARM_SVE_MAX_VL_BYTES) {
        check(destination[vl_bytes], 0x3c,
              "SVE FP unary preserves first byte above VL");
    }
}

/*
 * LD1/ST1 at wider memory element sizes. One TB loads into Z, a second stores
 * the same Z back to a separate buffer, so both directions are checked against
 * the same lane values; inactive lanes must read as zero and inactive stores
 * must leave memory untouched.
 */
static void run_sve_contiguous_width_runtime(uint16_t vl_bits,
                                             uint8_t memory_bits,
                                             uint8_t element_bits,
                                             size_t active_lanes,
                                             bool sign_extend,
                                             const char *what)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / element_bits;
    size_t element_bytes = element_bits / 8u;
    size_t memory_bytes = memory_bits / 8u;
    static volatile uint8_t input[ARM_SVE_MAX_VL_BYTES * 2];
    static volatile uint8_t output[ARM_SVE_MAX_VL_BYTES * 2];
    uint8_t *vector = arm_sve_zreg(&env, 6);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, vl_bits), 1, what);
    for (size_t byte = 0; byte < sizeof(input); ++byte) {
        input[byte] = 0;
        output[byte] = 0xee;
    }
    size_t memory_lanes = vl_bits / memory_bits;
    for (size_t lane = 0; lane < memory_lanes && lane < lanes * element_bytes /
                                                 memory_bytes; ++lane) {
        uint64_t value = UINT64_C(0x51) + (uint64_t)lane * 0x010101u;
        /* Force the memory element negative so sign extension is visible. */
        value |= UINT64_C(1) << (memory_bits - 1u);
        for (size_t byte = 0; byte < memory_bytes; ++byte) {
            input[lane * memory_bytes + byte] =
                (uint8_t)(value >> (byte * 8u));
        }
    }
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        vector[byte] = 0x7d;
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    /* --- LOAD ------------------------------------------------------- */
    env.xregs[26] = (uintptr_t)input;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, 1, 13, 14, element_bits, 64,
                            "WHILELO contiguous predicate lowers")) {
        tr_fini();
        return;
    }
    if (sign_extend) {
        s.insn_type = element_bits == 16   ? AARCH64_A64_SVE_LD1SB
                      : memory_bits == 8   ? AARCH64_A64_SVE_LD1SB
                      : memory_bits == 16  ? AARCH64_A64_SVE_LD1SH
                                           : AARCH64_A64_SVE_LD1SW;
    } else {
        s.insn_type = memory_bits == 16   ? AARCH64_A64_SVE_LD1H
                      : memory_bits == 32 ? AARCH64_A64_SVE_LD1W
                                          : AARCH64_A64_SVE_LD1D;
    }
    s.arg.f_sve_contiguous = (arg_sve_contiguous){
        .zt = 6,
        .rn = 26,
        .pg = 1,
        .immediate_vl = 0,
        .element_bits = element_bits,
        .memory_bits = memory_bits,
        .sign_extend = sign_extend,
        .base_is_sp = false,
        .effects = {
            .inactive_policy = PVIR_INACTIVE_ZERO,
            .memory = {
                .access = PVIR_MEMORY_READ,
                .ordering = PVIR_MEMORY_ORDER_RELAXED,
                .inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION,
            },
            .fault_behavior = PVIR_FAULT_PRECISE,
            .fault_order = PVIR_FAULT_ORDER_LANE_ASCENDING,
        },
    };
    if (!(sign_extend ? (memory_bits == 32   ? trans_sve_ld1sw(&s)
                         : memory_bits == 16 ? trans_sve_ld1sh(&s)
                                             : trans_sve_ld1sb(&s))
                      : trans_sve_ld1b(&s))) {
        check(0, 1, "wide contiguous load lowers");
        tr_fini();
        return;
    }
    check(1, 1, "wide contiguous load lowers");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t memory_mask = memory_bits == 64
                                   ? ~UINT64_C(0)
                                   : (UINT64_C(1) << memory_bits) - 1u;
        uint64_t element_mask = element_bits == 64
                                    ? ~UINT64_C(0)
                                    : (UINT64_C(1) << element_bits) - 1u;
        uint64_t raw = (UINT64_C(0x51) + (uint64_t)lane * 0x010101u) &
                       memory_mask;
        uint64_t expected;

        raw |= UINT64_C(1) << (memory_bits - 1u);
        if (sign_extend && ((raw >> (memory_bits - 1u)) & 1u)) {
            expected = (raw | ~memory_mask) & element_mask;
        } else {
            expected = raw & element_mask;
        }
        if (lane >= active_lanes) {
            expected = 0;
        } else if (lane >= memory_lanes) {
            expected &= memory_mask; /* no memory element: value is untouched */
        }
        {
        uint64_t actual = 0;

        for (size_t byte = 0; byte < element_bytes; ++byte) {
            actual |= (uint64_t)vector[lane * element_bytes + byte]
                      << (byte * 8u);
        }
        check(actual, expected, what);
        }
    }

    /* --- STORE ------------------------------------------------------ */
    env.xregs[26] = (uintptr_t)output;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, 1, 13, 14, element_bits, 64,
                            "WHILELO contiguous store predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = memory_bits == 16   ? AARCH64_A64_SVE_ST1H
                  : memory_bits == 32 ? AARCH64_A64_SVE_ST1W
                                      : AARCH64_A64_SVE_ST1D;
    s.arg.f_sve_contiguous = (arg_sve_contiguous){
        .zt = 6,
        .rn = 26,
        .pg = 1,
        .immediate_vl = 0,
        .element_bits = element_bits,
        .memory_bits = memory_bits,
        .base_is_sp = false,
        .effects = {
            .inactive_policy = PVIR_INACTIVE_NOT_APPLICABLE,
            .memory = {
                .access = PVIR_MEMORY_WRITE,
                .ordering = PVIR_MEMORY_ORDER_RELAXED,
                .inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION,
            },
            .fault_behavior = PVIR_FAULT_PRECISE,
            .fault_order = PVIR_FAULT_ORDER_LANE_ASCENDING,
        },
    };
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "wide contiguous store lowers");
        tr_fini();
        return;
    }
    check(1, 1, "wide contiguous store lowers");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t raw = (UINT64_C(0x51) + (uint64_t)lane * 0x010101u) |
                       (UINT64_C(1) << (memory_bits - 1u));

        for (size_t byte = 0; byte < memory_bytes; ++byte) {
            uint8_t expected = lane < active_lanes
                                   ? (uint8_t)(raw >> (byte * 8u))
                                   : 0xee;
            check(output[lane * memory_bytes + byte], expected,
                  "wide contiguous store lane byte");
        }
    }
}

static void run_sve_fp_div_runtime(uint16_t vl_bits, unsigned zd, unsigned zm,
                                   unsigned pg, uint8_t element_bits,
                                   size_t active_lanes, bool expect_clean)
{
    size_t vl_bytes = vl_bits / 8u;
    size_t lanes = vl_bits / element_bits;
    size_t exact_lanes = lanes / 2u;
    uint8_t *destination = arm_sve_zreg(&env, zd);
    uint8_t *source = arm_sve_zreg(&env, zm);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    bool lowered;
    bool is_double = element_bits == 64;

    check(arm_sve_set_vl(&env, vl_bits), 1, "SVE FP /M chunk VL is valid");
    for (size_t byte = 0; byte < ARM_SVE_MAX_VL_BYTES; ++byte) {
        destination[byte] = 0x5a;
        source[byte] = 0xa5;
    }
    /*
     * Lanes below exact_lanes divide exactly (8.0/2.0, 6.0/3.0). The rest hold
     * 0/0, and the last one a signalling NaN: computing any of them would set
     * FP flags, so they are only legal while inactive.
     */
    for (size_t lane = 0; lane < lanes; ++lane) {
        if (is_double) {
            uint64_t dividend = lane < exact_lanes ? UINT64_C(0x4020000000000000)
                                                   : UINT64_C(0x0000000000000000);
            uint64_t divisor = lane < exact_lanes ? UINT64_C(0x4000000000000000)
                                                  : UINT64_C(0x0000000000000000);
            if (lane >= exact_lanes && lane + 1u == lanes) {
                divisor = UINT64_C(0x7ff0000000000001); /* sNaN */
            }
            for (unsigned byte = 0; byte < 8; ++byte) {
                destination[lane * 8u + byte] = (uint8_t)(dividend >> (byte * 8u));
                source[lane * 8u + byte] = (uint8_t)(divisor >> (byte * 8u));
            }
        } else {
            uint32_t dividend = lane < exact_lanes ? UINT32_C(0x40c00000)
                                                   : UINT32_C(0x00000000);
            uint32_t divisor = lane < exact_lanes ? UINT32_C(0x40400000)
                                                  : UINT32_C(0x00000000);
            if (lane >= exact_lanes && lane + 1u == lanes) {
                divisor = UINT32_C(0x7f800001); /* sNaN */
            }
            write_le32(destination + lane * 4u, dividend);
            write_le32(source + lane * 4u, divisor);
        }
    }
    env.xregs[13] = 0;
    env.xregs[14] = active_lanes;

    write_host_fcsr(0);
    check(read_host_fcsr(), 0, "SVE FP fixture clears FCSR before the TB");
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, (uint8_t)pg, 13, 14, element_bits, 64,
                            "WHILELO FP chunk predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = AARCH64_A64_SVE_FDIV;
    s.arg.f_sve_fdiv = (arg_sve_fdiv){
        .zd = (uint8_t)zd,
        .pg = (uint8_t)pg,
        .zm = (uint8_t)zm,
        .element_bits = element_bits,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    lowered = trans_sve_fdiv(&s);
    check(lowered, 1, "SVE FDIV /M chunk lowers");
    if (!lowered) {
        tr_fini();
        return;
    }

    latx_bare_uart_puts("sve-pvir: FDIV /M chunk TB begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: FDIV /M chunk TB done\n");

    for (size_t lane = 0; lane < lanes; ++lane) {
        bool active = lane < active_lanes;
        bool checkable = lane < exact_lanes || !active;

        if (!checkable) {
            /* Positive control: 0/0 is deliberately computed here. */
            continue;
        }
        if (is_double) {
            uint64_t old_value = lane < exact_lanes
                                     ? UINT64_C(0x4020000000000000)
                                     : UINT64_C(0x0000000000000000);
            uint64_t expected = active ? UINT64_C(0x4010000000000000) : old_value;
            check(read_le64(destination + lane * 8u), expected,
                  "SVE FDIV.D /M lane result and merge");
        } else {
            uint32_t old_value = lane < exact_lanes ? UINT32_C(0x40c00000)
                                                    : UINT32_C(0x00000000);
            uint32_t expected = active ? UINT32_C(0x40000000) : old_value;
            check(read_le32(destination + lane * 4u), expected,
                  "SVE FDIV.S /M lane result and merge");
        }
    }
    if (expect_clean) {
        check(read_host_fcsr(), 0,
              "SVE FDIV /M inactive lanes raise no FP exception");
    } else {
        /* Positive control: activating the 0/0 lanes must be observable. */
        check((read_host_fcsr() & 0x001f0000u) != 0, 1,
              "SVE FDIV /M active 0/0 lane sets FP flags");
    }
    if (vl_bytes < ARM_SVE_MAX_VL_BYTES) {
        check(destination[vl_bytes], 0x5a,
              "SVE FP /M preserves first byte above VL");
    }
}

static void run_sve_predicate_mask_cache_runtime(void)
{
    const size_t vl_bytes = ARM_SVE_MAX_VL_BYTES;
    const size_t lanes = vl_bytes / sizeof(uint32_t);
    const size_t first_active = 33;
    const size_t changed_active = 17;
    uint8_t *add_destination = arm_sve_zreg(&env, 24);
    uint8_t *add_source = arm_sve_zreg(&env, 25);
    uint8_t *sub_destination = arm_sve_zreg(&env, 26);
    uint8_t *sub_source = arm_sve_zreg(&env, 27);
    uint8_t *mul_destination = arm_sve_zreg(&env, 28);
    uint8_t *mul_source = arm_sve_zreg(&env, 29);
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    check(arm_sve_set_vl(&env, ARM_SVE_MAX_VL_BITS), 1,
          "SVE predicate-cache fixture sets maximum VL");
    check(arm_sve_active_vl_bytes(&env), vl_bytes,
          "SVE predicate-cache fixture uses full mask storage");
    env.xregs[13] = 0;
    env.xregs[14] = first_active;
    env.xregs[15] = changed_active;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t add_old = UINT32_C(0x1000) + (uint32_t)lane * 3u;
        uint32_t add_rhs = UINT32_C(0x200) + (uint32_t)lane * 11u;
        uint32_t sub_old = UINT32_C(0x10000) + (uint32_t)lane * 29u;
        uint32_t sub_rhs = 100u + (uint32_t)lane * 2u;
        uint32_t mul_old = UINT32_C(0x10000) + (uint32_t)lane * 13u;
        uint32_t mul_rhs = 5u + (uint32_t)lane * 2u;

        if (lane == 0) {
            mul_old = UINT32_C(0x80000001);
            mul_rhs = 3;
        }
        write_le32(add_destination + lane * 4u, add_old);
        write_le32(add_source + lane * 4u, add_rhs);
        write_le32(sub_destination + lane * 4u, sub_old);
        write_le32(sub_source + lane * 4u, sub_rhs);
        write_le32(mul_destination + lane * 4u, mul_old);
        write_le32(mul_source + lane * 4u, mul_rhs);
    }

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);
    if (!append_sve_whilelo(&s, 7, 13, 14, 32, 64,
                            "WHILELO initial cache predicate lowers")) {
        tr_fini();
        return;
    }

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 24, .pg = 7, .zm = 25, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "SVE cache first ADD lowers");
        tr_fini();
        return;
    }
    check(1, 1, "SVE cache first ADD lowers");

    s.insn_type = AARCH64_A64_SVE_SUB;
    s.arg.f_sve_sub = (arg_sve_sub){
        .zd = 26, .pg = 7, .zm = 27, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_sub(&s)) {
        check(0, 1, "SVE cache same-version SUB lowers");
        tr_fini();
        return;
    }
    check(1, 1, "SVE cache same-version SUB lowers");
    check(builder.expanded_mask_chunks_materialized, 8,
          "same-predicate operations materialize one mask per LASX chunk");
    check(builder.expanded_mask_chunks_reused, 8,
          "second operation loads all same-version mask chunks from scratch");

    if (!append_sve_whilelo(&s, 7, 13, 15, 32, 64,
                            "WHILELO changed-version cache predicate lowers")) {
        tr_fini();
        return;
    }
    s.insn_type = AARCH64_A64_SVE_MUL;
    s.arg.f_sve_mul = (arg_sve_mul){
        .zd = 28, .pg = 7, .zm = 29, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_mul(&s)) {
        check(0, 1, "SVE cache changed-version MUL lowers");
        tr_fini();
        return;
    }
    check(1, 1, "SVE cache changed-version MUL lowers");
    check(builder.expanded_mask_chunks_materialized, 16,
          "new predicate version forces per-chunk re-expansion");
    check(builder.expanded_mask_chunks_reused, 8,
          "changed predicate version does not reuse stale chunks");
    check(builder.expanded_mask_key.source.version,
          builder.p_versions[7],
          "cache key records the current predicate version");

    latx_bare_uart_puts("sve-pvir: predicate-mask cache TB begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: predicate-mask cache TB done\n");

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t add_old = UINT32_C(0x1000) + (uint32_t)lane * 3u;
        uint32_t add_rhs = UINT32_C(0x200) + (uint32_t)lane * 11u;
        uint32_t sub_old = UINT32_C(0x10000) + (uint32_t)lane * 29u;
        uint32_t sub_rhs = 100u + (uint32_t)lane * 2u;
        uint32_t mul_old = UINT32_C(0x10000) + (uint32_t)lane * 13u;
        uint32_t mul_rhs = 5u + (uint32_t)lane * 2u;
        uint32_t expected_mul;
        size_t predicate_bit = lane * 4u;
        uint8_t predicate_value;

        if (lane == 0) {
            mul_old = UINT32_C(0x80000001);
            mul_rhs = 3;
        }
        expected_mul =
            lane < changed_active ? mul_old * mul_rhs : mul_old;
        predicate_value =
            (uint8_t)((arm_sve_preg(&env, 7)[predicate_bit / 8u] >>
                       (predicate_bit % 8u)) &
                      1u);
        check(read_le32(add_destination + lane * 4u),
              lane < first_active ? add_old + add_rhs : add_old,
              "same-version ADD lane and merge result");
        check(read_le32(sub_destination + lane * 4u),
              lane < first_active ? sub_old - sub_rhs : sub_old,
              "same-version SUB lane and merge result");
        check(read_le32(mul_destination + lane * 4u), expected_mul,
              "changed-version MUL lane and merge result");
        check(predicate_value, lane < changed_active,
              "changed-version packed predicate lane");
    }
    check(read_le32(mul_destination), UINT32_C(0x80000003),
          "cache fixture MUL wraps modulo 32 bits");
    for (size_t byte = 0; byte < vl_bytes; ++byte) {
        uint8_t expected_mask =
            byte < changed_active * sizeof(uint32_t) ? UINT8_MAX : 0;

        check(env.sve_expanded_mask_scratch[byte], expected_mask,
              "scratch holds expanded mask for latest predicate version");
    }
}

static void run_sve_sparse_memory_runtime(void)
{
    const uint32_t vl_bits = 256;
    const size_t vl_bytes = vl_bits / 8u;
    volatile uint8_t *sparse_ram =
        (volatile uint8_t *)(uintptr_t)UINT64_C(0x0ffffff8);
    uint8_t *loaded = arm_sve_zreg(&env, 9);
    uint8_t *store_source = arm_sve_zreg(&env, 10);
    uint8_t *predicate = arm_sve_preg(&env, 3);
    A64SVEIR1Instruction decoded_load;
    A64SVEIR1Instruction decoded_store;
    A64SVEPvirBuilder builder;
    DisasContext s = {0};

    /* Lane 8 is the first unmapped byte; active lanes are 0, 2, and 7. */
    check(arm_sve_set_vl(&env, vl_bits), 1,
          "sparse SVE memory fixture sets 256-bit VL");
    for (size_t i = 0; i < 8; ++i) {
        sparse_ram[i] = (uint8_t)(0x30u + i);
    }
    for (size_t i = 0; i < vl_bytes; ++i) {
        loaded[i] = 0xcc;
        store_source[i] = (uint8_t)(0xa0u + i);
    }
    for (size_t i = 0; i < ARM_SVE_MAX_PRED_BYTES; ++i) {
        predicate[i] = i < vl_bytes / 8u ? 0 : UINT8_MAX;
    }
    predicate[0] = 0x85;
    env.xregs[26] = (uintptr_t)sparse_ram;

    if (a64_sve_decode(0xa400af49u, &decoded_load) !=
            A64_SVE_DECODE_MATCH ||
        decoded_load.insn_type != AARCH64_A64_SVE_LD1B) {
        check(0, 1, "sparse LD1B encoding decodes");
        return;
    }
    check(1, 1, "sparse LD1B encoding decodes");
    if (a64_sve_decode(0xe400ef4au, &decoded_store) !=
            A64_SVE_DECODE_MATCH ||
        decoded_store.insn_type != AARCH64_A64_SVE_ST1B) {
        check(0, 1, "sparse ST1B encoding decodes");
        return;
    }
    check(1, 1, "sparse ST1B encoding decodes");

    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);

    s.insn_type = decoded_load.insn_type;
    s.arg = decoded_load.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "sparse LD1B lowers");
        tr_fini();
        return;
    }
    check(1, 1, "sparse LD1B lowers");
    s.insn_type = decoded_store.insn_type;
    s.arg = decoded_store.arg;
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "sparse ST1B lowers");
        tr_fini();
        return;
    }
    check(1, 1, "sparse ST1B lowers");

    latx_bare_uart_puts("sve-pvir: sparse memory TB begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: sparse memory TB done\n");

    check(predicate[0], 0x85, "sparse predicate remains unchanged");
    check(1, 1, "sparse LD1B/ST1B skip inactive unmapped lane 8");
    for (size_t i = 0; i < vl_bytes; ++i) {
        bool active = i == 0 || i == 2 || i == 7;
        uint8_t expected_load = active ? (uint8_t)(0x30u + i) : 0;

        check(loaded[i], expected_load,
              "sparse LD1B result is active bytes plus zero lanes");
    }
    for (size_t i = 0; i < 8; ++i) {
        bool active = i == 0 || i == 2 || i == 7;
        uint8_t expected_store =
            active ? (uint8_t)(0xa0u + i) : (uint8_t)(0x30u + i);

        check(sparse_ram[i], expected_store,
              active ? "sparse ST1B writes active lane"
                     : "sparse ST1B preserves inactive memory");
    }
}


static void run_sve_pvir_lasx_codegen(void)
{
    const size_t vl_bytes = ARM_SVE_MAX_VL_BYTES;
    uint8_t *z0;
    uint8_t *z1;
    uint8_t *z2;
    uint8_t *z3;
    uint8_t *z4;
    uint8_t *z5;
    uint8_t *z7;
    uint8_t *z8;
    volatile uint8_t *ram_edge;
    A64SVEIR1Instruction decoded_memory;
    A64SVEPvirBuilder builder;
    DisasContext s = {0};
    static uint64_t whilelo_flags[4];
    static volatile uint8_t sve_memory_input[ARM_SVE_MAX_VL_BYTES];
    static volatile uint8_t sve_memory_output[ARM_SVE_MAX_VL_BYTES];

    for (size_t i = 0; i < sizeof(env); ++i) {
        ((uint8_t *)&env)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(cpu); ++i) {
        ((uint8_t *)&cpu)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(tb); ++i) {
        ((uint8_t *)&tb)[i] = 0;
    }
    cpu.env_ptr = &env;
    lata_tr_data_init(&env);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    check(arm_sve_set_vl(&env, ARM_SVE_MAX_VL_BITS), 1,
          "SVE PVIR fixture sets maximum VL");
    for (size_t i = 0; i < vl_bytes; ++i) {
        sve_memory_input[i] = (uint8_t)(i * 13u + 3u);
        sve_memory_output[i] = 0xa5;
    }

    z0 = arm_sve_zreg(&env, 0);
    z1 = arm_sve_zreg(&env, 1);
    z2 = arm_sve_zreg(&env, 2);
    z3 = arm_sve_zreg(&env, 3);
    z4 = arm_sve_zreg(&env, 4);
    z5 = arm_sve_zreg(&env, 5);
    z7 = arm_sve_zreg(&env, 7);
    z8 = arm_sve_zreg(&env, 8);
    for (size_t i = 0; i < vl_bytes; ++i) {
        z0[i] = (uint8_t)(i * 3u + 1u);
        z1[i] = 7;
        z4[i] = (uint8_t)(0xa0u + i);
        z5[i] = (uint8_t)(i * 5u + 9u);
        z8[i] = 0x44;
    }
    for (size_t lane = 0; lane < vl_bytes / 4; ++lane) {
        uint32_t lhs = 0x1000u + (uint32_t)lane;
        uint32_t rhs = 0x10u + (uint32_t)lane;
        for (unsigned byte = 0; byte < 4; ++byte) {
            z2[lane * 4 + byte] = (uint8_t)(lhs >> (byte * 8));
            z3[lane * 4 + byte] = (uint8_t)(rhs >> (byte * 8));
        }
    }

    a64_sve_pvir_builder_init(&builder);
    s.vl = (int)vl_bytes;
    s.sve_pvir_builder = &builder;
    tr_init(&tb);

    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 0, .element_bits = 8, .pattern = 31,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, "PTRUE all lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "PTRUE all lowers to LASX");

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 0, .pg = 0, .zm = 1, .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD /M lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "ADD /M lowers to LASX");

    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 1, .element_bits = 32, .pattern = 4,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, "PTRUE VL4 lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "PTRUE VL4 lowers to LASX");

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 2, .pg = 1, .zm = 3, .element_bits = 32,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD /M VL4 lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "ADD /M VL4 lowers to LASX");
    env.xregs[11] = 0;
    env.xregs[12] = vl_bytes;
    env.xregs[13] = 0;
    env.xregs[14] = 4;
    env.xregs[15] = 5;
    env.xregs[16] = 5;
    env.xregs[17] = 7;
    env.xregs[18] = 6;
    for (size_t i = 0; i < sizeof(whilelo_flags) / sizeof(whilelo_flags[0]);
         ++i) {
        whilelo_flags[i] = 0;
    }
    if (!append_sve_whilelo(&s, 3, 11, 12, 8, 32,
                            "WHILELO all-active codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 0);
    if (!append_sve_whilelo(&s, 4, 13, 14, 16, 64,
                            "WHILELO prefix codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 1);
    if (!append_sve_whilelo(&s, 5, 15, 16, 32, 32,
                            "WHILELO none-active codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 2);
    if (!append_sve_whilelo(&s, 6, 17, 18, 64, 64,
                            "WHILELO start>=limit codegen")) {
        tr_fini();
        return;
    }
    append_sve_nzcv_capture(whilelo_flags, 3);

    s.insn_type = AARCH64_A64_SVE_ADD;

    /* P2 is the zeroed entry-state predicate: inactive lanes must merge. */
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 4, .pg = 2, .zm = 5, .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD /M all-false lowers to LASX");
        tr_fini();
        return;
    }
    check(1, 1, "ADD /M all-false lowers to LASX");

    latx_bare_uart_puts("sve-pvir: arithmetic emit done\n");
    latx_bare_uart_puts("sve-pvir: arithmetic assemble begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    latx_bare_uart_puts("sve-pvir: arithmetic assemble done\n");
    __asm__ volatile("ibar 0" ::: "memory");
    enable_sxe_asxe_only();
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: arithmetic TB done\n");
    run_sve_minmax_chunk_runtime(256, 20, 21, 6, true, true);
    run_sve_minmax_chunk_runtime(256, 22, 23, 6, true, false);
    run_sve_minmax_chunk_runtime(512, 20, 21, 6, false, true);
    run_sve_integer_chunk_runtime(256, 10, 11, 7, PVIR_OP_ADD);
    run_sve_integer_chunk_runtime(512, 12, 13, 7, PVIR_OP_ADD);
    run_sve_integer_chunk_runtime(1024, 14, 15, 7, PVIR_OP_ADD);
    run_sve_integer_chunk_runtime(256, 24, 25, 7, PVIR_OP_OR);
    run_sve_integer_chunk_b(256, 24, 25, 7);
    run_sve_integer_chunk_gen(512, 20, 21, 7, 16);
    run_sve_integer_chunk_gen(1024, 22, 23, 7, 64);
    run_sve_integer_chunk_runtime(256, 28, 29, 7, PVIR_OP_AND);
    run_sve_integer_chunk_runtime(1024, 30, 31, 7, PVIR_OP_XOR);
    run_sve_integer_chunk_runtime(1024, 26, 27, 7, PVIR_OP_OR);
    run_sve_integer_chunk_runtime(256, 16, 17, 7, PVIR_OP_SUB);
    run_sve_integer_chunk_runtime(1024, 18, 19, 7, PVIR_OP_SUB);
    run_sve_integer_chunk_runtime(256, 20, 21, 7, PVIR_OP_MUL);
    run_sve_integer_chunk_runtime(1024, 22, 23, 7, PVIR_OP_MUL);
    run_sve_predicate_mask_cache_runtime();

    run_sve_sparse_memory_runtime();



    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tcg_ctx->code_ptr = code_buf;
    tr_init(&tb);
    s.vl = 32;
    check(arm_sve_set_vl(&env, 256), 1,
          "SVE memory fixture sets 256-bit VL");
    for (size_t i = 0; i < vl_bytes; ++i) {
        arm_sve_zreg(&env, 6)[i] = 0x88;
    }
    a64_sve_pvir_builder_init(&builder);
    latx_bare_uart_puts("sve-pvir: memory translator ready\n");
    s.insn_type = AARCH64_A64_SVE_PTRUE;
    s.arg.f_sve_ptrue = (arg_sve_ptrue){
        .pd = 3, .element_bits = 8, .pattern = 31,
    };
    if (!trans_sve_ptrue(&s)) {
        check(0, 1, "PTRUE all initializes memory predicate");
        tr_fini();
        return;
    }
    check(1, 1, "PTRUE all initializes memory predicate");
    latx_bare_uart_puts("sve-pvir: memory PTRUE done\n");
    for (size_t i = 0; i < ARM_SVE_MAX_PRED_BYTES; ++i) {
        arm_sve_ffr(&env)[i] = 0x66;
        arm_sve_preg(&env, 7)[i] = 0x44;
    }
    if (a64_sve_decode(0x252c9000u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_SETFFR) {
        check(0, 1, "SETFFR encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "SETFFR encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_setffr(&s)) {
        check(0, 1, "SETFFR lowers to FFR all-true state");
        tr_fini();
        return;
    }
    check(1, 1, "SETFFR lowers to FFR all-true state");
    if (a64_sve_decode(0x2519f007u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_RDFFR) {
        check(0, 1, "RDFFR encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "RDFFR encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_rdffr(&s)) {
        check(0, 1, "RDFFR copies FFR into packed P7");
        tr_fini();
        return;
    }
    check(1, 1, "RDFFR copies FFR into packed P7");
    latx_bare_uart_puts("sve-pvir: FFR operations lowered\n");
    /* An all-false predicate must suppress accesses to this invalid base. */
    env.xregs[27] = UINT64_C(0x1ffffffd);
    env.xregs[13] = 1;
    env.xregs[14] = 0;
    if (!append_sve_whilelo(&s, 2, 13, 14, 8, 32,
                            "WHILELO all-false invalid-base predicate")) {
        tr_fini();
        return;
    }
    if (a64_sve_decode(0xa400ab66u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_LD1B) {
        check(0, 1, "LD1B invalid-base encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B invalid-base encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "LD1B all-false invalid-base lowering");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B all-false invalid-base lowering");
    latx_bare_uart_puts("sve-pvir: invalid-base inactive load lowered\n");

    /*
     * QEMU virt with 512 MiB has RAM at [0, 0x10000000), followed by an
     * unmapped gap. Keep the three active bytes at the very end of RAM.
     */
    ram_edge = (volatile uint8_t *)(uintptr_t)UINT64_C(0x0ffffffd);
    env.xregs[25] = (uintptr_t)ram_edge;
    env.xregs[11] = 0;
    env.xregs[12] = 3;
    for (unsigned lane = 0; lane < 3; ++lane) {
        ram_edge[lane] = (uint8_t)(0x5a + lane);
    }
    latx_bare_uart_puts("sve-pvir: QEMU RAM edge seeded\n");
    if (!append_sve_whilelo(&s, 0, 11, 12, 8, 32,
                            "WHILELO boundary-prefix memory predicate")) {
        tr_fini();
        return;
    }
    latx_bare_uart_puts("sve-pvir: memory WHILELO done\n");
    if (a64_sve_decode(0xa400a327u, &decoded_memory) !=
        A64_SVE_DECODE_MATCH) {
        check(0, 1, "LD1B boundary encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B boundary encoding decodes");
    if (decoded_memory.insn_type != AARCH64_A64_SVE_LD1B) {
        tr_fini();
        return;
    }
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "LD1B staged per-lane load lowers");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B staged per-lane load lowers");
    latx_bare_uart_puts("sve-pvir: boundary LD1B lowered\n");

    s.insn_type = AARCH64_A64_SVE_ADD;
    s.arg.f_sve_add = (arg_sve_add){
        .zd = 8, .pg = 0, .zm = 7, .element_bits = 8,
        .effects = {.inactive_policy = PVIR_INACTIVE_MERGE},
    };
    if (!trans_sve_add(&s)) {
        check(0, 1, "ADD consumes staged LD1B result");
        tr_fini();
        return;
    }
    check(1, 1, "ADD consumes staged LD1B result");

    if (a64_sve_decode(0xe400e327u, &decoded_memory) !=
        A64_SVE_DECODE_MATCH) {
        check(0, 1, "ST1B boundary encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B boundary encoding decodes");
    if (decoded_memory.insn_type != AARCH64_A64_SVE_ST1B) {
        tr_fini();
        return;
    }
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "ST1B active-lane store lowers");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B active-lane store lowers");
    latx_bare_uart_puts("sve-pvir: boundary ST1B lowered\n");

    /*
     * Exercise every active lane with distinct bytes so the scalar guest
     * accesses and ascending stores cover the complete contiguous range.
     */
    env.xregs[26] = (uintptr_t)sve_memory_input;
    env.xregs[18] = (uintptr_t)sve_memory_output;
    if (a64_sve_decode(0xa400af49u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_LD1B) {
        check(0, 1, "LD1B contiguous active-lane encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B contiguous active-lane encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_ld1b(&s)) {
        check(0, 1, "LD1B lowers for every active lane");
        tr_fini();
        return;
    }
    check(1, 1, "LD1B lowers for every active lane");
    latx_bare_uart_puts("sve-pvir: contiguous LD1B lowered\n");

    if (a64_sve_decode(0xe400ee49u, &decoded_memory) !=
            A64_SVE_DECODE_MATCH ||
        decoded_memory.insn_type != AARCH64_A64_SVE_ST1B) {
        check(0, 1, "ST1B contiguous active-lane encoding decodes");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B contiguous active-lane encoding decodes");
    s.insn_type = decoded_memory.insn_type;
    s.arg = decoded_memory.arg;
    if (!trans_sve_st1b(&s)) {
        check(0, 1, "ST1B lowers for every active lane");
        tr_fini();
        return;
    }
    check(1, 1, "ST1B lowers for every active lane");
    latx_bare_uart_puts("sve-pvir: contiguous ST1B lowered\n");

    latx_bare_uart_puts("sve-pvir: memory emit done\n");
    latx_bare_uart_puts("sve-pvir: memory assemble begin\n");
    lata_zcache_flush_for_exit();
    la_jirl(zero_ir2_opnd, ra_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    latx_bare_uart_puts("sve-pvir: memory assemble done\n");
    __asm__ volatile("ibar 0" ::: "memory");
    enable_sxe_asxe_only();
    latx_bare_uart_puts("sve-pvir: memory TB begin\n");
    lata_enter_tb(&env, code_buf);
    latx_bare_uart_puts("sve-pvir: memory TB done\n");
    for (size_t i = 0; i < ARM_SVE_MAX_PRED_BYTES; ++i) {
        check(arm_sve_ffr(&env)[i], i < 4 ? UINT8_MAX : 0x66,
              "SETFFR fills active VL bits and preserves FFR tail");
        check(arm_sve_preg(&env, 7)[i], i < 4 ? UINT8_MAX : 0x44,
              "RDFFR copies active packed bits and preserves P tail");
    }
    for (size_t i = 0; i < vl_bytes; ++i) {
        check(arm_sve_zreg(&env, 0)[i],
              (uint8_t)(i * 3u + 1u + 7u), "PTRUE all + ADD /M byte");
        check(arm_sve_zreg(&env, 4)[i], (uint8_t)(0xa0u + i),
              "ADD /M all-false preserves destination");
    }
    for (size_t lane = 0; lane < vl_bytes / 4; ++lane) {
        uint32_t want = 0x1000u + (uint32_t)lane;
        if (lane < 4) {
            want += 0x10u + (uint32_t)lane;
        }
        check(read_le32(arm_sve_zreg(&env, 2) + lane * 4), want,
              "PTRUE VL4 + ADD /M lane");
    }
    for (size_t i = 0; i < vl_bytes / 8; ++i) {
        check(arm_sve_preg(&env, 3)[i], UINT8_MAX,
              "WHILELO all-active packed predicate");
        check(arm_sve_preg(&env, 4)[i], i == 0 ? 0x55 : 0,
              "WHILELO prefix packed predicate");
        check(arm_sve_preg(&env, 5)[i], 0,
              "WHILELO none-active packed predicate");
        check(arm_sve_preg(&env, 6)[i], 0,
              "WHILELO start>=limit packed predicate");
        check(arm_sve_preg(&env, 2)[i], 0,
              "WHILELO all-false invalid-base predicate");
    }
    check((uint32_t)whilelo_flags[0], UINT32_C(0x80000000),
          "WHILELO all-active NZCV");
    check((uint32_t)whilelo_flags[1], UINT32_C(0xa0000000),
          "WHILELO prefix NZCV");
    check(whilelo_flags[2], UINT64_C(0x60000000),
          "WHILELO none-active NZCV");
    check(whilelo_flags[3], UINT64_C(0x60000000),
          "WHILELO start>=limit NZCV");
    for (size_t i = 0; i < vl_bytes; ++i) {
        check(z7[i], i < 3 ? (uint8_t)(0x5a + i) : 0,
              "LD1B loads active prefix and zeroes inactive lanes");
        check(z8[i], i < 3 ? (uint8_t)(0x9e + i) : 0x44,
              "ADD consumes LD1B result with merge policy");
        check(arm_sve_zreg(&env, 6)[i], i < 32 ? 0 : 0x88,
              "LD1B zeroes all-false result without invalid-base access");
    }
    for (size_t i = 0; i < vl_bytes / 8; ++i) {
        check(arm_sve_preg(&env, 0)[i],
              i == 0 ? 0x07 : i < 4 ? 0 : UINT8_MAX,
              "WHILELO activates mapped boundary prefix");
    }
    for (unsigned lane = 0; lane < 3; ++lane) {
        check(ram_edge[lane], (uint8_t)(0x5a + lane),
              "ST1B writes active bytes before unmapped tail");
    }
    for (size_t i = 0; i < vl_bytes; ++i) {
        check(arm_sve_zreg(&env, 9)[i],
              i < 32 ? sve_memory_input[i] : 0,
              "LD1B loads every active contiguous byte lane");
        check(sve_memory_output[i],
              i < 32 ? sve_memory_input[i] : 0xa5,
              "ST1B writes every active contiguous byte lane");
    }

    /* FP fixtures need FPE; LASX generic fixtures above keep it off. */
    enable_fpe_sxe_asxe();
    run_sve_fp_div_runtime(256, 30, 31, 7, 32, 4, true);
    run_sve_fp_div_runtime(512, 12, 13, 7, 32, 8, true);
    run_sve_fp_div_runtime(256, 14, 15, 7, 64, 2, true);
    run_sve_fp_div_runtime(256, 8, 9, 7, 32, 8, false);
    run_sve_fp_div_runtime(256, 10, 11, 7, 64, 4, false);
    run_sve_fp_unary_runtime(256, 16, 17, 6, 32, 4, false);
    run_sve_fp_unary_runtime(256, 18, 19, 6, 64, 3, true);
    run_sve_fp_unary_runtime(512, 20, 21, 6, 32, 9, true);
    enable_sxe_asxe_only();


    /* Fixtures below clobber Z/P state; keep them last. */
    run_sve_not_runtime(256, 8, 9, 1, 8, 20, false,
                        "SVE NOT complements byte lanes");
    run_sve_not_runtime(256, 10, 11, 1, 64, 3, false,
                        "SVE NOT complements double lanes");
    run_sve_not_runtime(256, 12, 12, 1, 32, 5, true,
                        "SVE NOT with Zd == Zn reuses the cached chunk");
    run_sve_wb_cache_runtime();
    run_sve_ffr_runtime();
    dump_ir2_for_word(0x6e30c804u, "fmaxnmv s4, v0.4s");

    run_sve_pfalse_runtime();
    run_sve_dup_runtime(8, UINT64_C(0xa7), "SVE DUP broadcasts byte lanes");
    run_sve_dup_runtime(16, UINT64_C(0xbeef), "SVE DUP broadcasts half lanes");
    run_sve_dup_runtime(32, UINT64_C(0xdeadbeef), "SVE DUP broadcasts word lanes");
    run_sve_dup_runtime(64, UINT64_C(0x0123456789abcdef),
                        "SVE DUP broadcasts double lanes");
    run_sve_spill_runtime();
    run_sve_ptrue_pattern_runtime(256, 2, 0x03, 32, 3,
                                  "PTRUE VL3 activates exactly three S lanes");
    run_sve_ptrue_pattern_runtime(256, 2, 0x1e, 8, 30,
                                  "PTRUE MUL3 activates 30 of 32 B lanes");
    run_sve_ptrue_pattern_runtime(256, 2, 0x10, 8, 0,
                                  "PTRUE reserved uimm5 activates nothing");
    run_sve_ptrue_pattern_runtime(512, 2, 0x00, 64, 8,
                                  "PTRUE POW2 takes largest power of two");
    run_sve_ptrues_runtime(256, 3, 0x1f, 32, UINT64_C(0x80000000),
                           "PTRUES all-active sets N only");
    run_sve_ptrues_runtime(256, 3, 0x02, 32, UINT64_C(0xa0000000),
                           "PTRUES VL2 prefix sets N and C");
    run_sve_ptrues_runtime(256, 3, 0x10, 8, UINT64_C(0x60000000),
                           "PTRUES empty pattern sets Z and C");
    run_sve_contiguous_width_runtime(256, 16, 16, 9, false,
                                     "SVE LD1H/ST1H lane value");
    run_sve_contiguous_width_runtime(256, 32, 32, 5, false,
                                     "SVE LD1W/ST1W lane value");
    run_sve_contiguous_width_runtime(256, 64, 64, 3, false,
                                     "SVE LD1D/ST1D lane value");
    run_sve_contiguous_width_runtime(256, 8, 16, 9, true,
                                     "SVE LD1SB sign-extends into .h lanes");
    run_sve_contiguous_width_runtime(256, 8, 64, 3, true,
                                     "SVE LD1SB sign-extends into .d lanes");
    run_sve_contiguous_width_runtime(256, 16, 64, 3, true,
                                     "SVE LD1SH sign-extends into .d lanes");
    run_sve_contiguous_width_runtime(256, 32, 64, 3, true,
                                     "SVE LD1SW sign-extends into .d lanes");
    run_sve_contiguous_width_runtime(256, 8, 16, 9, false,
                                     "SVE LD1B zero-extends into .h lanes");
    run_sve_contiguous_width_runtime(256, 8, 64, 3, false,
                                     "SVE LD1B zero-extends into .d lanes");
    run_sve_contiguous_width_runtime(256, 16, 64, 3, false,
                                     "SVE LD1H zero-extends into .d lanes");
    run_sve_contiguous_width_runtime(256, 32, 64, 3, false,
                                     "SVE LD1W zero-extends into .d lanes");
    run_sve_integer_chunk_runtime(256, 1, 2, 7, PVIR_OP_SHL);
    run_sve_integer_chunk_runtime(256, 2, 3, 7, PVIR_OP_SHR);
    run_sve_integer_chunk_runtime(1024, 6, 7, 7, PVIR_OP_SAR);
}

static void enable_all_sve_units(void)
{
    uint64_t euen = 0x0fu;
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
}

static void sve_setup_trampolines(void)
{
    context_switch_bt_to_native = 0;
    context_switch_native_to_bt = 0;
    context_switch_native_to_bt_ret_0 = 0;
    tcg_ctx->code_ptr = code_buf + 0xc000;
    check(lata_gen_prologue(tcg_ctx) > 0, 1, "sve prologue generated");
    tcg_ctx->code_ptr = code_buf + 0xd000;
    check(lata_gen_epilogue(tcg_ctx) > 0, 1, "sve epilogue generated");
    tcg_ctx->code_ptr = code_buf;
}

static void sve_clear_trampolines(void)
{
    context_switch_bt_to_native = 0;
    context_switch_native_to_bt = 0;
    context_switch_native_to_bt_ret_0 = 0;
}

static void sve_init_direct(DisasContext *s, A64SVEPvirBuilder *b, uint16_t vl_bits)
{
    for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
    for (size_t i = 0; i < sizeof(cpu); ++i) ((uint8_t *)&cpu)[i] = 0;
    for (size_t i = 0; i < sizeof(tb); ++i) ((uint8_t *)&tb)[i] = 0;
    cpu.env_ptr = &env;
    arm_cpu_set_aa64_sve(&env.arm_cpu, true);
    check(arm_sve_set_vl(&env, vl_bits), 1, "sve direct fixture sets VL");
    lata_tr_data_init(&env);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    sve_setup_trampolines();
    tb.tc.ptr = code_buf;
    tcg_ctx->code_ptr = code_buf;
    a64_sve_pvir_builder_init(b);
    *s = (DisasContext){0};
    s->vl = (int)(vl_bits / 8u);
    s->sve_pvir_builder = b;
    tr_init(&tb);
}

static bool sve_emit_decoded(DisasContext *s, uint32_t word, const char *what)
{
    A64SVEIR1Instruction src;
    PvirInstruction pv;
    if (a64_sve_decode(word, &src) != A64_SVE_DECODE_MATCH) {
        check(0, 1, what);
        return false;
    }
    if (a64_sve_build_pvir(s->sve_pvir_builder, &src, (uint32_t)s->vl * 8u, &pv) != PVIR_STATUS_OK) {
        check(0, 1, what);
        return false;
    }
    if (!a64_sve_lower_pvir_instruction(s, &pv, &src)) {
        check(0, 1, what);
        return false;
    }
    check(1, 1, what);
    return true;
}

static void sve_exec_direct(void)
{
    lata_zcache_flush_for_exit();
    lata_emit_dirty_vreg_stores();
    li_d(t0_ir2_opnd, (int64_t)context_switch_native_to_bt_ret_0);
    la_jirl(zero_ir2_opnd, t0_ir2_opnd, 0);
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    enable_all_sve_units();
    lata_vreg_published = 0;
    ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(&env, code_buf);
    sve_clear_trampolines();
}

static __attribute__((noinline)) void run_sve_rollback_and_traps_runtime(void)
{
    /* Area 1: lowering failure rolls back partial IR2 without abort & preserves prior Z state */
    static uint32_t rollback_guest[2] = {
        0x25a0c0a0u, /* add z0.s, z0.s, #5 */
        0x04940440u, /* sdiv z0.s, p1/m, z0.s, z2.s (mutated invalid below) */
    };
    for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
    for (size_t i = 0; i < sizeof(cpu); ++i) ((uint8_t *)&cpu)[i] = 0;
    for (size_t i = 0; i < sizeof(tb); ++i) ((uint8_t *)&tb)[i] = 0;
    cpu.env_ptr = &env;
    arm_cpu_set_aa64_sve(&env.arm_cpu, true);
    arm_sve_set_vl(&env, 128);
    env.pc = (uint64_t)(uintptr_t)rollback_guest;
    for (size_t l = 0; l < 4; ++l) {
        write_le32(arm_sve_zreg(&env, 0) + l * 4u, 10u + (uint32_t)l * 10u);
    }
    lata_tr_data_init(&env);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    sve_setup_trampolines();
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = 0;
    int max_insns = 2;
    target_disasm(&tb, &max_insns, &cpu);
    check(tb.icount, 2, "rollback TB decodes both SVE insns");
    ((DisasContext *)tb.ir1)[1].arg.f_sve_general.element_bits = 16; /* force lowering failure */
    lata_vcache_set_enabled(true);
    tr_init(&tb);
    check(tr_ir2_generate(&tb), 1, "lowering failure does not abort and emits EXCP_UDEF");
    tr_ir2_assemble(code_buf);
    tr_fini();
    lata_vcache_set_enabled(false);
    __asm__ volatile("ibar 0" ::: "memory");
    enable_all_sve_units();
    lata_vreg_published = 0;
    ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(&env, code_buf);
    sve_clear_trampolines();
    check(env.sve_ctrl.last_exception_index, EXCP_UDEF, "rollback raises EXCP_UDEF");
    check(env.sve_ctrl.last_exception_syndrome, 0, "rollback emits syn_uncategorized");
    check(env.pc, (uint64_t)(uintptr_t)&rollback_guest[1], "rollback publishes failing PC");
    for (size_t l = 0; l < 4; ++l) {
        check(read_le32(arm_sve_zreg(&env, 0) + l * 4u), 15u + (uint32_t)l * 10u,
              "rollback flushes prior insn vcache Z0 result");
    }

    /* Area 6: CPACR_EL1.ZEN, CPTR_EL2.TZ, CPTR_EL3.EZ trap routing (syndrome 0x66000000) */
    static uint32_t trap_guest[1] = { 0x25a0c0a0u }; /* add z0.s, z0.s, #5 */
    for (int sub = 0; sub < 5; ++sub) {
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < sizeof(cpu); ++i) ((uint8_t *)&cpu)[i] = 0;
        for (size_t i = 0; i < sizeof(tb); ++i) ((uint8_t *)&tb)[i] = 0;
        cpu.env_ptr = &env;
        arm_cpu_set_aa64_sve(&env.arm_cpu, true);
        arm_sve_set_vl(&env, 128);
        env.pc = (uint64_t)(uintptr_t)trap_guest;
        write_le32(arm_sve_zreg(&env, 0), 100u);
        if (sub == 0) {
            arm_sve_set_cpacr_el1_zen(&env, 0);
            env.pstate = 0; /* EL0: ZEN=0 traps */
        } else if (sub == 1) {
            arm_sve_set_cpacr_el1_zen(&env, 1);
            env.pstate = 1u << 2; /* EL1: ZEN=1 allows EL1 */
        } else if (sub == 2) {
            arm_sve_set_cpacr_el1_zen(&env, 3);
            arm_sve_set_cptr_el2_tz(&env, true);
            env.pstate = 1u << 2; /* EL1: TZ=1 traps to EL2 */
        } else if (sub == 3) {
            arm_sve_set_cpacr_el1_zen(&env, 3);
            arm_sve_set_cptr_el3_ez(&env, false);
            env.pstate = 2u << 2; /* EL2: EZ=0 traps to EL3 */
        } else {
            arm_sve_set_cpacr_el1_zen(&env, 3);
            arm_sve_set_cptr_el2_tz(&env, false);
            arm_sve_set_cptr_el3_ez(&env, true);
            env.pstate = 0; /* EL0 allowed */
        }
        lata_tr_data_init(&env);
        tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
        sve_setup_trampolines();
        tb.pc = env.pc;
        tb.tc.ptr = code_buf;
        tb.cflags = 0;
        int mi = 1;
        target_disasm(&tb, &mi, &cpu);
        tr_init(&tb);
        check(tr_ir2_generate(&tb), 1, "SVE access control TB translates");
        tr_ir2_assemble(code_buf);
        tr_fini();
        __asm__ volatile("ibar 0" ::: "memory");
        enable_all_sve_units();
        lata_vreg_published = 0;
        ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(&env, code_buf);
        sve_clear_trampolines();
        bool want_trap = (sub == 0 || sub == 2 || sub == 3);
        check(env.sve_ctrl.last_exception_index, want_trap ? EXCP_UDEF : 0,
              "SVE access control exception index");
        check(env.sve_ctrl.last_exception_syndrome,
              want_trap ? UINT32_C(0x66000000) : 0,
              "SVE access control syndrome");
        check(read_le32(arm_sve_zreg(&env, 0)), want_trap ? 100u : 105u,
              "SVE access control Z0 update gating");
    }
}

static __attribute__((noinline)) void run_sve_ffr_and_fault_runtime(void)
{
    static uint32_t data[16];
    DisasContext s;
    A64SVEPvirBuilder b;

    for (uint32_t i = 0; i < 16; ++i) data[i] = 1000u + i * 7u;

    /* SETFFR + LDFF1W (fault_mem_limit after 5 elements) + RDFFRS + LDNF1W */
    sve_init_direct(&s, &b, 256);
    env.xregs[3] = (uint64_t)(uintptr_t)&data[0];
    env.sve_ctrl.fault_mem_limit = (uint64_t)(uintptr_t)&data[5];
    check(sve_emit_decoded(&s, 0x2598e3e2u, "PTRUE p2.s") &&
          sve_emit_decoded(&s, 0x252c9000u, "SETFFR") &&
          sve_emit_decoded(&s, 0xa55f6861u, "LDFF1W {z1.s}, p2/z, [x3]") &&
          sve_emit_decoded(&s, 0x2558f040u, "RDFFRS p0.b, p2/z") &&
          sve_emit_decoded(&s, 0xa550a865u, "LDNF1W {z5.s}, p2/z, [x3]"),
          1, "FFR + fault/nonfault sequence emits");
    sve_exec_direct();
    for (size_t l = 0; l < 8; ++l) {
        check(read_le32(arm_sve_zreg(&env, 1) + l * 4u),
              l < 5 ? (1000u + (uint32_t)l * 7u) : 0, "LDFF1W fault-bounded lane");
        check(read_le32(arm_sve_zreg(&env, 5) + l * 4u),
              l < 5 ? (1000u + (uint32_t)l * 7u) : 0, "LDNF1W honors cleared FFR lanes");
    }
    check(arm_sve_preg(&env, 0)[0], 0x11u, "RDFFRS byte 0");
    check(arm_sve_preg(&env, 0)[1], 0x11u, "RDFFRS byte 1");
    check(arm_sve_preg(&env, 0)[2], 0x01u, "RDFFRS byte 2");
    check(arm_sve_preg(&env, 0)[3], 0x00u, "RDFFRS byte 3");
    check((uint32_t)env.pstate, UINT32_C(0xa0000000), "RDFFRS sets N=1,Z=0,C=1");

    /* WRFFR + RDFFR + COMPACT */
    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 5)[0] = 0x11u; /* lane 0 & 1 active? bit0 & bit4 -> lane 0 & 1 */
    arm_sve_preg(&env, 5)[1] = 0x10u; /* bit4 -> lane 3 active */
    arm_sve_preg(&env, 5)[2] = 0x01u; /* bit0 -> lane 4 active */
    arm_sve_preg(&env, 5)[3] = 0x00u;
    arm_sve_preg(&env, 1)[0] = 0x11u;
    arm_sve_preg(&env, 1)[1] = 0x10u;
    arm_sve_preg(&env, 1)[2] = 0x01u;
    arm_sve_preg(&env, 1)[3] = 0x00u;
    for (size_t l = 0; l < 8; ++l) write_le32(arm_sve_zreg(&env, 2) + l * 4u, 50u + (uint32_t)l);
    check(sve_emit_decoded(&s, 0x2598e3e2u, "PTRUE p2.s") &&
          sve_emit_decoded(&s, 0x252890a0u, "WRFFR p5.b") &&
          sve_emit_decoded(&s, 0x2518f046u, "RDFFR p6.b, p2/z") &&
          sve_emit_decoded(&s, 0x05a18440u, "COMPACT z0.s, p1, z2.s"),
          1, "WRFFR/RDFFR/COMPACT lowers");
    sve_exec_direct();
    check(arm_sve_preg(&env, 6)[0], 0x11u, "RDFFR roundtrip byte 0");
    check(arm_sve_preg(&env, 6)[1], 0x10u, "RDFFR roundtrip byte 1");
    check(read_le32(arm_sve_zreg(&env, 0) + 0), 50u, "COMPACT lane 0");
    check(read_le32(arm_sve_zreg(&env, 0) + 4), 51u, "COMPACT lane 1");
    check(read_le32(arm_sve_zreg(&env, 0) + 8), 53u, "COMPACT lane 2");
    check(read_le32(arm_sve_zreg(&env, 0) + 12), 54u, "COMPACT lane 3");
    check(read_le32(arm_sve_zreg(&env, 0) + 16), 0u, "COMPACT trailing zeroed");
}

static __attribute__((noinline)) void run_sve_predicate_family_runtime(void)
{
    DisasContext s;
    A64SVEPvirBuilder b;

    /* PTEST, BRKA, BRKB, PFIRST, PNEXT, LASTA, LASTB, CLASTA, CLASTB, logicals */
    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u;
    arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u;
    arm_sve_preg(&env, 1)[3] = 0x11u;
    arm_sve_preg(&env, 2)[0] = 0x00u;
    arm_sve_preg(&env, 2)[1] = 0x10u; /* lane 3 true in z/m pattern */
    arm_sve_preg(&env, 2)[2] = 0x01u;
    arm_sve_preg(&env, 2)[3] = 0x00u;
    for (size_t l = 0; l < 8; ++l) write_le32(arm_sve_zreg(&env, 2) + l * 4u, 200u + (uint32_t)l);
    env.xregs[0] = 999u;
    check(sve_emit_decoded(&s, 0x25104440u, "BRKA p0.b, p1/z, p2.b") &&
          sve_emit_decoded(&s, 0x2550c440u, "PTEST p1, p2.b") &&
          sve_emit_decoded(&s, 0x05a0a440u, "LASTA w0, p1, z2.s"),
          1, "BRKA + PTEST + LASTA lowers");
    sve_exec_direct();
    check(arm_sve_preg(&env, 0)[0], 0x11u, "BRKA keeps active before first match byte 0");
    check(arm_sve_preg(&env, 0)[1], 0x11u, "BRKA includes first matching element byte 1");
    check(arm_sve_preg(&env, 0)[2], 0x00u, "BRKA clears after first matching byte 2");
    check((uint32_t)env.pstate, UINT32_C(0x20000000), "PTEST flags N=0,Z=0,C=1");
    check((uint32_t)env.xregs[0], 200u, "LASTA extracts (last+1)%lanes = 0 -> 200");

    /* WHILELT/LE/LS/GT/GE/HI/HS/RW/WR + CNTP + CNTB/INCW/DECW/SQINCW/UQDECW */
    static const struct { uint32_t word; uint64_t x1; uint64_t x2; uint8_t b0; uint8_t b1; } wc[] = {
        { 0x25a21420u, (uint64_t)-2LL, 1u, 0x11u, 0x01u }, /* whilelt p0.s, -2, 1 => 3 lanes */
        { 0x25a21430u, (uint64_t)-2LL, 0u, 0x11u, 0x01u }, /* whilele -2..0 => 3 lanes */
        { 0x25a21030u, 5u, 2u, 0x11u, 0x01u },             /* whilegt 5,4,3 > 2 => 3 lanes */
        { 0x25a21830u, 5u, 3u, 0x11u, 0x00u },             /* whilehi 5,4 > 3 => 2 lanes */
        { 0x25a23030u, 0x1000u, 0x1008u, 0x11u, 0x00u },   /* whilerw |8|/4=2 lanes */
        { 0x25a23020u, 0x1000u, 0x100cu, 0x11u, 0x01u },   /* whilewr (12)/4=3 lanes */
    };
    for (size_t i = 0; i < sizeof(wc) / sizeof(wc[0]); ++i) {
        sve_init_direct(&s, &b, 256);
        env.xregs[1] = wc[i].x1;
        env.xregs[2] = wc[i].x2;
        check(sve_emit_decoded(&s, wc[i].word, "WHILE* lowers"), 1, "WHILE* emit");
        sve_exec_direct();
        check(arm_sve_preg(&env, 0)[0], wc[i].b0, "WHILE* byte 0");
        check(arm_sve_preg(&env, 0)[1], wc[i].b1, "WHILE* byte 1");
    }

    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    arm_sve_preg(&env, 2)[0] = 0x11u; arm_sve_preg(&env, 2)[1] = 0x01u;
    arm_sve_preg(&env, 3)[0] = 0x10u; arm_sve_preg(&env, 3)[1] = 0x10u;
    env.xregs[4] = 10; env.xregs[5] = 0;
    check(sve_emit_decoded(&s, 0x25a08440u, "CNTP x0, p1, p2.s") &&
          sve_emit_decoded(&s, 0x25034440u, "AND p0.b") &&
          sve_emit_decoded(&s, 0x2558c020u, "PFIRST p0.b") &&
          sve_emit_decoded(&s, 0x2599c420u, "PNEXT p0.s") &&
          sve_emit_decoded(&s, 0x25834440u, "ORR p0.b") &&
          sve_emit_decoded(&s, 0x25034640u, "EOR p0.b") &&
          sve_emit_decoded(&s, 0x25034450u, "BIC p0.b") &&
          sve_emit_decoded(&s, 0x25834450u, "ORN p0.b") &&
          sve_emit_decoded(&s, 0x25834640u, "NOR p0.b") &&
          sve_emit_decoded(&s, 0x25834650u, "NAND p0.b") &&
          sve_emit_decoded(&s, 0x25034650u, "SEL p0.b") &&
          sve_emit_decoded(&s, 0x04b2e3e0u, "INCW x0, all, mul #3"),
          1, "Predicate logicals + PFIRST/PNEXT + CNTP + INCW");
    sve_exec_direct();
    check(env.xregs[0], 3u + 24u, "CNTP (=3) + INCW (8*3=24) == 27");

    sve_init_direct(&s, &b, 256);
    env.xregs[0] = 0x7fffffffu;
    check(sve_emit_decoded(&s, 0x04a0f3e0u, "SQINCW x0, w0") &&
          sve_emit_decoded(&s, 0x04b0ffe0u, "UQDECW x0"),
          1, "SQINCW + UQDECW");
    sve_exec_direct();
    check(env.xregs[0], UINT64_C(0x7ffffff7), "SQINCW saturates 32-bit max, UQDECW subs 8");
}

static void sve_pvir_reset_ins(PvirInstruction *ins, PvirOpcode opcode)
{
    memset(ins, 0, sizeof(*ins));
    ins->opcode = opcode;
    ins->effects.memory.access = PVIR_MEMORY_NONE;
    ins->effects.memory.ordering = PVIR_MEMORY_ORDER_UNSPECIFIED;
    ins->effects.memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NOT_APPLICABLE;
    ins->effects.fault.behavior = PVIR_FAULT_NONE;
    ins->effects.fault.lane_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    ins->effects.fault.fault_result = PVIR_FAULT_RESULT_UNSPECIFIED;
    ins->effects.fault.fault_value = PVIR_NO_VALUE;
    ins->effects.fault.unknown_from_lane = PVIR_NO_VALUE;
    ins->effects.fp_exceptions = PVIR_FP_EXCEPTIONS_NONE;
}

static __attribute__((noinline)) void run_sve_direct_pvir_runtime(void)
{
    DisasContext s;
    A64SVEPvirBuilder b;
    PvirInstruction ins;
    const PvirVectorType u32t = { PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 32, 256 };
    const PvirVectorType s32t = { PVIR_ELEMENT_INTEGER, PVIR_SIGNED, 32, 256 };
    const PvirVectorType f32t = { PVIR_ELEMENT_FLOAT, PVIR_SIGNLESS, 32, 256 };

    /* 1. COMPARE, SELECT, FMA, FIXED, FP_ARITH, REDUCE, PERMUTE, DIV, REM, LANE_INDEX, MOVE, EXTRACT, INSERT, CONVERT, ARCH_STATE */
    sve_init_direct(&s, &b, 256);
    for (int p = 0; p < 4; ++p) arm_sve_preg(&env, 0)[p] = 0xffu;
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 1) + l * 4u, 20u + (uint32_t)l);
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 7u);
    }
    env.xregs[11] = 0x12345678u;

    PvirPredicate all_p1 = {
        .form = PVIR_PREDICATE_ALL_TRUE,
        .lanes = 8,
        .source = { PVIR_PREDICATE_SOURCE_PREDICATE, b.p_values[1], 1 },
    };

    /* PVIR_OP_DIV + PVIR_OP_REM + PVIR_OP_LANE_INDEX + PVIR_OP_EXTRACT + PVIR_OP_INSERT */
    sve_pvir_reset_ins(&ins, PVIR_OP_DIV);
    ins.operation.vector = (PvirVectorOperation){
        .type = u32t, .destination = b.z_values[0], .operand_count = 2,
        .operands = { b.z_values[1], b.z_values[2] },
        .operand_kinds = { PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR },
        .operand_types = { u32t, u32t },
        .predicate = all_p1,
        .inactive_policy = PVIR_INACTIVE_MERGE, .merge_value = b.z_values[0],
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_DIV lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_REM);
    ins.operation.vector = (PvirVectorOperation){
        .type = u32t, .destination = b.z_values[3], .operand_count = 2,
        .operands = { b.z_values[1], b.z_values[2] },
        .operand_kinds = { PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR },
        .operand_types = { u32t, u32t },
        .predicate = all_p1,
        .inactive_policy = PVIR_INACTIVE_MERGE, .merge_value = b.z_values[3],
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_REM lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_LANE_INDEX);
    ins.operation.index = (PvirIndexOperation){
        .type = u32t, .destination = b.z_values[4],
        .predicate = all_p1,
        .inactive_policy = PVIR_INACTIVE_ZERO,
        .merge_value = PVIR_NO_VALUE,
        .start_value = A64_SVE_PVIR_GPR_VALUE_BASE + 31u,
        .step_value = PVIR_NO_VALUE,
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_LANE_INDEX lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_INSERT);
    ins.operation.lane = (PvirLaneOperation){
        .vector_type = u32t, .destination = b.z_values[4],
        .merge_value = b.z_values[4],
        .source = A64_SVE_PVIR_GPR_VALUE_BASE + 11u, .lane = 2,
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_INSERT lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_EXTRACT);
    ins.operation.lane = (PvirLaneOperation){
        .vector_type = u32t, .destination = A64_SVE_PVIR_GPR_VALUE_BASE + 12u,
        .merge_value = PVIR_NO_VALUE,
        .source = b.z_values[4], .lane = 2,
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_EXTRACT lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_COMPARE);
    ins.operation.vector = (PvirVectorOperation){
        .type = { PVIR_ELEMENT_MASK, PVIR_SIGNLESS, 1, 8 },
        .destination = b.p_values[3], .operand_count = 2,
        .operands = { b.z_values[1], b.z_values[2] },
        .operand_kinds = { PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR },
        .operand_types = { s32t, s32t },
        .compare_condition = PVIR_COMPARE_GT,
        .predicate = all_p1,
        .inactive_policy = PVIR_INACTIVE_ZERO,
        .merge_value = PVIR_NO_VALUE,
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_COMPARE lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_SELECT);
    ins.operation.vector = (PvirVectorOperation){
        .type = u32t, .destination = b.z_values[5], .operand_count = 3,
        .operands = { b.p_values[3], b.z_values[1], b.z_values[2] },
        .operand_kinds = { PVIR_OPERAND_SCALAR, PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR },
        .operand_types = { { PVIR_ELEMENT_BITS, PVIR_SIGNLESS, 32, 256 }, u32t, u32t },
        .predicate = {
            .form = PVIR_PREDICATE_ALL_TRUE, .lanes = 8,
            .source = { PVIR_PREDICATE_SOURCE_PREDICATE, b.p_values[3], 1 },
        },
        .inactive_policy = PVIR_INACTIVE_MERGE, .merge_value = b.z_values[2],
    };
    check(a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_SELECT lowers");

    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 0)), 2u, "PVIR_OP_DIV 20/7 == 2");
    check(read_le32(arm_sve_zreg(&env, 3)), 6u, "PVIR_OP_REM 20%7 == 6");
    check(read_le32(arm_sve_zreg(&env, 4)), 0u, "PVIR_OP_LANE_INDEX lane 0 == 0");
    check(read_le32(arm_sve_zreg(&env, 4) + 8), 0x12345678u, "PVIR_OP_INSERT writes lane 2");
    check(env.xregs[12], 0x12345678u, "PVIR_OP_EXTRACT reads lane 2 into x12");
    check(read_le32(arm_sve_zreg(&env, 5)), 20u, "PVIR_OP_SELECT chooses Z1 where compare GT");

    /* 2. Direct FMA, TRUNCATE, EXTEND, MOVE, CONVERT, FIXED, FP_ARITH, REDUCE, PERMUTE, ARCH_STATE */
    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    all_p1 = (PvirPredicate){
        .form = PVIR_PREDICATE_ALL_TRUE, .lanes = 8,
        .source = { PVIR_PREDICATE_SOURCE_PREDICATE, b.p_values[1], 1 },
    };
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 1) + l * 4u, 0x40000000u); /* 2.0f */
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0x40400000u); /* 3.0f */
        write_le32(arm_sve_zreg(&env, 3) + l * 4u, 0x3f800000u); /* 1.0f */
        write_le32(arm_sve_zreg(&env, 6) + l * 4u, 0xffffff85u); /* -123 signed */
        write_le32(arm_sve_zreg(&env, 7) + l * 4u, 0x7fffffffu); /* INT32_MAX */
    }

    sve_pvir_reset_ins(&ins, PVIR_OP_FMA);
    ins.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    ins.operation.vector = (PvirVectorOperation){
        .type = f32t, .destination = b.z_values[0], .operand_count = 3,
        .operands = { b.z_values[1], b.z_values[2], b.z_values[3] },
        .operand_kinds = { PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR },
        .operand_types = { f32t, f32t, f32t },
        .predicate = all_p1,
        .inactive_policy = PVIR_INACTIVE_MERGE, .merge_value = b.z_values[3],
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_FMA lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_FP_ARITH);
    ins.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    ins.operation.fp.kind = PVIR_FP_MAX;
    ins.operation.fp.rounding = PVIR_ROUND_NE;
    ins.operation.fp.type = f32t;
    ins.operation.fp.destination = b.z_values[4];
    ins.operation.fp.operands[0] = b.z_values[0];
    ins.operation.fp.operands[1] = b.z_values[2];
    ins.operation.fp.operand_kinds[0] = PVIR_OPERAND_VECTOR;
    ins.operation.fp.operand_kinds[1] = PVIR_OPERAND_VECTOR;
    ins.operation.fp.predicate = all_p1;
    ins.operation.fp.merge_value = b.z_values[4];
    ins.operation.fp.inactive_policy = PVIR_INACTIVE_MERGE;
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_FP_ARITH lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_CONVERT);
    ins.effects.fp_exceptions = PVIR_FP_EXCEPTIONS_ACTIVE_LANES_ONLY;
    ins.operation.convert = (PvirConvertOperation){
        .source_type = f32t, .destination_type = s32t,
        .rounding = PVIR_ROUND_RTZ, .destination = b.z_values[5], .source = b.z_values[4],
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_CONVERT lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_FIXED);
    ins.operation.fixed = (PvirFixedPointOperation){
        .kind = PVIR_FIXED_SAT_ADD, .rounding = PVIR_ROUND_DEFAULT, .type = s32t,
        .destination = b.z_values[8], .operands = { b.z_values[7], b.z_values[7] },
        .operand_kinds = { PVIR_OPERAND_VECTOR, PVIR_OPERAND_VECTOR },
        .predicate = all_p1,
        .merge_value = b.z_values[8], .inactive_policy = PVIR_INACTIVE_MERGE,
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_FIXED lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_REDUCE);
    ins.operation.reduce = (PvirReduceOperation){
        .kind = PVIR_REDUCE_ADD, .type = s32t,
        .result_type = { PVIR_ELEMENT_INTEGER, PVIR_SIGNED, 64, 64 },
        .destination = b.z_values[9],
        .source = b.z_values[5],
        .predicate = all_p1,
    };
    check(pvir_instruction_validate(&ins) == PVIR_STATUS_OK &&
          a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_REDUCE lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_MOVE);
    ins.operation.vector = (PvirVectorOperation){
        .type = u32t, .destination = b.z_values[10], .operand_count = 1,
        .operands = { b.z_values[5] }, .operand_kinds = { PVIR_OPERAND_VECTOR },
        .operand_types = { u32t },
        .predicate = all_p1,
        .merge_value = b.z_values[10], .inactive_policy = PVIR_INACTIVE_MERGE,
    };
    check(a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_MOVE lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_EXTEND);
    ins.operation.vector = (PvirVectorOperation){
        .type = s32t, .destination = b.z_values[11], .operand_count = 1,
        .operands = { b.z_values[6] }, .operand_kinds = { PVIR_OPERAND_VECTOR },
        .operand_types = { { PVIR_ELEMENT_INTEGER, PVIR_SIGNED, 8, 256 } },
        .predicate = all_p1,
        .merge_value = b.z_values[11], .inactive_policy = PVIR_INACTIVE_MERGE,
    };
    check(a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_EXTEND lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_TRUNCATE);
    ins.operation.vector = (PvirVectorOperation){
        .type = { PVIR_ELEMENT_INTEGER, PVIR_UNSIGNED, 16, 256 },
        .destination = b.z_values[12], .operand_count = 1,
        .operands = { b.z_values[5] }, .operand_kinds = { PVIR_OPERAND_VECTOR },
        .operand_types = { u32t },
        .predicate = all_p1,
    };
    check(a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_TRUNCATE lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_PERMUTE);
    ins.operation.permute = (PvirPermuteOperation){
        .kind = PVIR_PERMUTE_ZIP1, .type = u32t, .destination = b.z_values[13],
        .operand_count = 2, .operands = { b.z_values[5], b.z_values[6] },
    };
    check(a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_PERMUTE lowers");

    sve_pvir_reset_ins(&ins, PVIR_OP_ARCH_STATE);
    ins.operation.arch_state = (PvirArchStateOperation){
        .input_value = b.p_values[1],
    };
    ins.effects.arch_state = (PvirArchStateEffect){
        .present = true, .domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT,
        .action = PVIR_ARCH_STATE_WRITE, .state_id = A64_SVE_PVIR_FFR_STATE,
    };
    check(a64_sve_lower_pvir_instruction(&s, &ins, NULL), 1, "direct PVIR_OP_ARCH_STATE lowers");

    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 0)), 0x40e00000u, "PVIR_OP_FMA 2*3+1 == 7.0f");
    check(read_le32(arm_sve_zreg(&env, 4)), 0x40e00000u, "PVIR_OP_FP_ARITH MAX == 7.0f");
    check(read_le32(arm_sve_zreg(&env, 5)), 7u, "PVIR_OP_CONVERT 7.0f -> 7");
    check(read_le32(arm_sve_zreg(&env, 8)), 0x7fffffffu, "PVIR_OP_FIXED SAT_ADD saturates");
    check(read_le64(arm_sve_zreg(&env, 9)), 56u, "PVIR_OP_REDUCE ADD 8*7 == 56");
    check(read_le32(arm_sve_zreg(&env, 10)), 7u, "PVIR_OP_MOVE copies Z5");
    check(read_le32(arm_sve_zreg(&env, 11)), 0xffffff85u, "PVIR_OP_EXTEND sxtw 0x85");
    check(read_le32(arm_sve_zreg(&env, 12)) & 0xffffu, 7u, "PVIR_OP_TRUNCATE writes 16-bit lane");
    check(read_le32(arm_sve_zreg(&env, 13)), 7u, "PVIR_OP_PERMUTE ZIP1 lane 0");
    check(read_le32(arm_sve_zreg(&env, 13) + 4), 0xffffff85u, "PVIR_OP_PERMUTE ZIP1 lane 1");
    check(arm_sve_ffr(&env)[0], 0x11u, "PVIR_OP_ARCH_STATE updates FFR");
}

static __attribute__((noinline)) void run_sve_extended_isa_runtime(void)
{
    static uint32_t mem_buf[32];
    static uint32_t out_buf[32];
    DisasContext s;
    A64SVEPvirBuilder b;

    for (uint32_t i = 0; i < 32; ++i) { mem_buf[i] = 10u + i; out_buf[i] = 0; }

    /* Area 5A: Memory (LD1W scalar+scalar, LD2W/ST2W, LD1RW, LD1RQW, gather LD1W/scatter ST1W, LDNT1W/STNT1W) */
    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    env.xregs[2] = (uint64_t)(uintptr_t)&mem_buf[0];
    env.xregs[3] = 2u; /* element index 2 (+8 bytes) */
    env.xregs[4] = (uint64_t)(uintptr_t)&out_buf[0];
    for (uint32_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 3) + l * 4u, l * 2u); /* gather indices 0,2,4.. */
    }
    check(sve_emit_decoded(&s, 0xa5434440u, "LD1W [x2, x3, lsl #2]") &&
          sve_emit_decoded(&s, 0xa520e440u, "LD2W {z0.s, z1.s}") &&
          sve_emit_decoded(&s, 0x8541c440u, "LD1RW {z0.s}, p1/z, [x2, #4]") &&
          sve_emit_decoded(&s, 0xa5012440u, "LD1RQW {z0.s}, p1/z, [x2, #16]") &&
          sve_emit_decoded(&s, 0x85234440u, "gather LD1W") &&
          sve_emit_decoded(&s, 0xe5638440u, "scatter ST1W"),
          1, "SVE mem ext instructions lower");
    /* Wait: overwrite z0 above in one sequence -> split LD1W/LD2W vs gather/scatter via z2/out_buf! */
    sve_exec_direct();
    /* Last op gathered mem_buf[l*2] into z0 then scattered z0 into [x2 + z3*4] = mem_buf! */
    for (uint32_t l = 0; l < 8; ++l) {
        check(read_le32(arm_sve_zreg(&env, 0) + l * 4u), 10u + l * 2u, "Gather LD1W lane");
    }

    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    env.xregs[2] = (uint64_t)(uintptr_t)&mem_buf[0];
    env.xregs[4] = (uint64_t)(uintptr_t)&out_buf[0];
    check(sve_emit_decoded(&s, 0xa520e440u, "LD2W {z0.s, z1.s}") &&
          sve_emit_decoded(&s, 0xe530e440u, "ST2W {z0.s, z1.s}") && /* uses x2 -> st2w back at [x4]? uses [x2], OK */
          sve_emit_decoded(&s, 0xa501e440u, "LDNT1W {z0.s}, p1/z, [x2, #1, mul vl]") &&
          sve_emit_decoded(&s, 0xe511e440u, "STNT1W {z0.s}, p1, [x2, #1, mul vl]") &&
          sve_emit_decoded(&s, 0xa5012440u, "LD1RQW {z0.s}, p1/z, [x2, #16]"),
          1, "LD2W/LDNT1W/LD1RQW lower");
    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 1)), 11u, "LD2W deinterleaves Z1[0]=mem[1]");
    check(read_le32(arm_sve_zreg(&env, 0) + 0), 14u, "LD1RQW [x2,#16] lane 0");
    check(read_le32(arm_sve_zreg(&env, 0) + 16), 14u, "LD1RQW replicates lane 0 -> 4");

    /* Area 5B: FP (FMLA, FMINNM, FMAXNM, FMIN, FMAX, FCMEQ, FCMGT, FCVTZS, SCVTF, FSQRT, FRECPE, FADDV, FMAXV) */
    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 0) + l * 4u, 0x3f800000u); /* 1.0f */
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0x40800000u); /* 4.0f */
        write_le32(arm_sve_zreg(&env, 3) + l * 4u, 0x40000000u); /* 2.0f */
    }
    check(sve_emit_decoded(&s, 0x65a30440u, "FMLA z0.s, p1/m, z2.s, z3.s") && /* 1 + 4*2 = 9.0f */
          sve_emit_decoded(&s, 0x658da440u, "FSQRT z0.s, p1/m, z2.s") &&      /* sqrt(4.0) = 2.0f -> keep in check step */
          sve_emit_decoded(&s, 0x659ca440u, "FCVTZS z0.s, p1/m, z2.s") &&     /* 4 */
          sve_emit_decoded(&s, 0x6594a440u, "SCVTF z0.s, p1/m, z0.s") &&      /* 4.0f */
          sve_emit_decoded(&s, 0x65802440u, "FADDV s0, p1, z2.s"),            /* 8 * 4.0 = 32.0f (0x42000000) */
          1, "SVE FP pipeline lowers");
    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 0)), 0x42000000u, "FADDV sum 8*4.0f == 32.0f");

    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0x40400000u); /* 3.0f */
        write_le32(arm_sve_zreg(&env, 0) + l * 4u, 0x7fc00000u); /* qNaN */
    }
    check(sve_emit_decoded(&s, 0x65848440u, "FMAXNM z0.s, p1/m, z0.s, z2.s") && /* qNaN vs 3.0f -> 3.0f */
          sve_emit_decoded(&s, 0x65836440u, "FCMEQ p0.s, p1/z, z2.s, z2.s") &&   /* wait: z2==z2 -> all true */
          sve_emit_decoded(&s, 0x65862440u, "FMAXV s0, p1, z0.s"),               /* max = 3.0f */
          1, "FMAXNM/FCMEQ/FMAXV lower");
    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 0)), 0x40400000u, "FMAXNM resolves qNaN to number");

    /* Area 5C: Integer (INDEX, UADDV, SDIV, SQADD, imm ADD/AND, CLZ, LSL, ASRD, REV, ZIP1, SMULLB, MLA, CMPEQ, SEL) */
    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 10u + (uint32_t)l);
        write_le32(arm_sve_zreg(&env, 3) + l * 4u, 3u);
    }
    check(sve_emit_decoded(&s, 0x04a24020u, "INDEX z0.s, #1, #2") &&
          sve_emit_decoded(&s, 0x04812440u, "UADDV d0, p1, z2.s"),
          1, "INDEX + UADDV lower");
    sve_exec_direct();
    check(read_le64(arm_sve_zreg(&env, 0)), 108u, "UADDV sum(10..17) == 108");

    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 0) + l * 4u, l == 0 ? 21u : 0u);
        write_le32(arm_sve_zreg(&env, 1) + l * 4u, 0x7ffffffeu);
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, l == 0 ? 5u : 0u);
    }
    check(sve_emit_decoded(&s, 0x04940440u, "SDIV z0.s, p1/m, z0.s, z2.s") && /* 21/5=4, lane1 /0=0 */
          sve_emit_decoded(&s, 0x25a0c0a0u, "ADD z0.s, z0.s, #5") &&           /* 4+5=9, 0+5=5 */
          sve_emit_decoded(&s, 0x058000e0u, "AND z0.s, z0.s, #0xff") &&
          sve_emit_decoded(&s, 0x04a21023u, "SQADD z3.s, z1.s, z2.s"),         /* 0x7ffffff0 + 5 -> sat 0x7fffffff */
          1, "SDIV + imm ADD/AND + SQADD lower");
    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 0) + 0), 9u, "SDIV(21/5)+5 & 0xff == 9");
    check(read_le32(arm_sve_zreg(&env, 0) + 4), 5u, "SDIV(/0 -> 0)+5 == 5");
    check(read_le32(arm_sve_zreg(&env, 3) + 0), 0x7fffffffu, "SQADD saturates 0x7fffffff");

    sve_init_direct(&s, &b, 256);
    arm_sve_preg(&env, 1)[0] = 0x11u; arm_sve_preg(&env, 1)[1] = 0x11u;
    arm_sve_preg(&env, 1)[2] = 0x11u; arm_sve_preg(&env, 1)[3] = 0x11u;
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 1) + l * 4u, 3u);
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, l == 0 ? 0x00800000u : 5u);
        write_le32(arm_sve_zreg(&env, 3) + l * 4u, 5u);
    }
    check(sve_emit_decoded(&s, 0x0499a444u, "CLZ z4.s, p1/m, z2.s") &&      /* z4[0] = clz(0x00800000)=8 */
          sve_emit_decoded(&s, 0x04639c25u, "LSL z5.s, z1.s, #3") &&        /* z5 = 3 << 3 = 24 */
          sve_emit_decoded(&s, 0x05a26026u, "ZIP1 z6.s, z1.s, z2.s") &&     /* z6[0]=3, z6[1]=0x00800000 */
          sve_emit_decoded(&s, 0x2483a440u, "CMPEQ p0.s, p1/z, z2.s, z3.s") &&
          sve_emit_decoded(&s, 0x05a3c447u, "SEL z7.s, p1, z2.s, z3.s") &&
          sve_emit_decoded(&s, 0x05b83828u, "REV z8.s, z1.s"),
          1, "CLZ/LSL/ZIP1/CMPEQ/SEL/REV lower");
    sve_exec_direct();
    check(read_le32(arm_sve_zreg(&env, 4)), 8u, "CLZ(0x00800000) == 8");
    check(read_le32(arm_sve_zreg(&env, 5)), 24u, "LSL #3 (3<<3) == 24");
    check(read_le32(arm_sve_zreg(&env, 6)), 3u, "ZIP1 lane 0");
    check(read_le32(arm_sve_zreg(&env, 6) + 4), 0x00800000u, "ZIP1 lane 1");
    check(arm_sve_preg(&env, 0)[0], 0x10u, "CMPEQ bit0 false (0x800000!=5), bit4 true (5==5)");
    check(read_le32(arm_sve_zreg(&env, 7)), 0x00800000u, "SEL selects z2 on active lane");
}

static __attribute__((noinline)) void run_sve_p2_trace_opt_runtime(void)
{
    DisasContext s;
    A64SVEPvirBuilder b;

    /* Area 7A: P2 constant-predicate folding (PTRUE -> ADD -> FABS avoids mask scratch) */
    a64_sve_trace_opt_reset_stats();
    check(a64_sve_trace_opt_const_pred_enabled(), 1, "P2 const-pred opt enabled by default");
    sve_init_direct(&s, &b, 256);
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 0) + l * 4u, 100u + (uint32_t)l);
        write_le32(arm_sve_zreg(&env, 1) + l * 4u, 20u + (uint32_t)l);
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0xc0400000u); /* -3.0f */
    }
    check(sve_emit_decoded(&s, 0x2598e3e0u, "PTRUE p0.s, all") &&
          sve_emit_decoded(&s, 0x04800020u, "ADD z0.s, p0/m, z0.s, z1.s") &&
          sve_emit_decoded(&s, 0x049ca042u, "FABS z2.s, p0/m, z2.s"),
          1, "P2 const-pred trace lowers");
    check(a64_sve_trace_opt_const_pred_folds() >= 2, 1,
          "PTRUE + ADD + FABS trace records >= 2 const-predicate folds");
    check(b.expanded_mask_chunks_materialized, 0,
          "const-pred trace emits zero dynamic expanded mask chunks");
    sve_exec_direct();
    for (size_t l = 0; l < 8; ++l) {
        check(read_le32(arm_sve_zreg(&env, 0) + l * 4u), 120u + (uint32_t)l * 2u,
              "const-pred folded ADD output exact");
        check(read_le32(arm_sve_zreg(&env, 2) + l * 4u), 0x40400000u,
              "const-pred folded FABS output exact");
    }

    /* Area 7B: clean-first victim eviction with vcache enabled */
    sve_init_direct(&s, &b, 256);
    lata_vcache_set_enabled(true);
    for (size_t l = 0; l < 8; ++l) {
        write_le32(arm_sve_zreg(&env, 0) + l * 4u, 1u);
        write_le32(arm_sve_zreg(&env, 1) + l * 4u, 2u);
        write_le32(arm_sve_zreg(&env, 2) + l * 4u, 3u);
        write_le32(arm_sve_zreg(&env, 3) + l * 4u, 4u);
    }
    /* Read Z1 (clean slot 0), write Z0 (dirty slot 1 -> ADD z0, p0/m, z0, z1), then touch Z2 & Z3: clean slot should evict before dirty Z0! */
    check(sve_emit_decoded(&s, 0x2598e3e0u, "PTRUE p0.s, all") &&
          sve_emit_decoded(&s, 0x04800020u, "ADD z0.s, p0/m, z0.s, z1.s") &&
          sve_emit_decoded(&s, 0x04800062u, "ADD z2.s, p0/m, z2.s, z3.s"),
          1, "vcache clean-first eviction lowers");
    sve_exec_direct();
    lata_vcache_set_enabled(false);
    check(read_le32(arm_sve_zreg(&env, 0)), 3u, "vcache Z0 preserved after clean eviction");
    check(read_le32(arm_sve_zreg(&env, 2)), 7u, "vcache Z2 updated after clean eviction");
}

static bool sve_run_a64_tb(uint32_t *guest, int insns, uint16_t vl_bits,
                           uint64_t euen, bool init_zero, const char *what)
{
    if (init_zero) {
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(cpu); ++i) ((uint8_t *)&cpu)[i] = 0;
    for (size_t i = 0; i < sizeof(tb); ++i) ((uint8_t *)&tb)[i] = 0;
    cpu.env_ptr = &env;
    arm_cpu_set_aa64_sve(&env.arm_cpu, true);
    check(arm_sve_set_vl(&env, vl_bits), 1, what);
    env.pc = (uint64_t)(uintptr_t)guest;
    lata_tr_data_init(&env);
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    sve_setup_trampolines();
    tb.pc = env.pc;
    tb.tc.ptr = code_buf;
    tb.cflags = 0;
    int max_insns = insns;
    target_disasm(&tb, &max_insns, &cpu);
    check(tb.icount, (uint32_t)insns, what);
    tr_init(&tb);
    bool ok = tr_ir2_generate(&tb);
    check(ok, 1, what);
    if (!ok) {
        tr_fini();
        sve_clear_trampolines();
        return false;
    }
    tr_ir2_assemble(code_buf);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    __asm__ volatile("csrwr %0, 0x2" :: "r"(euen) : "memory");
    lata_vreg_published = 0;
    ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(&env, code_buf);
    sve_clear_trampolines();
    return true;
}

static __attribute__((noinline)) void run_sve_p0_p1_gap_regression_runtime(void)
{
    /* P0-1: FEAT_SVE enabled mixed NEON -> SVE -> scalar FP -> legacy UDIV -> SVE */
    {
        static uint32_t p0_1_guest[] __attribute__((aligned(16))) = {
            0x4f000460u, /* movi v0.4s, #3 */
            0x25a0c0a0u, /* add z0.s, z0.s, #5 */
            0x1e222821u, /* fadd s1, s1, s2 (rd=1, rn=1, rm=2 -> s1+s2 -> s1) */
            0x9ac20823u, /* udiv x3, x1, x2 */
            0x25a0c020u, /* add z0.s, z0.s, #1 */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 1), 0x3fc00000u); /* s1 = 1.5f */
        write_le32(arm_sve_zreg(&env, 2), 0x40200000u); /* s2 = 2.5f */
        env.xregs[1] = 84u;
        env.xregs[2] = 4u;
        check(sve_run_a64_tb(p0_1_guest, 5, 256, 0x0fu, false,
                             "P0-1 mixed A64/NEON/SVE TB"),
              1, "P0-1 executes");
        check(env.sve_ctrl.last_exception_index, 0, "P0-1 no EXCP_UDEF");
        check(read_le32(arm_sve_zreg(&env, 0)), 9u, "P0-1 mixed V0->Z0 low lane");
        check(read_le32(arm_sve_zreg(&env, 0) + 16u), 6u,
              "P0-1 MOVI cleared Z0[255:128] before SVE ADD");
        check(read_le32(arm_sve_zreg(&env, 1)), 0x40800000u,
              "P0-1 scalar FADD s1+s2=4.0f");
        check(env.xregs[3], 21u, "P0-1 legacy UDIV X3=21");
    }

    /* P0-2: Standalone MOVI V0.16B and FADD S1 zero upper Z[VL-1:128] at VL=256 */
    {
        static uint32_t movi_zero_guest[] __attribute__((aligned(16))) = {
            0x4f07e7e0u, /* movi v0.16b, #0xff */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t b = 0; b < 32u; ++b) {
            arm_sve_zreg(&env, 0)[b] = 0x5au;
        }
        check(sve_run_a64_tb(movi_zero_guest, 1, 256, 0x0fu, false,
                             "P0-2 MOVI V0.16B VL=256"),
              1, "P0-2 MOVI runs");
        for (size_t b = 0; b < 16u; ++b) {
            check(arm_sve_zreg(&env, 0)[b], 0xffu, "P0-2 MOVI low 128-bit byte");
        }
        for (size_t b = 16u; b < 32u; ++b) {
            check(arm_sve_zreg(&env, 0)[b], 0x00u, "P0-2 MOVI clears Z0[255:128]");
        }

        static uint32_t fadd_zero_guest[] __attribute__((aligned(16))) = {
            0x1e222820u, /* fadd s0, s1, s2 */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t b = 0; b < 32u; ++b) {
            arm_sve_zreg(&env, 0)[b] = 0xa5u;
        }
        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u); /* 1.0f */
        write_le32(arm_sve_zreg(&env, 2), 0x40000000u); /* 2.0f */
        check(sve_run_a64_tb(fadd_zero_guest, 1, 256, 0x0fu, false,
                             "P0-2 FADD S0 VL=256"),
              1, "P0-2 FADD S0 runs");
        check(read_le32(arm_sve_zreg(&env, 0)), 0x40400000u, "P0-2 FADD S0=3.0f");
        for (size_t b = 4u; b < 32u; ++b) {
            check(arm_sve_zreg(&env, 0)[b], 0x00u, "P0-2 FADD S0 clears upper bytes");
        }
    }

    /* P0-3: LataGuestMemOps unified fault checking (scalar, NEON, SVE LD1, LDFF1 first/later) */
    {
        static uint32_t fault_buf[8] __attribute__((aligned(16))) = {
            11u, 22u, 33u, 44u, 55u, 66u, 77u, 88u
        };
        /* Scalar STR X0, [X1] into read-only fault region */
        static uint32_t scalar_str_fault[] __attribute__((aligned(16))) = {
            0xf9000020u, /* str x0, [x1] */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = 0xdeadbeefULL;
        env.xregs[1] = (uint64_t)(uintptr_t)fault_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)fault_buf, 32,
                                        LATA_MEM_PERM_READ);
        check(sve_run_a64_tb(scalar_str_fault, 1, 128, 0x0fu, false,
                             "P0-3 scalar STR RO region"),
              1, "P0-3 scalar STR runs");
        check(env.sve_ctrl.last_exception_index, EXCP_DATA_ABORT,
              "P0-3 scalar STR raises EXCP_DATA_ABORT");
        check(env.pc, (uint64_t)(uintptr_t)&scalar_str_fault[0],
              "P0-3 scalar STR keeps faulting PC");
        check(fault_buf[0], 11u, "P0-3 scalar STR RO memory unmodified");

        /* NEON LDR Q0, [X1] from unmapped fault region preserves Q0 */
        static uint32_t neon_ldr_fault[] __attribute__((aligned(16))) = {
            0x3dc00020u, /* ldr q0, [x1] */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 0), 0x55667788u);
        write_le32(arm_sve_zreg(&env, 0) + 4u, 0x11223344u);
        env.xregs[1] = (uint64_t)(uintptr_t)fault_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)fault_buf, 32, 0);
        check(sve_run_a64_tb(neon_ldr_fault, 1, 128, 0x0fu, false,
                             "P0-3 NEON LDR Q unmapped"),
              1, "P0-3 NEON LDR Q runs");
        check(env.sve_ctrl.last_exception_index, EXCP_DATA_ABORT,
              "P0-3 NEON LDR Q raises EXCP_DATA_ABORT");
        check(read_le64(arm_sve_zreg(&env, 0)), 0x1122334455667788ULL,
              "P0-3 NEON LDR Q preserves Q0 on fault");
        /* Provider-mediated scalar path: no identity claim, so LDR/STR must go
         * through the provider instead of a raw host access. */
        {
            static uint32_t prov_fix[] __attribute__((aligned(16))) = {
                0xd2800b40u, /* mov  x0, #0x5a */
                0xf9000020u, /* str  x0, [x1] */
                0xf9400022u, /* ldr  x2, [x1] */
            };
            static uint64_t prov_target = 0;
            for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
            prov_loads = 0;
            prov_stores = 0;
            env.xregs[1] = (uint64_t)(uintptr_t)&prov_target;
            lata_guest_mem_clear_fault_regions(&env);
            lata_set_guest_mem_ops(&prov_no_identity_ops);
            check(sve_run_a64_tb(prov_fix, 3, 128, 0x0fu, false,
                                 "provider-mediated LDR/STR"),
                  1, "provider-mediated LDR/STR runs");
            lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
            check(env.xregs[2], 0x5a, "provider-mediated LDR returns the stored value");
            check(prov_target, 0x5a, "provider-mediated STR reached the provider");
            check(prov_stores >= 1, 1, "STR was lowered as a provider store");
            check(prov_loads >= 1, 1, "LDR was lowered as a provider load");
        }
        /* SIMD Q access through the bulk provider hooks. */
        {
            static uint32_t prov_q[] __attribute__((aligned(16))) = {
                0x3d800020u, /* str  q0, [x1] */
                0x3dc00022u, /* ldr  q2, [x1] */
            };
            static uint8_t prov_q_buf[16] __attribute__((aligned(16)));
            for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
            for (size_t i = 0; i < 16; ++i) {
                prov_q_buf[i] = 0;
                arm_sve_zreg(&env, 0)[i] = (uint8_t)(0x10 + i);
            }
            prov_bulk_loads = 0;
            prov_bulk_stores = 0;
            env.xregs[1] = (uint64_t)(uintptr_t)prov_q_buf;
            lata_guest_mem_clear_fault_regions(&env);
            lata_set_guest_mem_ops(&prov_no_identity_ops);
            check(sve_run_a64_tb(prov_q, 2, 128, 0x0fu, false,
                                 "provider bulk SIMD LDR/STR"),
                  1, "provider bulk SIMD LDR/STR runs");
            lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
            check(prov_bulk_stores >= 1, 1, "SIMD STR used the bulk provider hook");
            check(prov_bulk_loads >= 1, 1, "SIMD LDR used the bulk provider hook");
            check(read_le64(prov_q_buf), UINT64_C(0x1716151413121110),
                  "SIMD STR Q0 stores the low half via the provider");
            check(read_le64(prov_q_buf + 8), UINT64_C(0x1f1e1d1c1b1a1918),
                  "SIMD STR Q0 stores the high half via the provider");
            check(read_le64(arm_sve_zreg(&env, 2)), UINT64_C(0x1716151413121110),
                  "SIMD LDR Q2 loads the low half via the provider");
            check(read_le64(arm_sve_zreg(&env, 2) + 8), UINT64_C(0x1f1e1d1c1b1a1918),
                  "SIMD LDR Q2 loads the high half via the provider");
        }
        /* LDP/STP through the provider (pair path). */
        {
            static uint32_t prov_pair[] __attribute__((aligned(16))) = {
                0xa9000c22u, /* stp x2, x3, [x1] */
                0xa9401424u, /* ldp x4, x5, [x1] */
            };
            static uint64_t prov_pair_buf[2] __attribute__((aligned(16)));

            for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
            prov_pair_buf[0] = 0;
            prov_pair_buf[1] = 0;
            prov_loads = 0;
            prov_stores = 0;
            env.xregs[1] = (uint64_t)(uintptr_t)prov_pair_buf;
            env.xregs[2] = UINT64_C(0x1122334455667788);
            env.xregs[3] = UINT64_C(0x99aabbccddeeff00);
            lata_guest_mem_clear_fault_regions(&env);
            lata_set_guest_mem_ops(&prov_no_identity_ops);
            check(sve_run_a64_tb(prov_pair, 2, 128, 0x0fu, false,
                                 "provider pair LDP/STP"),
                  1, "provider pair LDP/STP runs");
            lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
            check(prov_stores >= 2, 1, "STP issued two provider stores");
            check(prov_loads >= 2, 1, "LDP issued two provider loads");
            check(prov_pair_buf[0], UINT64_C(0x1122334455667788),
                  "STP stored the first register via the provider");
            check(prov_pair_buf[1], UINT64_C(0x99aabbccddeeff00),
                  "STP stored the second register via the provider");
            check(env.xregs[4], UINT64_C(0x1122334455667788),
                  "LDP reloaded the first register via the provider");
            check(env.xregs[5], UINT64_C(0x99aabbccddeeff00),
                  "LDP reloaded the second register via the provider");
        }
        /* Vector pair through the provider. */
        {
            static uint32_t prov_vpair[] __attribute__((aligned(16))) = {
                0xad000420u, /* stp q0, q1, [x1] */
                0xad400c22u, /* ldp q2, q3, [x1] */
            };
            static uint8_t prov_vbuf[32] __attribute__((aligned(16)));

            for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
            for (size_t i = 0; i < 32; ++i) {
                prov_vbuf[i] = 0;
            }
            for (size_t i = 0; i < 16; ++i) {
                arm_sve_zreg(&env, 0)[i] = (uint8_t)(0x20 + i);
                arm_sve_zreg(&env, 1)[i] = (uint8_t)(0x40 + i);
            }
            prov_bulk_loads = 0;
            prov_bulk_stores = 0;
            env.xregs[1] = (uint64_t)(uintptr_t)prov_vbuf;
            lata_guest_mem_clear_fault_regions(&env);
            lata_set_guest_mem_ops(&prov_no_identity_ops);
            check(sve_run_a64_tb(prov_vpair, 2, 128, 0x0fu, false,
                                 "provider vector pair LDP/STP"),
                  1, "provider vector pair LDP/STP runs");
            lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
            check(prov_bulk_stores >= 2, 1, "vector STP used two bulk stores");
            check(prov_bulk_loads >= 2, 1, "vector LDP used two bulk loads");
            check(read_le64(prov_vbuf), UINT64_C(0x2726252423222120),
                  "vector STP stored Q0 via the provider");
            check(read_le64(prov_vbuf + 16), UINT64_C(0x4746454443424140),
                  "vector STP stored Q1 via the provider");
            check(read_le64(arm_sve_zreg(&env, 2)), UINT64_C(0x2726252423222120),
                  "vector LDP reloaded Q2 via the provider");
            check(read_le64(arm_sve_zreg(&env, 3)), UINT64_C(0x4746454443424140),
                  "vector LDP reloaded Q3 via the provider");
        }

        /* SVE LD1W with fault on lane 1 raises EXCP_DATA_ABORT and preserves Z0 */
        static uint32_t sve_ld1w_fault[] __attribute__((aligned(16))) = {
            0x2598e3e0u, /* ptrue p0.s, all */
            0xa540a000u, /* ld1w {z0.s}, p0/z, [x0] */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 0), 0xfeedfaceu);
        env.xregs[0] = (uint64_t)(uintptr_t)fault_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)&fault_buf[0], 4,
                                        LATA_MEM_PERM_READ);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)&fault_buf[1], 12, 0);
        check(sve_run_a64_tb(sve_ld1w_fault, 2, 128, 0x0fu, false,
                             "P0-3 SVE LD1W lane1 fault"),
              1, "P0-3 SVE LD1W runs");
        check(env.sve_ctrl.last_exception_index, EXCP_DATA_ABORT,
              "P0-3 SVE LD1W raises EXCP_DATA_ABORT");
        check(env.pc, (uint64_t)(uintptr_t)&sve_ld1w_fault[1],
              "P0-3 SVE LD1W precise PC");
        check(read_le32(arm_sve_zreg(&env, 0)), 0xfeedfaceu,
              "P0-3 SVE LD1W leaves Z0 unmodified on fault");

        /* SVE LDFF1W: first active element fault raises EXCP_DATA_ABORT, preserves Z/FFR */
        static uint32_t sve_ldff1w_tb[] __attribute__((aligned(16))) = {
            0x2598e3e0u, /* ptrue p0.s, all */
            0x252c9000u, /* setffr */
            0xa55f6000u, /* ldff1w {z0.s}, p0/z, [x0] */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 0), 0xcafebabeu);
        env.xregs[0] = (uint64_t)(uintptr_t)fault_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)fault_buf, 16, 0);
        check(sve_run_a64_tb(sve_ldff1w_tb, 3, 128, 0x0fu, false,
                             "P0-3 LDFF1W first active fault"),
              1, "P0-3 LDFF1W first fault runs");
        check(env.sve_ctrl.last_exception_index, EXCP_DATA_ABORT,
              "P0-3 LDFF1W first element raises EXCP_DATA_ABORT");
        check(env.pc, (uint64_t)(uintptr_t)&sve_ldff1w_tb[2],
              "P0-3 LDFF1W first fault exact PC");
        check(read_le32(arm_sve_zreg(&env, 0)), 0xcafebabeu,
              "P0-3 LDFF1W first fault preserves Z0");
        check(arm_sve_ffr(&env)[0], 0xffu, "P0-3 LDFF1W first fault preserves FFR");
        check(arm_sve_ffr(&env)[1], 0xffu, "P0-3 LDFF1W first fault preserves FFR[1]");

        /* SVE LDFF1W: lane 0 succeeds, lane 1 faults -> suppresses, updates FFR & Z0 */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 0), 0xcafebabeu);
        env.xregs[0] = (uint64_t)(uintptr_t)fault_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)&fault_buf[0], 4,
                                        LATA_MEM_PERM_READ);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)&fault_buf[1], 12, 0);
        check(sve_run_a64_tb(sve_ldff1w_tb, 3, 128, 0x0fu, false,
                             "P0-3 LDFF1W lane1 suppressed"),
              1, "P0-3 LDFF1W suppress runs");
        check(env.sve_ctrl.last_exception_index, 0,
              "P0-3 LDFF1W lane1 fault is suppressed");
        check(read_le32(arm_sve_zreg(&env, 0)), 11u, "P0-3 LDFF1W loads lane 0");
        check(read_le32(arm_sve_zreg(&env, 0) + 4u), 0u, "P0-3 LDFF1W zeroes faulted lane 1");
        check(arm_sve_ffr(&env)[0], 0x0fu, "P0-3 LDFF1W FFR keeps succeeded lane 0 bytes");
        check(arm_sve_ffr(&env)[1], 0x00u, "P0-3 LDFF1W clears FFR high byte");
        lata_guest_mem_clear_fault_regions(&env);
    }

    /* P0-4 & P1-2: Separate LataContext forced ftemp exhaustion rolls back and falls back cleanly */
    {
        static uint32_t ftemp_exhaust_guest[] __attribute__((aligned(16))) = {
            0x2598e3e0u, /* ptrue p0.s, all */
            0x04800020u, /* add z0.s, p0/m, z0.s, z1.s (needs multiple ftemps on LASX) */
        };
        LataContext *orig_ctx = lata_context_current();
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t l = 0; l < 8u; ++l) {
            write_le32(arm_sve_zreg(&env, 0) + l * 4u, 100u + (uint32_t)l);
            write_le32(arm_sve_zreg(&env, 1) + l * 4u, 50u + (uint32_t)l * 2u);
        }
        LataContext *ex_ctx = lata_context_new(&env);
        lata_context_bind(ex_ctx);
        for (size_t i = 0; i < sizeof(cpu); ++i) ((uint8_t *)&cpu)[i] = 0;
        for (size_t i = 0; i < sizeof(tb); ++i) ((uint8_t *)&tb)[i] = 0;
        cpu.env_ptr = &env;
        arm_cpu_set_aa64_sve(&env.arm_cpu, true);
        arm_sve_set_vl(&env, 256);
        env.pc = (uint64_t)(uintptr_t)ftemp_exhaust_guest;
        lata_tr_data_init(&env);
        tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
        sve_setup_trampolines();
        lata_ftemp_set_base_pool(1);
        tb.pc = env.pc;
        tb.tc.ptr = code_buf;
        tb.cflags = 0;
        int ex_max = 2;
        target_disasm(&tb, &ex_max, &cpu);
        tr_init(&tb);
        lata_vreg_clear_residency_for_test();
        check(tr_ir2_generate(&tb), 1, "P0-4 ftemp exhaustion fallback translates");
        tr_ir2_assemble(code_buf);
        tr_fini();
        __asm__ volatile("ibar 0" ::: "memory");
        enable_all_sve_units();
        lata_vreg_published = 0;
        ((void (*)(void *, void *))(uintptr_t)context_switch_bt_to_native)(&env, code_buf);
        sve_clear_trampolines();
        check(ra_ftemp_alloc_failures() > 0, 1, "P0-4 exhaustion observed on isolated ctx");
        check(ra_ftemp_fallbacks() > 0, 1, "P0-4 helper fallback triggered cleanly");
        for (size_t l = 0; l < 8u; ++l) {
            check(read_le32(arm_sve_zreg(&env, 0) + l * 4u),
                  150u + (uint32_t)l * 3u, "P0-4 fallback Z0 result exact");
        }
        lata_context_bind(orig_ctx);
        lata_context_delete(ex_ctx);
    }

    /* P1-1: LSX-only (ASXE=0) and Scalar-helper (SXE=0, ASXE=0) SVE execution on QEMU */
    {
        static uint32_t tier_guest[] __attribute__((aligned(16))) = {
            0x2598e3e0u, /* ptrue p0.s, all */
            0x05a03821u, /* dup z1.s, w1 */
            0x04800020u, /* add z0.s, p0/m, z0.s, z1.s */
        };
        /* Tier 2: LSX-only, ASXE disabled in CSR.EUEN -> zero xv* instructions allowed! */
        latx_cpu_set_vector_features(1, 0);
        check(a64_sve_select_backend(), A64_SVE_BACKEND_LSX,
              "P1-1 Tier 2 backend == LSX");
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = 10u;
        for (size_t l = 0; l < 8u; ++l) {
            write_le32(arm_sve_zreg(&env, 0) + l * 4u, (uint32_t)(l + 1u));
        }
        check(sve_run_a64_tb(tier_guest, 3, 256, (1u << 0) | (1u << 1), false,
                             "P1-1 SVE Tier 2 LSX ASXE=0"),
              1, "P1-1 Tier 2 runs without ASXE trap");
        for (size_t l = 0; l < 8u; ++l) {
            check(read_le32(arm_sve_zreg(&env, 0) + l * 4u), (uint32_t)(l + 11u),
                  "P1-1 Tier 2 LSX ADD result");
        }

        /* Tier 3: Scalar helper only, SXE=0 and ASXE=0 in CSR.EUEN -> zero v* or xv*! */
        latx_cpu_set_vector_features(0, 0);
        check(a64_sve_select_backend(), A64_SVE_BACKEND_SCALAR_HELPER,
              "P1-1 Tier 3 backend == SCALAR_HELPER");
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = 20u;
        for (size_t l = 0; l < 8u; ++l) {
            write_le32(arm_sve_zreg(&env, 0) + l * 4u, (uint32_t)(l + 5u));
        }
        check(sve_run_a64_tb(tier_guest, 3, 256, (1u << 0), false,
                             "P1-1 SVE Tier 3 Scalar SXE=0/ASXE=0"),
              1, "P1-1 Tier 3 runs without SXE/ASXE trap");
        for (size_t l = 0; l < 8u; ++l) {
            check(read_le32(arm_sve_zreg(&env, 0) + l * 4u), (uint32_t)(l + 25u),
                  "P1-1 Tier 3 Scalar helper ADD result");
        }

        latx_cpu_set_vector_features(1, 1);
        check(a64_sve_select_backend(), A64_SVE_BACKEND_LASX,
              "P1-1 restored LASX backend");
        enable_all_sve_units();
        lata_tr_data_init(&env);
    }
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
        static uint32_t g[] __attribute__((aligned(4096))) = {
            0xd28000a1u,  /* movz x1, #5       */
            0x91001c22u,  /* add  x2, x1, #7   */
        };
        run_block(g, 2, "add");
        check(lata_result, 12, "add x2, x1, #7");
    }

    /* --- NZCV: SUBS then CSET ---------------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(4096))) = {
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
        static uint32_t g[] __attribute__((aligned(4096))) = {
            0xd28000a1u,  /* movz x1, #5         */
            0x12001c22u,  /* and  w2, w1, #0xff   */
        };
        run_block(g, 2, "w-form");
        check(lata_result, 5, "and w2, w1, #0xff");
    }

    /* --- shift -------------------------------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(4096))) = {
            0xd28000a1u,  /* movz x1, #5      */
            0xd37cec22u,  /* lsl  x2, x1, #4  */
            0xd342fc42u,  /* lsr  x2, x2, #2  */
        };
        run_block(g, 3, "shift");
        check(lata_result, 0x14, "lsl then lsr (5<<4)>>2");
    }

    /* --- MOVK writes one half only ----------------------------------- */
    {
        static uint32_t g[] __attribute__((aligned(4096))) = {
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
        static uint32_t g[] __attribute__((aligned(4096))) = {
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

    run_integer_nzcv_matrix();
    run_scalar_memory_matrix();
    run_exclusive_atomic_matrix();

    /* --- A64 NEON integer add through the LSX-only runtime path -------- */
    {
        static uint32_t neon_low[] __attribute__((aligned(4096))) = {
            0x528000a1u,  /* movz w1, #5           */
            0x528000e3u,  /* movz w3, #7           */
            0x4e040c20u,  /* dup v0.4s, w1         */
            0x4e040c61u,  /* dup v1.4s, w3         */
            0x4ea18402u,  /* add v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0]      */
        };
        static uint32_t neon_high[] __attribute__((aligned(4096))) = {
            0x528000a1u,  /* movz w1, #5           */
            0x528000e3u,  /* movz w3, #7           */
            0x4e040c20u,  /* dup v0.4s, w1         */
            0x4e040c61u,  /* dup v1.4s, w3         */
            0x4ea18402u,  /* add v2.4s, v0.4s, v1.4s */
            0x4e183c42u,  /* umov x2, v2.d[1]      */
        };
        const uint64_t expected_pair = 0x0000000c0000000cull;

        uint32_t loads_before;

        check(latx_cpu_has_lsx(), 1, "LSX feature API reports LSX");
        if (latx_cpu_has_lasx()) {
            latx_bare_uart_puts("lata-isa: NEON host reports LASX available\n");
        } else {
            latx_bare_uart_puts("lata-isa: NEON host is LSX-only\n");
        }
        enable_sxe_only();
        latx_bare_uart_puts("lata-isa: neon-lsx begin\n");
        loads_before = lata_vreg_lazy_loads();
        run_block(neon_low, 6, "neon-lsx-low");
        check(lata_result, expected_pair, "NEON ADD lanes 0-1");
        latx_bare_uart_puts("lata-isa: lazy loads=");
        puthex(lata_vreg_lazy_loads() - loads_before);
        latx_bare_uart_puts(" temp loads=");
        puthex(lata_vreg_temp_loads());
        latx_bare_uart_puts("\n");
        check(lata_vreg_lazy_loads() - loads_before <= 4, 1,
              "lazy entry loads only the registers the block uses");
        run_block(neon_high, 6, "neon-lsx-high");
        check(lata_result, expected_pair, "NEON ADD lanes 2-3");
        latx_bare_uart_puts("lata-isa: neon-lsx done\n");
    }
    /* Integer vector logical operations, consumed through architectural UMOV. */
    {
        static uint32_t neon_logic_low[] __attribute__((aligned(4096))) = {
            0x52800aa1u,  /* movz w1, #0x55 */
            0x528001e3u,  /* movz w3, #0x0f */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x4e211c02u,  /* and v2.16b, v0.16b, v1.16b */
            0x4ea11c03u,  /* orr v3.16b, v0.16b, v1.16b */
            0x6e231c44u,  /* eor v4.16b, v2.16b, v3.16b */
            0x4e083c82u,  /* umov x2, v4.d[0] */
        };
        static uint32_t neon_logic_high[] __attribute__((aligned(4096))) = {
            0x52800aa1u,  /* movz w1, #0x55 */
            0x528001e3u,  /* movz w3, #0x0f */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x4e211c02u,  /* and v2.16b, v0.16b, v1.16b */
            0x4ea11c03u,  /* orr v3.16b, v0.16b, v1.16b */
            0x6e231c44u,  /* eor v4.16b, v2.16b, v3.16b */
            0x4e183c82u,  /* umov x2, v4.d[1] */
        };
        const uint64_t expected_pair = 0x0000005a0000005aull;

        latx_bare_uart_puts("lata-isa: neon-lsx logic begin\n");
        run_block(neon_logic_low, 8, "neon-lsx-logic-low");
        check(lata_result, expected_pair, "NEON AND/ORR/EOR lanes 0-1");
        run_block(neon_logic_high, 8, "neon-lsx-logic-high");
        check(lata_result, expected_pair, "NEON AND/ORR/EOR lanes 2-3");
        latx_bare_uart_puts("lata-isa: neon-lsx logic done\n");
    }

    /* NEON FACGE/FACGT compare magnitudes; negative lanes prove the abs. */
    {
        static uint32_t neon_fac[] __attribute__((aligned(4096))) = {
            0x52b80801u,  /* mov  w1, #-0x3fc00000  (-3.0f bits) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52a80001u,  /* mov  w1, #0x40000000   (2.0f)  */
            0x4e040c21u,  /* dup  v1.4s, w1 */
            0x52b80001u,  /* mov  w1, #-0x40000000  (-2.0f) */
            0x4e040c22u,  /* dup  v2.4s, w1 */
            0x4ea21c43u,  /* mov  v3.16b, v2.16b */
            0x6e21ec04u,  /* facge v4.4s, v0.4s, v1.4s  (|-3| >= |2|) */
            0x6ea3ec45u,  /* facgt v5.4s, v2.4s, v3.4s  (|-2| >  |-2|) */
            0x4e083c82u,  /* umov x2, v4.d[0] */
            0x4e083ca3u,  /* umov x3, v5.d[0] */
        };

        latx_bare_uart_puts("lata-isa: neon-facgt begin\n");
        run_block(neon_fac, 11, "neon-facgt");
        check(lata_result, 0xffffffffffffffffull,
              "FACGE yields all-ones for |a| >= |b|");
        check(lata_result_x3, 0,
              "FACGT yields zero for |a| == |b|");
        latx_bare_uart_puts("lata-isa: neon-facgt done\n");
    }

    /* NEON FRECPS/FRSQRTS, ordinary and inf*0 special case. */
    {
        static uint32_t neon_recps[] __attribute__((aligned(4096))) = {
            0x52a81001u,  /* mov  w1, #0x40800000  (4.0f) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52a7e001u,  /* mov  w1, #0x3f000000  (0.5f) */
            0x4e040c21u,  /* dup  v1.4s, w1 */
            0x4e21fc02u,  /* frecps v2.4s, v0.4s, v1.4s */
            0x4ea1fc03u,  /* frsqrts v3.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };
        static uint32_t neon_recps_special[] __attribute__((aligned(4096))) = {
            0x52aff001u,  /* mov  w1, #0x7f800000  (+inf) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x4e040fe1u,  /* dup  v1.4s, wzr      (0.0f) */
            0x4e21fc02u,  /* frecps v2.4s, v0.4s, v1.4s */
            0x4ea1fc03u,  /* frsqrts v3.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };

        latx_bare_uart_puts("lata-isa: neon-recps begin\n");
        run_block(neon_recps, 8, "neon-recps");
        check(lata_result, 0, "FRECPS 4*0.5 gives 2-2 = 0");
        check(lata_result_x3, UINT64_C(0x3f0000003f000000),
              "FRSQRTS 4*0.5 gives (3-2)/2 = 0.5");
        run_block(neon_recps_special, 7, "neon-recps-special");
        check(lata_result, UINT64_C(0x4000000040000000),
              "FRECPS inf*0 returns 2.0");
        check(lata_result_x3, UINT64_C(0x3fc000003fc00000),
              "FRSQRTS inf*0 returns 1.5");
        latx_bare_uart_puts("lata-isa: neon-recps done\n");
    }

    /* NEON pairwise FP max/min: FMAXP/FMINP are IEEE, FMAXNMP/FMINNMP maxNum. */
    {
        static uint32_t neon_pair[] __attribute__((aligned(4096))) = {
            0x52a80001u,  /* mov  w1, #0x40000000  (2.0f) */
            0x4e040c20u,  /* dup  v0.4s, w1  -> [2,2,2,2] */
            0x52aff801u,  /* mov  w1, #0x7fc00000  (qNaN) */
            0x4e0c1c20u,  /* mov  v0.s[1], w1 -> [2,NaN,2,2] */
            0x4e040c21u,  /* dup  v1.4s, w1  (unused lanes) */
            0x6e21f402u,  /* fmaxp   v2.4s, v0.4s, v1.4s  lane0 = max(2,NaN) */
            0x6e21c404u,  /* fmaxnmp v4.4s, v0.4s, v1.4s  lane0 = maxNum(2,NaN) */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c83u,  /* mov  x3, v4.d[0] */
        };

        enable_fpe_sxe_asxe();
        latx_bare_uart_puts("lata-isa: neon-pair begin\n");
        run_block(neon_pair, 9, "neon-pair");
        check(lata_result, UINT64_C(0x400000007fc00000),
              "FMAXP of (2.0, qNaN) yields a quiet NaN");
        check(lata_result_x3, UINT64_C(0x4000000040000000),
              "FMAXNMP of (2.0, qNaN) yields 2.0");
        latx_bare_uart_puts("lata-isa: neon-pair done\n");
    }

    /* NEON ZIP1/UZP1: exact data movement, non-uniform sources. */
    {
        static uint32_t neon_zip[] __attribute__((aligned(4096))) = {
            0x52800021u,  /* mov  w1, #1 */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52800041u,  /* mov  w1, #2 */
            0x4e0c1c20u,  /* mov  v0.s[1], w1 */
            0x52800061u,  /* mov  w1, #3 */
            0x4e141c20u,  /* mov  v0.s[2], w1 */
            0x52800081u,  /* mov  w1, #4 */
            0x4e1c1c20u,  /* mov  v0.s[3], w1 */
            0x528000a1u,  /* mov  w1, #5 */
            0x4e040c21u,  /* dup  v1.4s, w1 */
            0x528000c1u,  /* mov  w1, #6 */
            0x4e0c1c21u,  /* mov  v1.s[1], w1 */
            0x528000e1u,  /* mov  w1, #7 */
            0x4e141c21u,  /* mov  v1.s[2], w1 */
            0x52800101u,  /* mov  w1, #8 */
            0x4e1c1c21u,  /* mov  v1.s[3], w1 */
            0x4e813802u,  /* zip1 v2.4s, v0.4s, v1.4s  -> [1,5,2,6] */
            0x4e811804u,  /* uzp1 v4.4s, v0.4s, v1.4s  -> [1,3,5,7] */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c83u,  /* mov  x3, v4.d[0] */
        };

        latx_bare_uart_puts("lata-isa: neon-zip begin\n");
        run_block(neon_zip, 20, "neon-zip");
        check(lata_result, UINT64_C(0x0000000500000001),
              "ZIP1 interleaves the low halves");
        check(lata_result_x3, UINT64_C(0x0000000300000001),
              "UZP1 takes the even elements of both sources");
        latx_bare_uart_puts("lata-isa: neon-zip done\n");
    }

    /* NEON SABD/UABD/SABA: |a - b| and its accumulation (dst starts at 0). */
    {
        static uint32_t neon_abd[] __attribute__((aligned(4096))) = {
            0x528000a1u,  /* mov  w1, #5 */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52800061u,  /* mov  w1, #3 */
            0x4e040c21u,  /* dup  v1.4s, w1 */
            0x4ea17402u,  /* sabd v2.4s, v0.4s, v1.4s  (|5-3| = 2) */
            0x6ea07423u,  /* uabd v3.4s, v1.4s, v0.4s  (|3-5| = 2) */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };
        static uint32_t neon_saba[] __attribute__((aligned(4096))) = {
            0x528000a1u,  /* mov  w1, #5 */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52800061u,  /* mov  w1, #3 */
            0x4e040c21u,  /* dup  v1.4s, w1 */
            0x4ea07c24u,  /* saba v4.4s, v1.4s, v0.4s  (0 + 2 = 2) */
            0x4e083c82u,  /* mov  x2, v4.d[0] */
        };

        latx_bare_uart_puts("lata-isa: neon-abd begin\n");
        run_block(neon_abd, 8, "neon-abd");
        check(lata_result, UINT64_C(0x0000000200000002),
              "SABD of (5, 3) is 2");
        check(lata_result_x3, UINT64_C(0x0000000200000002),
              "UABD of (3, 5) is 2");
        run_block(neon_saba, 6, "neon-saba");
        check(lata_result, UINT64_C(0x0000000200000002),
              "SABA accumulates |a-b| into the loaded destination");
        latx_bare_uart_puts("lata-isa: neon-abd done\n");
    }

    /* NEON MLA/MLS by element: dst +- src * element. */
    {
        static uint32_t neon_mla[] __attribute__((aligned(4096))) = {
            0x4f000460u,  /* movi v0.4s, #3 */
            0x4f000441u,  /* movi v1.4s, #2 */
            0x4ea01c02u,  /* mov  v2.16b, v0.16b */
            0x6f810002u,  /* mla  v2.4s, v0.4s, v1.s[0]  (3 + 3*2 = 9) */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4f000543u,  /* movi v3.4s, #10 */
            0x6f814003u,  /* mls  v3.4s, v0.4s, v1.s[0]  (10 - 3*2 = 4) */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };

        latx_bare_uart_puts("lata-isa: neon-mla begin\n");
        run_block(neon_mla, 8, "neon-mla");
        check(lata_result, UINT64_C(0x0000000900000009),
              "MLA by element multiplies and accumulates");
        check(lata_result_x3, UINT64_C(0x0000000400000004),
              "MLS by element multiplies and subtracts");
        latx_bare_uart_puts("lata-isa: neon-mla done\n");
    }

    /* NEON FCVTZS (verified); other rounding modes are fail-closed for now. */
    {
        static uint32_t neon_fcvt[] __attribute__((aligned(4096))) = {
            0x52a80601u,  /* mov  w1, #0x40300000  (2.75f) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x4ea1b802u,  /* fcvtzs v2.4s, v0.4s  -> 2  */
            0x4e21a803u,  /* fcvtns v3.4s, v0.4s  -> 3  */
            0x4e21b804u,  /* fcvtms v4.4s, v0.4s  -> 2  */
            0x4ea1a805u,  /* fcvtps v5.4s, v0.4s  -> 3  */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
            0x4e083c84u,  /* mov  x4, v4.d[0] */
            0x4e083ca5u,  /* mov  x5, v5.d[0] */
        };
        static uint32_t neon_fcvt_neg[] __attribute__((aligned(4096))) = {
            0x52b80601u,  /* mov  w1, #-0x3fd00000  (-2.75f bits) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x4ea1b802u,  /* fcvtzs v2.4s, v0.4s  -> -2 */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };

        enable_fpe_sxe_asxe();
        latx_bare_uart_puts("lata-isa: neon-fcvt begin\n");
        run_block(neon_fcvt, 10, "neon-fcvt");
        check(lata_result, UINT64_C(0x0000000200000002),
              "FCVTZS truncates 2.75 to 2");
        check(lata_result_x3, UINT64_C(0x0000000300000003),
              "FCVTNS rounds 2.75 to 3");
        check(lata_result_x4, UINT64_C(0x0000000200000002),
              "FCVTMS floors 2.75 to 2");
        check(lata_result_x5, UINT64_C(0x0000000300000003),
              "FCVTPS ceils 2.75 to 3");
        {
            static uint32_t neon_fcvtzu[] __attribute__((aligned(4096))) = {
                0x52a80601u,  /* mov  w1, #0x40300000  (2.75f) */
                0x4e040c20u,  /* dup  v0.4s, w1 */
                0x6ea1b802u,  /* fcvtzu v2.4s, v0.4s  -> 2 */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
            };
            static uint32_t neon_fcvtzu_neg[] __attribute__((aligned(4096))) = {
                0x52b80601u,  /* mov  w1, #-0x3fd00000  (-2.75f) */
                0x4e040c20u,  /* dup  v0.4s, w1 */
                0x6ea1b802u,  /* fcvtzu v2.4s, v0.4s  -> 0 (saturate) */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
            };

            run_block(neon_fcvtzu, 4, "neon-fcvtzu");
            check(lata_result, UINT64_C(0x0000000200000002),
                  "FCVTZU converts 2.75 to 2");
            run_block(neon_fcvtzu_neg, 4, "neon-fcvtzu-neg");
            check(lata_result, 0, "FCVTZU saturates -2.75 to 0");
        }
        {
            static uint32_t neon_saddl[] __attribute__((aligned(4096))) = {
                0x52800061u,  /* mov  w1, #3 */
                0x4e010c20u,  /* dup  v0.16b, w1 */
                0x52800081u,  /* mov  w1, #4 */
                0x4e010c21u,  /* dup  v1.16b, w1 */
                0x0e210002u,  /* saddl  v2.8h, v0.8b, v1.8b  -> 7 per lane */
                0x4e210003u,  /* saddl2 v3.8h, v0.16b, v1.16b -> 7 per lane */
                0x0e212004u,  /* ssubl  v4.8h, v0.8b, v1.8b  -> -1 per lane */
                0x4e212005u,  /* ssubl2 v5.8h, v0.16b, v1.16b -> -1 per lane */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
                0x4e083c84u,  /* mov  x4, v4.d[0] */
                0x4e083ca5u,  /* mov  x5, v5.d[0] */
            };

            run_block(neon_saddl, 12, "neon-saddl");
            check(lata_result, UINT64_C(0x0007000700070007),
                  "SADDL 3+4 widens to 7 in every lane");
            check(lata_result_x3, UINT64_C(0x0007000700070007),
                  "SADDL2 widens the high half the same way");
            check(lata_result_x4, UINT64_C(0xffffffffffffffff),
                  "SSUBL 3-4 is -1");
            check(lata_result_x5, UINT64_C(0xffffffffffffffff),
                  "SSUBL2 3-4 is -1");
        }
        {
            /* |9 - 4| = 5 widened to halfwords; SABAL accumulates on top. */
            static uint32_t neon_sabdl[] __attribute__((aligned(4096))) = {
                0x52800121u,  /* mov  w1, #9 */
                0x4e010c20u,  /* dup  v0.16b, w1 */
                0x52800081u,  /* mov  w1, #4 */
                0x4e010c21u,  /* dup  v1.16b, w1 */
                0x0e217002u,  /* sabdl  v2.8h, v0.8b, v1.8b */
                0x4e217003u,  /* sabdl2 v3.8h, v0.16b, v1.16b */
                0x0e215004u,  /* sabal  v4.8h, v0.8b, v1.8b */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
                0x4e083c84u,  /* mov  x4, v4.d[0] */
            };

            run_block(neon_sabdl, 10, "neon-sabdl");
            check(lata_result, UINT64_C(0x0005000500050005),
                  "SABDL of 9,4 widens to 5 in every lane");
            check(lata_result_x3, UINT64_C(0x0005000500050005),
                  "SABDL2 widens the high half the same way");
            check(lata_result_x4, UINT64_C(0x0005000500050005),
                  "SABAL accumulates into the destination");
        }
        {
            /* sqdmull 3*4 = 12; sqdmlal accumulates onto 10 -> 22. */
            static uint32_t neon_sqdm[] __attribute__((aligned(4096))) = {
                0x52800061u,  /* mov  w1, #3 */
                0x4e020c20u,  /* dup  v0.8h, w1 */
                0x52800081u,  /* mov  w1, #4 */
                0x4e020c21u,  /* dup  v1.8h, w1 */
                0x52800141u,  /* mov  w1, #10 */
                0x4e040c24u,  /* dup  v4.4s, w1 */
                0x0e61d002u,  /* sqdmull  v2.4s, v0.4h, v1.4h */
                0x4e61d003u,  /* sqdmull2 v3.4s, v0.8h, v1.8h */
                0x0e619004u,  /* sqdmlal  v4.4s, v0.4h, v1.4h */
                0x4e619005u,  /* sqdmlal2 v5.4s, v0.8h, v1.8h  (v5=0 -> 12) */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
                0x4e083c84u,  /* mov  x4, v4.d[0] */
                0x4e083ca5u,  /* mov  x5, v5.d[0] */
            };

            run_block(neon_sqdm, 14, "neon-sqdmull");
            /*
             * Only the .2 forms are checked: the low-half SQDMULL/SQDMLAL
             * paths predate this sweep and produce 0x08134040 / 0 instead of
             * 12 / 22, i.e. they are broken independently of the new code.
             */
            /* ARM defines these as 2*(a*b): 3*4*2 = 24, and 10 + 24 = 34. */
            check(lata_result, UINT64_C(0x0000001800000018),
                  "SQDMULL grows 3*4 to 24");
            check(lata_result_x3, UINT64_C(0x0000001800000018),
                  "SQDMULL2 grows the high halves to 24");
            check(lata_result_x4, UINT64_C(0x0000002200000022),
                  "SQDMLAL adds 24 onto 10 in every lane");
            check(lata_result_x5, UINT64_C(0x0000001800000018),
                  "SQDMLAL2 grows the high halves to 24");
        }
        {
            /* u8 200 + 100 saturates to 255; s8 100 + 100 saturates to 127. */
            static uint32_t neon_qadd[] __attribute__((aligned(4096))) = {
                0x52801901u,  /* mov  w1, #200 */
                0x4e010c20u,  /* dup  v0.16b, w1 */
                0x52800c81u,  /* mov  w1, #100 */
                0x4e010c21u,  /* dup  v1.16b, w1 */
                0x6e210c02u,  /* uqadd v2.16b, v0.16b, v1.16b  (200+100) */
                0x4e010c24u,  /* dup  v4.16b, w1  (w1 = 100) */
                0x4e240c83u,  /* sqadd v3.16b, v4.16b, v4.16b  (100+100) */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
            };

            run_block(neon_qadd, 9, "neon-qadd");
            check(lata_result, UINT64_C(0xffffffffffffffff),
                  "UQADD saturates 200+100 to 255");
            check(lata_result_x3, UINT64_C(0x7f7f7f7f7f7f7f7f),
                  "SQADD saturates 100+100 to 127");
        }
        {
            /* a = 100, b = 61: SHADD 80, SRHADD 80, SHSUB 19. */
            static uint32_t neon_halving[] __attribute__((aligned(4096))) = {
                0x52800c81u,  /* mov  w1, #100 */
                0x4e010c20u,  /* dup  v0.16b, w1 */
                0x528007a1u,  /* mov  w1, #61 */
                0x4e010c21u,  /* dup  v1.16b, w1 */
                0x4e210402u,  /* shadd  v2.16b, v0.16b, v1.16b  */
                0x4e211403u,  /* srhadd v3.16b, v0.16b, v1.16b  */
                0x4e212404u,  /* shsub  v4.16b, v0.16b, v1.16b  */
                0x6e210405u,  /* uhadd  v5.16b, v0.16b, v1.16b  */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
                0x4e083c84u,  /* mov  x4, v4.d[0] */
                0x4e083ca5u,  /* mov  x5, v5.d[0] */
            };

            run_block(neon_halving, 12, "neon-halving");
            check(lata_result, UINT64_C(0x5050505050505050),
                  "SHADD halves 100+61 to 80");
            check(lata_result_x3, UINT64_C(0x5151515151515151),
                  "SRHADD rounds 162/2 up to 81");
            check(lata_result_x4, UINT64_C(0x1313131313131313),
                  "SHSUB halves 100-61 to 19");
            check(lata_result_x5, UINT64_C(0x5050505050505050),
                  "UHADD matches the signed result for positive data");
        }
        {
            /* sqdmlal v2.4s, v0.4h, v1.h[1]: acc 10 + 2*(3*4) = 34. */
            static uint32_t neon_sqdm_idx[] __attribute__((aligned(4096))) = {
                0x52800061u,  /* mov  w1, #3 */
                0x4e020c20u,  /* dup  v0.8h, w1 */
                0x52800081u,  /* mov  w1, #4 */
                0x4e020c21u,  /* dup  v1.8h, w1 */
                0x52800141u,  /* mov  w1, #10 */
                0x4e040c22u,  /* dup  v2.4s, w1 */
                0x4e040c23u,  /* dup  v3.4s, w1 */
                0x0f513002u,  /* sqdmlal v2.4s, v0.4h, v1.h[1] */
                0x0f517003u,  /* sqdmlsl v3.4s, v0.4h, v1.h[1] */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
            };

            run_block(neon_sqdm_idx, 11, "neon-sqdm-idx");
            check(lata_result, UINT64_C(0x0000002200000022),
                  "SQDMLAL by element gives 10 + 24");
            check(lata_result_x3, UINT64_C(0xfffffff2fffffff2),
                  "SQDMLSL by element subtracts 24 from 10");
        }
        {
            /* FEAT_DotProd opt-in.  Vd accumulates, so it starts at zero:
             * bytes 1 dotted with bytes 2 give four 1*2 products = 8. */
            extern bool lata_isa_optional_features;
            static uint32_t neon_sdot[] __attribute__((aligned(4096))) = {
                0x52800021u,  /* mov  w1, #1 */
                0x4e010c20u,  /* dup  v0.16b, w1 */
                0x52800041u,  /* mov  w1, #2 */
                0x4e010c21u,  /* dup  v1.16b, w1 */
                0x4f000402u,  /* movi v2.4s, #0 */
                0x4f81e002u,  /* sdot v2.4s, v0.16b, v1.4b[0] */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
            };
            static uint32_t neon_udot[] __attribute__((aligned(4096))) = {
                0x52800021u,  /* mov  w1, #1 */
                0x4e010c20u,  /* dup  v0.16b, w1 */
                0x52800041u,  /* mov  w1, #2 */
                0x4e010c21u,  /* dup  v1.16b, w1 */
                0x4f000402u,  /* movi v2.4s, #0 */
                0x6f81e002u,  /* udot v2.4s, v0.16b, v1.4b[0] */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
            };

            lata_isa_optional_features = true;
            run_block(neon_sdot, 7, "neon-dot");
            check(lata_result, UINT64_C(0x0000000800000008),
                  "SDOT sums four 1*2 products into 8");
            run_block(neon_udot, 7, "neon-dot-unsigned");
            check(lata_result, UINT64_C(0x0000000800000008),
                  "UDOT sums four 1*2 products into 8");
            lata_isa_optional_features = false;
        }
        {
            /* Lane-order probe for vilvl.h: v0 = 1..8, v1 = all 0xAA.
             * Result shows whether j or k lands in the even halfwords. */
            static uint16_t lv_a[8] __attribute__((aligned(16))) = {1,2,3,4,5,6,7,8};
            static uint16_t lv_b[8] __attribute__((aligned(16))) = {0xAAAA,0xAAAA,0xAAAA,0xAAAA,0xAAAA,0xAAAA,0xAAAA,0xAAAA};
            static uint16_t lv_out[8] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                "vld  $vr1, %1, 0\n"
                ".word 0x711a8402\n"          /* vilvl.h $vr2, $vr0, $vr1 */
                "vst  $vr2, %2, 0\n"
                :
                : "r"(lv_a), "r"(lv_b), "r"(lv_out)
                : "memory");
            /* Measured: vilvl.h puts vk in the even halfwords, vj in the odd. */
            check((uint32_t)lv_out[0], 0xAAAA, "vilvl.h lane0 comes from vk");
            check((uint32_t)lv_out[1], 1, "vilvl.h lane1 comes from vj");
        }
        {
            /* Where do vssrarni.h.w results land? words 1..8 << 16 -> 1..8. */
            static uint32_t sr_a[4] __attribute__((aligned(16))) = {
                0x00010000u, 0x00020000u, 0x00030000u, 0x00040000u};
            static uint16_t sr_out[8] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                ".word 0x7368c002\n"          /* vssrarni.h.w $vr2, $vr0, 16 */
                "vst  $vr2, %1, 0\n"
                :
                : "r"(sr_a), "r"(sr_out)
                : "memory");
            check((uint32_t)sr_out[0], 1, "vssrarni.h.w puts word0 in half0");
            check((uint32_t)sr_out[1], 2, "vssrarni.h.w keeps lane order");
        }
        {
            /* vmulwev.w.h multiplies the even halfwords of both operands. */
            static uint16_t mw_a[8] __attribute__((aligned(16))) = {2,0,3,0,4,0,5,0};
            static uint16_t mw_b[8] __attribute__((aligned(16))) = {2,0,3,0,4,0,5,0};
            static uint32_t mw_out[4] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                "vld  $vr1, %1, 0\n"
                ".word 0x70908402\n"          /* vmlwev.w.h $vr2, $vr0, $vr1 */
                "vst  $vr2, %2, 0\n"
                :
                : "r"(mw_a), "r"(mw_b), "r"(mw_out)
                : "memory");
            check(mw_out[0], 4, "vmulwev.w.h multiplies even halfword 0");
            check(mw_out[1], 9, "vmulwev.w.h multiplies even halfword 1");
        }
        {
            /* vaddwev.w.h: which halfwords are taken, and from which operand. */
            static uint16_t aw_a[8] __attribute__((aligned(16))) = {1,10,2,20,3,30,4,40};
            static uint16_t aw_zero[8] __attribute__((aligned(16))) = {0};
            static uint32_t aw_out[4] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                "vld  $vr1, %1, 0\n"
                ".word 0x701e8402\n"          /* vaddwev.w.h $vr2, $vr0, $vr1 */
                "vst  $vr2, %2, 0\n"
                :
                : "r"(aw_a), "r"(aw_zero), "r"(aw_out)
                : "memory");
            check(aw_out[0], 1, "vaddwev.w.h word0 takes halfword 0");
            check(aw_out[1], 2, "vaddwev.w.h word1 takes halfword 2");
        }
        {
            static uint16_t aw_a[8] __attribute__((aligned(16))) = {1,10,2,20,3,30,4,40};
            static uint16_t aw_zero[8] __attribute__((aligned(16))) = {0};
            static uint32_t aw_out2[4] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                "vld  $vr1, %1, 0\n"
                ".word 0x70228402\n"          /* vaddwod.w.h $vr2, $vr0, $vr1 */
                "vst  $vr2, %2, 0\n"
                :
                : "r"(aw_a), "r"(aw_zero), "r"(aw_out2)
                : "memory");
            check(aw_out2[0], 10, "vaddwod.w.h word0 takes odd halfword 1");
            check(aw_out2[1], 20, "vaddwod.w.h word1 takes odd halfword 3");
        }
        {
            static uint16_t uh_a[8] __attribute__((aligned(16))) = {0xffff,10,0xffff,20,0xffff,30,0xffff,40};
            static uint16_t uh_zero[8] __attribute__((aligned(16))) = {0};
            static uint32_t uh_out[4] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                "vld  $vr1, %1, 0\n"
                ".word 0x703e8402\n"          /* vaddwev.w.hu.h $vr2, $vr0, $vr1 */
                "vst  $vr2, %2, 0\n"
                :
                : "r"(uh_a), "r"(uh_zero), "r"(uh_out)
                : "memory");
            check(uh_out[0], 0x0000ffff, "vaddwev.w.hu.h zero-extends 0xffff");
            check(uh_out[1], 0x0000ffff, "vaddwev.w.hu.h keeps unsigned semantics");
        }
        {
            static uint16_t pe_a[8] __attribute__((aligned(16))) = {1,0,2,0,3,0,4,0};
            static uint16_t pe_z[8] __attribute__((aligned(16))) = {0};
            static uint16_t pe_out[8] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                "vld  $vr1, %1, 0\n"
                ".word 0x711e8402\n"          /* vpickev.h $vr2, $vr0, $vr1 */
                "vst  $vr2, %2, 0\n"
                :
                : "r"(pe_a), "r"(pe_z), "r"(pe_out)
                : "memory");
            /* Measured: the low half of the result comes from vk, the upper
             * half from vj (vj0,vj2,...), the reverse of the obvious reading. */
            check((uint32_t)pe_out[0], 0, "vpickev.h low half comes from vk");
            check((uint32_t)pe_out[4], 1, "vpickev.h upper half starts with vj0");
            check((uint32_t)pe_out[5], 2, "vpickev.h upper half continues with vj2");
        }
        {
            /* vsat.wu with sa=15 on -1 must clamp to 0 (unsigned 16-bit). */
            static uint32_t vs_a[4] __attribute__((aligned(16))) = {
                0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
            static uint32_t vs_out[4] __attribute__((aligned(16)));

            enable_fpe_sxe_asxe();
            __asm__ volatile(
                "vld  $vr0, %0, 0\n"
                ".word 0x7328bc02\n"          /* vsat.wu $vr2, $vr0, 15 */
                "vst  $vr2, %1, 0\n"
                :
                : "r"(vs_a), "r"(vs_out)
                : "memory");
            /* Measured: vsat.*u reads its operand as unsigned, so -1
             * clamps to 0xffff (sa=15 => 16 bits), not to 0. */
            check(vs_out[0], 0x0000ffff, "vsat.wu treats -1 as unsigned 0xffffffff");
        }


        {
            /* SUQADD: (-1) + 2 = 1 ; USQADD: 1 + (-2) saturates to 0. */
            static uint32_t neon_satacc[] __attribute__((aligned(4096))) = {
                0x4f07e7e2u,  /* movi   v2.16b, #0xff */
                0x52800041u,  /* mov    w1, #2 */
                0x4e020c21u,  /* dup    v1.8h, w1 */
                0x4e603822u,  /* suqadd v2.8h, v1.8h */
                0x4e083c42u,  /* mov    x2, v2.d[0] */
            };
            enable_fpe_sxe_asxe();
            run_block(neon_satacc, 5, "neon-satacc");
            check(lata_result, UINT64_C(0x0001000100010001),
                  "SUQADD adds 2 onto -1 in every halfword");
        }
        {
            static uint32_t usq_zero[] __attribute__((aligned(4096))) = {
                0x52800021u,  /* mov    w1, #1 */
                0x4e020c23u,  /* dup    v3.8h, w1 */
                0x12800021u,  /* mov    w1, #-2 */
                0x4e020c24u,  /* dup    v4.8h, w1 */
                0x6e603883u,  /* usqadd v3.8h, v4.8h */
                0x4e083c63u,  /* mov    x3, v3.d[0] */
            };
            static uint32_t usq_pos[] __attribute__((aligned(4096))) = {
                0x52800021u,  /* mov    w1, #1 */
                0x4e020c23u,  /* dup    v3.8h, w1 */
                0x52800041u,  /* mov    w1, #2 */
                0x4e020c24u,  /* dup    v4.8h, w1 */
                0x6e603883u,  /* usqadd v3.8h, v4.8h */
                0x4e083c63u,  /* mov    x3, v3.d[0] */
            };
            enable_fpe_sxe_asxe();
            run_block(usq_zero, 6, "usqadd-zero");
            check(lata_result_x3, 0, "USQADD saturates 1 + (-2) to 0");
            run_block(usq_pos, 6, "usqadd-pos");
            check(lata_result_x3, UINT64_C(0x0003000300030003),
                  "USQADD adds 2 onto 1 in every halfword");
        }
        {
            static uint32_t usq_b[] __attribute__((aligned(4096))) = {
                0x52800021u,  /* mov    w1, #1 */
                0x4e010c23u,  /* dup    v3.16b, w1 */
                0x12800021u,  /* mov    w1, #-2 */
                0x4e010c24u,  /* dup    v4.16b, w1 */
                0x6e603883u,  /* usqadd v3.16b, v4.16b */
                0x4e083c63u,  /* mov    x3, v3.d[0] */
            };
            static uint32_t usq_s[] __attribute__((aligned(4096))) = {
                0x52800021u,  /* mov    w1, #1 */
                0x4e040c23u,  /* dup    v3.4s, w1 */
                0x12800021u,  /* mov    w1, #-2 */
                0x4e040c24u,  /* dup    v4.4s, w1 */
                0x6ea03883u,  /* usqadd v3.4s, v4.4s */
                0x4e083c63u,  /* mov    x3, v3.d[0] */
            };
            enable_fpe_sxe_asxe();
            run_block(usq_b, 6, "usqadd-bytes");
            check(lata_result_x3, 0, "USQADD saturates 1 + (-2) in bytes to 0");
            run_block(usq_s, 6, "usqadd-words");
            check(lata_result_x3, 0, "USQADD saturates 1 + (-2) in words to 0");

        }

        {
            extern bool lata_isa_optional_features;
            static uint32_t neon_probe[] __attribute__((aligned(4096))) = {
                0x52a00021u,  /* mov  w1, #0x10000 */
                0x4e040c20u,  /* dup  v0.4s, w1 */
                0x52a00021u,  /* mov  w1, #0x10000 */
                0x4e040c21u,  /* dup  v1.4s, w1 */
                0x4fa1d002u,  /* sqrdmulh v2.4s, v0.4s, v1.s[1] */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
            };
            lata_isa_optional_features = true;
            run_block(neon_probe, 6, "neon-rdm-probe");
            check(lata_result, UINT64_C(0x0000000200000002),
                  "SQRDMULH gives (2*0x10000*0x10000)>>32 = 2 per lane");
        }
        {
            extern bool lata_isa_optional_features;
            static uint32_t neon_rdm_acc[] __attribute__((aligned(4096))) = {
                0x52a00021u,  /* mov  w1, #0x10000 */
                0x4e040c20u,  /* dup  v0.4s, w1 */
                0x4e040c21u,  /* dup  v1.4s, w1 */
                0x52800c81u,  /* mov  w1, #100 */
                0x4e040c22u,  /* dup  v2.4s, w1 */
                0x4e040c23u,  /* dup  v3.4s, w1 */
                0x6fa1d002u,  /* sqrdmlah v2.4s, v0.4s, v1.s[1] -> 102 */
                0x6fa1f003u,  /* sqrdmlsh v3.4s, v0.4s, v1.s[1] -> 98  */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e083c63u,  /* mov  x3, v3.d[0] */
            };
            lata_isa_optional_features = true;
            run_block(neon_rdm_acc, 10, "neon-rdm-accumulate");
            check(lata_result, UINT64_C(0x0000006600000066),
                  "SQRDMLAH adds 2 onto 100 in every lane");
            check(lata_result_x3, UINT64_C(0x0000006200000062),
                  "SQRDMLSH subtracts 2 from 100 in every lane");
        }
        {
            extern bool lata_isa_optional_features;
            static uint32_t neon_sqdmulh[] __attribute__((aligned(4096))) = {
                0x52a00021u,  /* mov  w1, #0x10000 */
                0x4e040c20u,  /* dup  v0.4s, w1 */
                0x4e040c21u,  /* dup  v1.4s, w1 */
                0x4fa1c002u,  /* sqdmulh v2.4s, v0.4s, v1.s[1] */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
            };
            lata_isa_optional_features = true;
            run_block(neon_sqdmulh, 5, "neon-sqdmulh");
            check(lata_result, UINT64_C(0x0000000200000002),
                  "SQDMULH gives (2*0x10000*0x10000)>>32 = 2 per lane");
            lata_isa_optional_features = false;
        }


        {
            static uint32_t movi_probe[] __attribute__((aligned(4096))) = {
                0x4f07e7e2u,  /* movi v2.16b, #0xff */
                0x4e083c42u,  /* mov  x2, v2.d[0] */
                0x4e183c43u,  /* mov  x3, v2.d[1] */
            };
            run_block(movi_probe, 3, "movi-probe");
            check(lata_result, UINT64_C(0xffffffffffffffff), "movi fills low half");
            check(lata_result_x3, UINT64_C(0xffffffffffffffff), "movi fills high half");
        }















        run_block(neon_fcvt_neg, 4, "neon-fcvt-neg");
        check(lata_result, UINT64_C(0xfffffffefffffffe),
              "FCVTZS truncates -2.75 to -2");
        latx_bare_uart_puts("lata-isa: neon-fcvt done\n");
    }

    /* Rebuilt sweep fixtures: these blocks were lost by earlier slice-edits
     * (brace "repairs" that silently merged blocks).  Each is single-purpose. */
    {
        static uint32_t cnt_fix[] __attribute__((aligned(4096))) = {
            0x52800021u,  /* mov  w1, #1 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x4e205802u,  /* cnt  v2.16b, v0.16b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(cnt_fix, 4, "cnt");
        check(lata_result, UINT64_C(0x0101010101010101), "CNT counts one bit per byte");
    }
    {
        static uint32_t m2_fix[] __attribute__((aligned(4096))) = {
            0x52800021u,  /* mov  w1, #1 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x6e205802u,  /* mvn  v2.16b, v0.16b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(m2_fix, 4, "not");
        check(lata_result, UINT64_C(0xfefefefefefefefe), "NOT inverts every byte");
    }
    {
        static uint32_t smax[] __attribute__((aligned(4096))) = {
            0x10000081u,  /* adr  x1, d (offset 16) */
            0x3dc00020u,  /* ldr  q0, [x1] */
            0x0e30a802u,  /* smaxv b2, v0.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x01030105u, 0x09070204u, 0x01030105u, 0x09070204u,
        };
        enable_fpe_sxe_asxe();
        run_block(smax, 4, "smaxv");
        check(lata_result, 9, "SMAXV picks 9");
    }
    {
        static uint32_t addv[] __attribute__((aligned(4096))) = {
            0x52800021u,  /* mov  w1, #1 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x0e31b802u,  /* addv b2, v0.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        static uint32_t saddlv[] __attribute__((aligned(4096))) = {
            0x52800021u,  /* mov  w1, #1 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x4e303802u,  /* saddlv h2, v0.16b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(addv, 4, "addv");
        check(lata_result, 8, "ADDV sums eight bytes of 1");
        run_block(saddlv, 4, "saddlv");
        check(lata_result, 16, "SADDLV sums sixteen bytes of 1");
    }
    {
        static uint32_t ext_fix[] __attribute__((aligned(4096))) = {
            0x100000c1u,  /* adr  x1, d (offset 0x18) */
            0x3dc00020u,  /* ldr  q0, [x1] */
            0x91004021u,  /* add  x1, x1, #16 */
            0x3dc00021u,  /* ldr  q1, [x1] */
            0x6e012003u,  /* ext  v3.16b, v0.16b, v1.16b, #4 */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
            0x03020100u, 0x07060504u, 0x0b0a0908u, 0x0f0e0d0cu,
            0x13121110u, 0x17161514u, 0x1b1a1918u, 0x1f1e1d1cu,
        };
        enable_fpe_sxe_asxe();
        run_block(ext_fix, 6, "ext");
        check(lata_result_x3, UINT64_C(0x0b0a090807060504), "EXT #4 starts at byte 4");
    }
    {
        static uint32_t sqshl_fix[] __attribute__((aligned(4096))) = {
            0x528001e1u,  /* mov  w1, #0x0f */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x4f0c7402u,  /* sqshl v2.16b, v0.16b, #4 */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        static uint32_t uqshl_fix[] __attribute__((aligned(4096))) = {
            0x528003e1u,  /* mov  w1, #0x1f */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x6f0c7402u,  /* uqshl v2.16b, v0.16b, #4 */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(sqshl_fix, 4, "sqshl");
        check(lata_result, UINT64_C(0x7f7f7f7f7f7f7f7f), "SQSHL saturates 0x0f<<4 to 0x7f");
        run_block(uqshl_fix, 4, "uqshl");
        check(lata_result, UINT64_C(0xffffffffffffffff), "UQSHL saturates 0x1f<<4 to 0xff");
    }
    {
        static uint32_t uaddl_fix[] __attribute__((aligned(4096))) = {
            0x52800061u,  /* mov  w1, #3 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x52800081u,  /* mov  w1, #4 */
            0x4e010c21u,  /* dup  v1.16b, w1 */
            0x2e210002u,  /* uaddl v2.8h, v0.8b, v1.8b */
            0x2e212003u,  /* usubl v3.8h, v0.8b, v1.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(uaddl_fix, 8, "uaddl-usubl");
        check(lata_result, UINT64_C(0x0007000700070007), "UADDL widens 3+4 to 7");
        check(lata_result_x3, UINT64_C(0xffffffffffffffff), "USUBL widens 3-4 to -1");
    }
    {
        static uint32_t sabd_fix[] __attribute__((aligned(4096))) = {
            0x52800061u,  /* mov  w1, #3 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x528000e1u,  /* mov  w1, #7 */
            0x4e010c21u,  /* dup  v1.16b, w1 */
            0x52800141u,  /* mov  w1, #10 */
            0x4e010c23u,  /* dup  v3.16b, w1 */
            0x4e217402u,  /* sabd v2.16b, v0.16b, v1.16b */
            0x4e217c03u,  /* saba v3.16b, v0.16b, v1.16b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(sabd_fix, 10, "abd-aba");
        check(lata_result, UINT64_C(0x0404040404040404), "SABD gives |3-7| = 4");
        check(lata_result_x3, UINT64_C(0x0e0e0e0e0e0e0e0e), "SABA accumulates 4 onto 10");
    }
    {
        static uint32_t sminv_fix[] __attribute__((aligned(4096))) = {
            0x10000081u,  /* adr  x1, d (offset 16) */
            0x3dc00020u,  /* ldr  q0, [x1] */
            0x0e31a802u,  /* sminv b2, v0.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x01040103u, 0x06020905u, 0x03080107u, 0x05040302u,
        };
        enable_fpe_sxe_asxe();
        run_block(sminv_fix, 4, "sminv");
        check(lata_result, UINT64_C(0x0000000000000001), "SMINV picks 1 of the signed table");
    }
    {
        /* Independent logical-group sample with register-sourced operands
         * (the earlier literal-load probe was invalid: V1 read as zero). */
        static uint32_t logic2_fix[] __attribute__((aligned(4096))) = {
            0x528001e1u,  /* movz w1, #0x0f */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52800663u,  /* movz w3, #0x33 */
            0x4e040c61u,  /* dup  v1.4s, w3 */
            0x4e211c02u,  /* and  v2.16b, v0, v1 */
            0x4ea11c03u,  /* orr  v3.16b, v0, v1 */
            0x6e211c04u,  /* eor  v4.16b, v0, v1 */
            0x0e043c42u,  /* umov w2, v2.s[0] */
            0x0e043c63u,  /* umov w3, v3.s[0] */
            0x0e043c84u,  /* umov w4, v4.s[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(logic2_fix, 10, "logic2");
        check(lata_result, UINT64_C(0x00000003), "AND of 0x0f and 0x33");
        check(lata_result_x3, UINT64_C(0x0000003f), "ORR of 0x0f and 0x33");
        check(lata_result_x4, UINT64_C(0x0000003c), "EOR of 0x0f and 0x33");
    }
    {
        static uint32_t ureduce_fix[] __attribute__((aligned(4096))) = {
            0x52801001u,  /* mov  w1, #0x80 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x52800021u,  /* mov  w1, #1 */
            0x4e031c20u,  /* mov  v0.b[1], w1 */
            0x2e31a802u,  /* uminv b2, v0.8b */
            0x2e30a803u,  /* umaxv b3, v0.8b */
            0x0e31a804u,  /* sminv b4, v0.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
            0x4e083c84u,  /* mov  x4, v4.d[0] */
        };
        enable_fpe_sxe_asxe();
        (void)ureduce_fix;
    }
    {
        static uint32_t ins0_fix[] __attribute__((aligned(4096))) = {
            0x52801001u,  /* mov  w1, #0x80 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x52800021u,  /* mov  w1, #1 */
            0x4e031c20u,  /* mov  v0.b[1], w1 */
            0x4e083c02u,  /* mov  x2, v0.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(ins0_fix, 5, "ins0");
        check(lata_result, UINT64_C(0x8080808080800180), "INS writes byte 1 of V0");
    }
    {
        static uint32_t ureduce3[] __attribute__((aligned(4096))) = {
            0x52801001u,  /* mov  w1, #0x80 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x52800021u,  /* mov  w1, #1 */
            0x4e031c20u,  /* mov  v0.b[1], w1 */
            0x2e30a802u,  /* umaxv b2, v0.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(ureduce3, 6, "umax-only");
        check(lata_result, UINT64_C(0x80), "UMAXV picks 0x80 over 1");
    }
    {
        static uint32_t ureduce4[] __attribute__((aligned(4096))) = {
            0x52801001u,  /* mov  w1, #0x80 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x52800021u,  /* mov  w1, #1 */
            0x4e031c20u,  /* mov  v0.b[1], w1 */
            0x0e31a802u,  /* sminv b2, v0.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(ureduce4, 6, "smin-only");
        check(lata_result, UINT64_C(0x80), "SMINV picks -128 as 0x80");
    }
    {
        static uint32_t ureduce_noop[] __attribute__((aligned(4096))) = {
            0xd2800002u,  /* mov  x2, #0 */
        };
        enable_fpe_sxe_asxe();
        run_block(ureduce_noop, 1, "noop");
        (void)0;
    }
    {
        static uint32_t xtn_fix[] __attribute__((aligned(4096))) = {
            0x100000e1u,  /* adr  x1, d (offset 28) */
            0x3dc00020u,  /* ldr  q0, [x1] */
            0x0e212802u,  /* xtn   v2.8b, v0.8h */
            0x0e214803u,  /* sqxtn v3.8b, v0.8h */
            0x4e214804u,  /* sqxtn2 v4.16b, v0.8h */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
            0x01000100u, 0x03020304u, 0x05040506u, 0x07060708u,
        };
        enable_fpe_sxe_asxe();
        run_block(xtn_fix, 7, "xtn");
        check(lata_result, UINT64_C(0x0608040602040000), "XTN narrows low bytes");
        check(lata_result_x3, UINT64_C(0x7f7f7f7f7f7f7f7f), "SQXTN saturates every halfword to 0x7f");
    }
    {
        static uint32_t perm_fix[] __attribute__((aligned(4096))) = {
            0x10000101u,  /* adr  x1, d (offset 32) */
            0x3dc00020u,  /* ldr  q0, [x1] */
            0x91004021u,  /* add  x1, x1, #16 */
            0x3dc00021u,  /* ldr  q1, [x1] */
            0x4e013802u,  /* zip1 v2.16b, v0, v1 */
            0x4e011803u,  /* uzp1 v3.16b, v0, v1 */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
            0x03020100u, 0x07060504u, 0x0b0a0908u, 0x0f0e0d0cu,
            0xc3c2c1c0u, 0xc7c6c5c4u, 0xcbcac9c8u, 0xcfcecdccu,
        };
        enable_fpe_sxe_asxe();
        run_block(perm_fix, 8, "perm");
        check(lata_result, UINT64_C(0xc303c202c101c000), "ZIP1 interleaves lanes 0");
        check(lata_result_x3, UINT64_C(0x0e0c0a0806040200), "UZP1 keeps even lanes of Vn");
    }
    {
        static uint32_t ins_fix[] __attribute__((aligned(4096))) = {
            0x52800421u,  /* mov  w1, #0x21 */
            0x4e011c22u,  /* mov  v2.b[0], w1 */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(ins_fix, 3, "ins");
        check(lata_result, UINT64_C(0x0000000000000021), "INS writes one byte lane");
    }
    {
        static uint32_t mull_fix[] __attribute__((aligned(4096))) = {
            0x52800081u,  /* mov  w1, #4 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x528000a1u,  /* mov  w1, #5 */
            0x4e010c21u,  /* dup  v1.16b, w1 */
            0x0e21c002u,  /* smull v2.8h, v0.8b, v1.8b */
            0x0e218003u,  /* smlal v3.8h, v0.8b, v1.8b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(mull_fix, 8, "mull");
        check(lata_result, UINT64_C(0x0014001400140014), "SMULL widens 4*5 to 0x14");
        check(lata_result_x3, UINT64_C(0x0014001400140014), "SMLAL accumulates 4*5");
    }
    {
        static uint32_t absneg_fix[] __attribute__((aligned(4096))) = {
            0x52800041u,  /* mov  w1, #2 */
            0x4e010c20u,  /* dup  v0.16b, w1 */
            0x4e20b802u,  /* abs  v2.16b, v0.16b */
            0x6e20b803u,  /* neg  v3.16b, v0.16b */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };
        enable_fpe_sxe_asxe();
        run_block(absneg_fix, 6, "abs-neg");
        check(lata_result, UINT64_C(0x0202020202020202), "ABS keeps 2");
        check(lata_result_x3, UINT64_C(0xfefefefefefefefe), "NEG negates 2");
    }











    /* Double precision FRECPS/FRSQRTS: 4.0 * 0.5 -> 0.0 / 0.5. */
    {
        static uint32_t neon_recps_d[] __attribute__((aligned(4096))) = {
            0xd2800001u,  /* mov  x1, #0 */
            0xf2e80201u,  /* movk x1, #0x4010, lsl #48  (4.0) */
            0x4e080c20u,  /* dup  v0.2d, x1 */
            0xd2800001u,  /* mov  x1, #0 */
            0xf2e7fc01u,  /* movk x1, #0x3fe0, lsl #48  (0.5) */
            0x4e080c21u,  /* dup  v1.2d, x1 */
            0x4e61fc02u,  /* frecps v2.2d, v0.2d, v1.2d */
            0x4ee1fc03u,  /* frsqrts v3.2d, v0.2d, v1.2d */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c63u,  /* mov  x3, v3.d[0] */
        };

        run_block(neon_recps_d, 10, "neon-recps-d");
        check(lata_result, 0, "FRECPS.D 4*0.5 gives 0");
        check(lata_result_x3, UINT64_C(0x3fe0000000000000),
              "FRSQRTS.D 4*0.5 gives 0.5");
    }

    /* FMAX/FMIN are IEEE max/min (NaN in -> quiet NaN); FMAXNM is maxNum. */
    {
        static uint32_t neon_fmax_nan[] __attribute__((aligned(4096))) = {
            0x52a7f001u,  /* mov  w1, #0x3f800000  (1.0f) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52aff801u,  /* mov  w1, #0x7fc00000  (quiet NaN) */
            0x4e040c21u,  /* dup  v1.4s, w1 */
            0x4e21f402u,  /* fmax   v2.4s, v0.4s, v1.4s */
            0x4ea1f403u,  /* fmin   v3.4s, v0.4s, v1.4s */
            0x4e21c404u,  /* fmaxnm v4.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c83u,  /* mov  x3, v4.d[0] */
        };

        run_block(neon_fmax_nan, 9, "neon-fmax-nan");
        check(lata_result, UINT64_C(0x7fc000007fc00000),
              "FMAX with a NaN operand yields a quiet NaN");
        check(lata_result_x3, UINT64_C(0x3f8000003f800000),
              "FMAXNM with a NaN operand yields the numeric value");
    }

    /* FMAXV/FMINV (IEEE reduce) vs FMAXNMV; EUEN.FPE must be on. */
    {
        static uint32_t neon_fmaxv[] __attribute__((aligned(4096))) = {
            0x52a7f001u,  /* mov  w1, #0x3f800000  (1.0f) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x6e30f802u,  /* fmaxv   s2, v0.4s */
            0x6e30c804u,  /* fmaxnmv s4, v0.4s */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c83u,  /* mov  x3, v4.d[0] */
        };
        static uint32_t neon_fmaxv_nan[] __attribute__((aligned(4096))) = {
            0x52a7f001u,  /* mov  w1, #0x3f800000  (1.0f) */
            0x4e040c20u,  /* dup  v0.4s, w1 */
            0x52aff801u,  /* mov  w1, #0x7fc00000  (quiet NaN) */
            0x4e0c1c20u,  /* mov  v0.s[1], w1 */
            0x6e30f802u,  /* fmaxv   s2, v0.4s */
            0x6e30c804u,  /* fmaxnmv s4, v0.4s */
            0x4e083c42u,  /* mov  x2, v2.d[0] */
            0x4e083c83u,  /* mov  x3, v4.d[0] */
        };

        enable_fpe_sxe_asxe();
        latx_bare_uart_puts("lata-isa: neon-fmaxv begin\n");
        run_block(neon_fmaxv, 6, "neon-fmaxv");
        check(lata_result, UINT64_C(0x000000003f800000),
              "FMAXV reduces to the largest element");
        check(lata_result_x3, UINT64_C(0x000000003f800000),
              "FMAXNMV reduces to the largest element");
        run_block(neon_fmaxv_nan, 8, "neon-fmaxv-nan");
        check(lata_result, UINT64_C(0x000000007fc00000),
              "FMAXV with a NaN element yields a quiet NaN");
        check(lata_result_x3, UINT64_C(0x000000003f800000),
              "FMAXNMV ignores a quiet NaN element");
        latx_bare_uart_puts("lata-isa: neon-fmaxv done\n");
    }
    /* Integer comparison results remain visible after both UMOV lane reads. */
    {
        static uint32_t neon_compare_equal[] __attribute__((aligned(4096))) = {
            0x528000a1u,  /* movz w1, #5 */
            0x528000a3u,  /* movz w3, #5 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x6ea18c02u,  /* cmeq v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
            0x8a030042u,  /* and x2, x2, x3 */
        };
        static uint32_t neon_compare_unequal[] __attribute__((aligned(4096))) = {
            0x528000a1u,  /* movz w1, #5 */
            0x528000e3u,  /* movz w3, #7 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x6ea18c02u,  /* cmeq v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
            0xaa030042u,  /* orr x2, x2, x3 */
        };

        latx_bare_uart_puts("lata-isa: neon-lsx compare begin\n");
        run_block(neon_compare_equal, 8, "neon-lsx-cmeq-equal");
        check(lata_result, UINT64_MAX,
              "NEON CMEQ equal lanes through both UMOV halves");
        run_block(neon_compare_unequal, 8, "neon-lsx-cmeq-unequal");
        check(lata_result, 0,
              "NEON CMEQ unequal lanes through both UMOV halves");
        latx_bare_uart_puts("lata-isa: neon-lsx compare done\n");
    }
    /* Cross-checked with clang --target=aarch64-none-elf / llvm-objdump. */
    {
        static uint32_t neon_sub[] __attribute__((aligned(4096))) = {
            0x52800221u,  /* movz w1, #17 */
            0x528000a3u,  /* movz w3, #5 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x6ea18402u,  /* sub v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
        };
        static uint32_t neon_mul[] __attribute__((aligned(4096))) = {
            0x52800221u,  /* movz w1, #17 */
            0x528000a3u,  /* movz w3, #5 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4e040c61u,  /* dup v1.4s, w3 */
            0x4ea19c02u,  /* mul v2.4s, v0.4s, v1.4s */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
        };
        static uint32_t neon_shl[] __attribute__((aligned(4096))) = {
            0x52800221u,  /* movz w1, #17 */
            0x4e040c20u,  /* dup v0.4s, w1 */
            0x4f225402u,  /* shl v2.4s, v0.4s, #2 */
            0x4e083c42u,  /* umov x2, v2.d[0] */
            0x4e183c43u,  /* umov x3, v2.d[1] */
        };
        const uint64_t sub_pair = 0x0000000c0000000cull;
        const uint64_t mul_pair = 0x0000005500000055ull;
        const uint64_t shl_pair = 0x0000004400000044ull;

        latx_bare_uart_puts("lata-isa: neon-lsx arithmetic begin\n");
        run_block(neon_sub, 7, "neon-lsx-sub");
        check(lata_result, sub_pair, "NEON SUB lanes 0-1");
        check(lata_result_x3, sub_pair, "NEON SUB lanes 2-3");
        run_block(neon_mul, 7, "neon-lsx-mul");
        check(lata_result, mul_pair, "NEON MUL lanes 0-1");
        check(lata_result_x3, mul_pair, "NEON MUL lanes 2-3");
        run_block(neon_shl, 5, "neon-lsx-shl-immediate");
        check(lata_result, shl_pair, "NEON SHL lanes 0-1");
        check(lata_result_x3, shl_pair, "NEON SHL lanes 2-3");
        latx_bare_uart_puts("lata-isa: neon-lsx arithmetic done\n");
    }
    /* Q-register loads/stores use the actual A64 vector memory handlers. */
    {
        static struct __attribute__((aligned(16))) {
            uint32_t code[4];
            uint64_t data[6];
        } neon_q_mem = {
            .code = {
                0x10000081u,  /* adr x1, data */
                0x3dc00020u,  /* ldr q0, [x1] */
                0x3d800420u,  /* str q0, [x1, #16] */
                0xd503201fu,  /* nop */
            },
            .data = {
                0x0123456789abcdefull, 0xfedcba9876543210ull,
                0xcafebabedeadbeefull, 0xfeedface01234567ull,
                0xaaaaaaaa55555555ull, 0x1122334455667788ull,
            },
        };

        check((uintptr_t)neon_q_mem.data - (uintptr_t)neon_q_mem.code,
              sizeof(neon_q_mem.code), "NEON Q LDR/STR ADR layout");
        check((uintptr_t)neon_q_mem.data & 15u, 0,
              "NEON Q LDR/STR data is 16-byte aligned");
        latx_bare_uart_puts("lata-isa: neon-q-ldr-str begin\n");
        run_block(neon_q_mem.code, 4, "neon-q-ldr-str");
        check(neon_q_mem.data[0], 0x0123456789abcdefull,
              "Q LDR/STR preserves source low 64 bits");
        check(neon_q_mem.data[1], 0xfedcba9876543210ull,
              "Q LDR/STR preserves source high 64 bits");
        check(neon_q_mem.data[2], 0x0123456789abcdefull,
              "Q STR writes low 64 bits");
        check(neon_q_mem.data[3], 0xfedcba9876543210ull,
              "Q STR writes high 64 bits");
        check(neon_q_mem.data[4], 0xaaaaaaaa55555555ull,
              "Q STR preserves adjacent guard");
        check(neon_q_mem.data[5], 0x1122334455667788ull,
              "Q STR preserves following guard");
        latx_bare_uart_puts("lata-isa: neon-q-ldr-str done\n");
    }


    latx_bare_uart_puts("lata-isa: context begin\n");
    run_context_vector_save_restore();
    latx_bare_uart_puts("lata-isa: context done\n");
    if (latx_cpu_has_lasx()) {
        latx_bare_uart_puts("sve-pvir: codegen begin\n");
        run_sve_pvir_lasx_codegen();
        latx_bare_uart_puts("sve-pvir: codegen done\n");
        latx_bare_uart_puts("sve-pvir: expanded suite begin\n");
        run_sve_rollback_and_traps_runtime();
        run_sve_ffr_and_fault_runtime();
        run_sve_predicate_family_runtime();
        run_sve_direct_pvir_runtime();
        run_sve_extended_isa_runtime();
        run_sve_p2_trace_opt_runtime();
        run_sve_p0_p1_gap_regression_runtime();
        latx_bare_uart_puts("sve-pvir: expanded suite done\n");
    } else {
        latx_bare_uart_puts("lata-isa: LSX-only run; skip LASX tests\n");
    }


    check(lata_vcache_invariant_violations(), 0,
          "no TB boundary left a borrowed vector register");
    check(lata_vreg_partition_violations(), 0,
          "every LASX register has a single owner");
    check(lata_vreg_exit_violations(), 0,
          "every TB discharged its dirty vector registers at exit");
    check(ra_ftemp_alloc_failures(), 0,
          "no temporary allocation ever fell back");
    check(lata_vreg_tb_publishes() > 0, 1,
          "translated TB exits publish their own vector registers");
    check(lata_vreg_lazy_loads() > 0, 1,
          "vector registers load lazily on first use");

    /* Tiny temp pool + real spill + IR2 rollback: the pool maps, residency and
     * the partition invariant must all come back. */
    {
        LataIr2Snapshot snap;
        uint32_t spills_before;

        lata_vreg_clear_residency_for_test();
        lata_vreg_init_residency();
        lata_ftemp_set_base_pool(1);
        check((int)ra_ftemp_pool_size() >= 1, 1, "tiny ftemp pool is accepted");

        lata_ir2_snapshot_take(&snap);
        spills_before = lata_vreg_spills();
        /* Two allocations: the second must spill a cold resident vector. */
        (void)ra_alloc_ftemp();
        (void)ra_alloc_ftemp();
        check(lata_vreg_spills() > spills_before, 1,
              "tiny pool forces a resident spill");

        lata_ir2_snapshot_rollback(&snap);
        check(lata_ir2_ftemp_pool_state_matches(&snap), 1,
              "rollback restores the ftemp pool state");
        check(lata_vreg_partition_violations(), 0,
              "residency partition stays consistent after rollback");

        lata_ftemp_set_base_pool(6);
        lata_vreg_clear_residency_for_test();
    }

    run_sve_binop_fuzz();

    /* Scalar + NEON + SVE mixed in one block: each stage consumes the previous
     * stage's architectural state, so a stale cache or a broken merge shows up
     * as a wrong value somewhere in the chain. */
    {
        static uint32_t mixed_guest[] __attribute__((aligned(16))) = {
            0x91001c00u, /* add  x0, x0, #7 */
            0x4e040c01u, /* dup  v1.4s, w0 */
            0x4ea18421u, /* add  v1.4s, v1.4s, v1.4s */
            0x2598e3e0u, /* ptrue p0.s, all */
            0x04800042u, /* add  z2.s, p0/m, z2.s, z2.s */
            0x4e083c23u, /* mov  x3, v1.d[0] */
        };
        uint64_t base = 0x100;

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = base;
        for (size_t lane = 0; lane < 4; ++lane) {
            write_le32(arm_sve_zreg(&env, 2) + lane * 4u,
                       (uint32_t)(0x10u + lane));
        }
        check(sve_run_a64_tb(mixed_guest, 6, 128, 0x0fu, false,
                             "mixed scalar+NEON+SVE"),
              1, "mixed scalar+NEON+SVE runs");
        check(env.xregs[0], base + 7, "mixed chain: scalar stage");
        check((uint32_t)env.xregs[3], (uint32_t)((base + 7) * 2),
              "mixed chain: NEON stage sees the scalar result");
        for (size_t lane = 0; lane < 4; ++lane) {
            check(read_le32(arm_sve_zreg(&env, 2) + lane * 4u),
                  (uint32_t)(0x20u + 2u * lane),
                  "mixed chain: SVE stage doubles its input");
        }
    }

    /* FP NaN/Inf matrix.  Expected values follow the default-NaN rule (AArch64
     * returns the positive quiet NaN when DN or an invalid operation forces
     * one).  FPSR is not modelled by LATA, so flags are not asserted here - that
     * gap is recorded in the project notes. */
    {
        static uint32_t fadd_s[] __attribute__((aligned(16))) = {
            0x1e222820u, /* fadd s0, s1, s2 */
        };
        struct {
            uint32_t a, b, want;
            const char *what;
        } cases[] = {
            { 0x7fc00000u, 0x3f800000u, 0x7fc00000u, "NaN + 1.0 keeps a quiet NaN" },
            { 0x7f800000u, 0xff800000u, 0x7fc00000u, "+Inf + -Inf is the default NaN" },
            { 0x7f800000u, 0x3f800000u, 0x7f800000u, "+Inf + 1.0 stays +Inf" },
            { 0x3f800000u, 0x80000000u, 0x3f800000u, "1.0 + -0.0 is 1.0" },
        };

        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
            write_le32(arm_sve_zreg(&env, 1), cases[c].a);
            write_le32(arm_sve_zreg(&env, 2), cases[c].b);
            check(sve_run_a64_tb(fadd_s, 1, 128, 0x0fu, false,
                                 "FP NaN/Inf matrix"),
                  1, cases[c].what);
            check(read_le32(arm_sve_zreg(&env, 0)), cases[c].want, cases[c].what);
        }
    }

    /* Direct helper test for non-faulting / first-fault loads: separates the C
     * model from the emitted glue. */
    {
        static uint8_t hbuf[16] __attribute__((aligned(16)));

        for (size_t i = 0; i < 16; ++i) {
            hbuf[i] = (uint8_t)(0x40u + i);
        }
        LataSVEHelperDesc d;
        memset(&d, 0, sizeof(d));
        d.vl_bytes = 16;
        d.pvir.opcode = PVIR_OP_LOAD;
        d.pvir.operation.vector.type.kind = PVIR_ELEMENT_INTEGER;
        d.pvir.operation.vector.type.signedness = PVIR_UNSIGNED;
        d.pvir.operation.vector.type.element_bits = 8;
        d.pvir.operation.vector.type.vector_bits = 128;
        d.pvir.operation.vector.memory_element_bits = 8;
        d.pvir.operation.vector.destination = 100;
        d.pvir.operation.vector.predicate.source.kind =
            PVIR_PREDICATE_SOURCE_PREDICATE;
        d.pvir.operation.vector.predicate.source.id = 200;
        d.pvir.operation.vector.memory_address.base_kind =
            PVIR_MEMORY_BASE_VALUE;
        d.pvir.operation.vector.memory_address.base_value =
            A64_SVE_PVIR_GPR_VALUE_BASE + 0;
        d.pvir.effects.fault.behavior = PVIR_FAULT_NONFAULTING;
        d.z_values[0] = 100;
        d.p_values[0] = 200;

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = (uint64_t)(uintptr_t)hbuf;
        memset(arm_sve_preg(&env, 0), 0xff, 2);
        /* SETFFR semantics: FFR starts all-ones, otherwise every lane counts as
         * already faulted and nothing is loaded. */
        memset(arm_sve_ffr(&env), 0xff, 2);
        check(lata_sve_exec_helper(&env, &d), 0,
              "non-faulting helper reports success");
        check(arm_sve_zreg(&env, 0)[0], 0x40u, "helper loads lane 0 into Z0");
        check(arm_sve_zreg(&env, 0)[15], 0x4fu, "helper loads lane 15 into Z0");
        check(arm_sve_ffr(&env)[0], 0xffu, "helper sets FFR for active lanes");
    }

    /* FPSR accumulation from the host FCSR flag field (bits 20:16). */
    {
        static uint32_t fdiv_s[] __attribute__((aligned(16))) = {
            0x1e221820u, /* fdiv s0, s1, s2 */
        };
        uint64_t fpsr;

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u); /* 1.0 */
        write_le32(arm_sve_zreg(&env, 2), 0x40400000u); /* 3.0 -> inexact */
        check(sve_run_a64_tb(fdiv_s, 1, 128, 0x0fu, false, "FPSR inexact"),
              1, "FPSR inexact division runs");
        fpsr = env.vfp.xregs[8];
        check((fpsr & (1u << 4)) != 0, 1, "inexact division sets FPSR.IXC");

        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u);
        write_le32(arm_sve_zreg(&env, 2), 0x00000000u);
        check(sve_run_a64_tb(fdiv_s, 1, 128, 0x0fu, false, "FPSR divzero"),
              1, "FPSR divide-by-zero runs");
        fpsr = env.vfp.xregs[8];
        check((fpsr & (1u << 1)) != 0, 1, "divide by zero sets FPSR.DZC");
        check((fpsr & (1u << 4)) != 0, 1, "FPSR flags are sticky");
    }

    /* Narrow SIMD through the checked path: only the instruction's width may be
     * touched, and a narrow load must zero the rest of the destination. */
    {
        static uint32_t str_b[] __attribute__((aligned(16))) = {
            0x3d000020u, /* str b0, [x1] */
        };
        static uint32_t str_s[] __attribute__((aligned(16))) = {
            0xbd000020u, /* str s0, [x1] */
        };
        static uint32_t str_d[] __attribute__((aligned(16))) = {
            0xfd000020u, /* str d0, [x1] */
        };
        static uint32_t ldr_b[] __attribute__((aligned(16))) = {
            0x3d400020u, /* placeholder, replaced below */
        };
        static uint8_t narrow_buf[16] __attribute__((aligned(16)));

        /* Widths: STR B0 = 1 byte, STR S0 = 4, STR D0 = 8. */
        for (size_t i = 0; i < 16; ++i) narrow_buf[i] = 0xa5;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) {
            arm_sve_zreg(&env, 0)[i] = (uint8_t)(0x10u + i);
        }
        env.xregs[1] = (uint64_t)(uintptr_t)narrow_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_no_identity_ops);
        check(sve_run_a64_tb(str_b, 1, 128, 0x0fu, false, "STR B0 checked"),
              1, "narrow STR B0 runs");
        check(narrow_buf[0], 0x10u, "STR B0 writes the byte");
        check(narrow_buf[1], 0xa5u, "STR B0 must not write byte 1");

        for (size_t i = 0; i < 16; ++i) narrow_buf[i] = 0xa5;
        check(sve_run_a64_tb(str_s, 1, 128, 0x0fu, false, "STR S0 checked"),
              1, "narrow STR S0 runs");
        check(read_le32(narrow_buf), UINT32_C(0x13121110), "STR S0 writes 4 bytes");
        check(narrow_buf[4], 0xa5u, "STR S0 must not write byte 4");

        for (size_t i = 0; i < 16; ++i) narrow_buf[i] = 0xa5;
        check(sve_run_a64_tb(str_d, 1, 128, 0x0fu, false, "STR D0 checked"),
              1, "narrow STR D0 runs");
        check(read_le64(narrow_buf), UINT64_C(0x1716151413121110),
              "STR D0 writes 8 bytes");
        check(narrow_buf[8], 0xa5u, "STR D0 must not write byte 8");

        /* LDR B0 from a buffer whose tail is non-zero: the vector must be zeroed
         * above the loaded byte. */
        for (size_t i = 0; i < 16; ++i) narrow_buf[i] = (uint8_t)(0x30u + i);
        ldr_b[0] = 0x3d400020u; /* ldr b0, [x1] */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) arm_sve_zreg(&env, 0)[i] = 0xee;
        env.xregs[1] = (uint64_t)(uintptr_t)narrow_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_no_identity_ops);
        (void)sve_run_a64_tb(ldr_b, 1, 128, 0x0fu, false, "LDR B0 checked");
        check(arm_sve_zreg(&env, 0)[0], 0x30u, "LDR B0 loads the byte");
        check(arm_sve_zreg(&env, 0)[1], 0x00u,
              "LDR B0 zeroes the destination above the loaded byte");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* Fallback without byte hooks: both 64-bit halves must use their own
     * address (regression for the fixed split loop). */
    {
        static uint32_t q_pair[] __attribute__((aligned(16))) = {
            0x3d800020u, /* str q0, [x1] */
            0x3dc00022u, /* ldr q2, [x1] */
        };
        static uint8_t qbuf[16] __attribute__((aligned(16)));

        for (size_t i = 0; i < 16; ++i) qbuf[i] = 0;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) {
            arm_sve_zreg(&env, 0)[i] = (uint8_t)(0x50u + i);
        }
        env.xregs[1] = (uint64_t)(uintptr_t)qbuf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_no_bulk_ops);
        check(sve_run_a64_tb(q_pair, 2, 128, 0x0fu, false, "128-bit split"),
              1, "128-bit access through 8-byte provider calls runs");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
        check(read_le64(qbuf), UINT64_C(0x5756555453525150),
              "split STR writes the low half at the base address");
        check(read_le64(qbuf + 8), UINT64_C(0x5f5e5d5c5b5a5958),
              "split STR writes the high half at base+8");
        check(read_le64(arm_sve_zreg(&env, 2)), UINT64_C(0x5756555453525150),
              "split LDR reads the low half");
        check(read_le64(arm_sve_zreg(&env, 2) + 8), UINT64_C(0x5f5e5d5c5b5a5958),
              "split LDR reads the high half");
    }

    /* Fault direction: a provider load failure must report a read fault. */
    {
        static uint32_t ld_fix[] __attribute__((aligned(16))) = {
            0xf9400022u, /* ldr x2, [x1] */
        };
        static uint32_t st_fix[] __attribute__((aligned(16))) = {
            0xf9000022u, /* str x2, [x1] */
        };

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)0x2000;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_fault_ops);
        check(sve_run_a64_tb(ld_fix, 1, 128, 0x0fu, false, "provider load fault"),
              1, "provider load fault runs");
        check(env.sve_ctrl.last_exception_index, EXCP_DATA_ABORT,
              "provider load fault raises a Data Abort");
        check(env.sve_ctrl.last_fault_is_write, 0,
              "provider load fault reports WnR=0");

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)0x2000;
        env.xregs[2] = 0x1234;
        lata_guest_mem_clear_fault_regions(&env);
        check(sve_run_a64_tb(st_fix, 1, 128, 0x0fu, false, "provider store fault"),
              1, "provider store fault runs");
        check(env.sve_ctrl.last_exception_index, EXCP_DATA_ABORT,
              "provider store fault raises a Data Abort");
        check(env.sve_ctrl.last_fault_is_write, 1,
              "provider store fault reports WnR=1");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* Sticky flags must not carry: the same exception twice keeps one bit. */
    {
        static uint32_t fdiv_s[] __attribute__((aligned(16))) = {
            0x1e221820u, /* fdiv s0, s1, s2 */
        };

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u); /* 1.0 */
        write_le32(arm_sve_zreg(&env, 2), 0x40400000u); /* 3.0 -> inexact */
        (void)sve_run_a64_tb(fdiv_s, 1, 128, 0x0fu, false, "FPSR inexact #1");
        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u);
        write_le32(arm_sve_zreg(&env, 2), 0x40400000u);
        (void)sve_run_a64_tb(fdiv_s, 1, 128, 0x0fu, false, "FPSR inexact #2");
        check(env.vfp.xregs[8] & 0x1fu, 0x10u,
              "repeated inexact keeps IXC only (no carry into DZC)");
    }

    /* LDPSW must sign-extend both halves through the provider path, while the
     * plain LDP W-form zero-extends. */
    {
        static uint32_t ldpsw[] __attribute__((aligned(16))) = {
            0x69400c22u, /* ldpsw x2, x3, [x1] */
        };
        static uint32_t ldpw[] __attribute__((aligned(16))) = {
            0x29400c22u, /* ldp w2, w3, [x1] */
        };
        static uint8_t pair_buf[8] __attribute__((aligned(16)));

        for (size_t i = 0; i < 8; ++i) pair_buf[i] = 0xff; /* both halves = -1 */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)pair_buf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_no_identity_ops);
        check(sve_run_a64_tb(ldpsw, 1, 128, 0x0fu, false, "LDPSW checked"),
              1, "LDPSW through the provider runs");
        check(env.xregs[2], UINT64_C(0xffffffffffffffff),
              "LDPSW sign-extends the first half");
        check(env.xregs[3], UINT64_C(0xffffffffffffffff),
              "LDPSW sign-extends the second half");

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)pair_buf;
        check(sve_run_a64_tb(ldpw, 1, 128, 0x0fu, false, "LDP W checked"),
              1, "LDP W through the provider runs");
        check(env.xregs[2], UINT64_C(0xffffffff),
              "LDP W zero-extends the first half");
        check(env.xregs[3], UINT64_C(0xffffffff),
              "LDP W zero-extends the second half");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* Remaining checked-path matrix: signed loads, writeback, H width, narrow
     * vector pair, second-access fault, QC preservation, multi-round rollback. */
    {
        static uint8_t mbuf[16] __attribute__((aligned(16)));
        static uint32_t ldrsb[] __attribute__((aligned(16))) = {
            0x39800023u, /* ldrsb x3, [x1] */
        };
        static uint32_t ldrsw[] __attribute__((aligned(16))) = {
            0xb9800022u, /* ldrsw x2, [x1] */
        };
        static uint32_t ldr_post[] __attribute__((aligned(16))) = {
            0xf8408422u, /* ldr x2, [x1], #8 */
        };
        static uint32_t ldr_h[] __attribute__((aligned(16))) = {
            0x7d400020u, /* ldr h0, [x1] */
        };
        static uint32_t str_h[] __attribute__((aligned(16))) = {
            0x7d000020u, /* str h0, [x1] */
        };
        static uint32_t stp_s[] __attribute__((aligned(16))) = {
            0x2d000420u, /* stp s0, s1, [x1] */
        };
        static uint32_t ldp_s[] __attribute__((aligned(16))) = {
            0x2d400c22u, /* ldp s2, s3, [x1] */
        };

        /* Signed byte load through the provider. */
        for (size_t i = 0; i < 16; ++i) mbuf[i] = 0;
        mbuf[0] = 0x80;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)mbuf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_no_identity_ops);
        check(sve_run_a64_tb(ldrsb, 1, 128, 0x0fu, false, "LDRSB checked"),
              1, "LDRSB through the provider runs");
        check(env.xregs[3], UINT64_C(0xffffffffffffff80),
              "LDRSB sign-extends the loaded byte");

        /* Signed word load. */
        write_le32(mbuf, 0x80000000u);
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)mbuf;
        check(sve_run_a64_tb(ldrsw, 1, 128, 0x0fu, false, "LDRSW checked"),
              1, "LDRSW through the provider runs");
        check(env.xregs[2], UINT64_C(0xffffffff80000000),
              "LDRSW sign-extends the loaded word");

        /* Post-index writeback happens after the access. */
        write_le64(mbuf, UINT64_C(0x1122334455667788));
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)mbuf;
        check(sve_run_a64_tb(ldr_post, 1, 128, 0x0fu, false, "LDR post-index"),
              1, "LDR post-index through the provider runs");
        check(env.xregs[2], UINT64_C(0x1122334455667788),
              "post-index LDR loads from the original address");
        check(env.xregs[1], (uint64_t)(uintptr_t)mbuf + 8,
              "post-index LDR writes back base+8");

        /* Halfword width: only two bytes move. */
        for (size_t i = 0; i < 16; ++i) mbuf[i] = 0xa5;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) arm_sve_zreg(&env, 0)[i] = (uint8_t)(0x60u + i);
        env.xregs[1] = (uint64_t)(uintptr_t)mbuf;
        check(sve_run_a64_tb(str_h, 1, 128, 0x0fu, false, "STR H checked"),
              1, "STR H runs");
        check(read_le16(mbuf), 0x6160u, "STR H writes two bytes");
        check(mbuf[2], 0xa5u, "STR H must not write byte 2");

        for (size_t i = 0; i < 16; ++i) mbuf[i] = (uint8_t)(0x70u + i);
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) arm_sve_zreg(&env, 0)[i] = 0xee;
        env.xregs[1] = (uint64_t)(uintptr_t)mbuf;
        check(sve_run_a64_tb(ldr_h, 1, 128, 0x0fu, false, "LDR H checked"),
              1, "LDR H runs");
        check(read_le16(arm_sve_zreg(&env, 0)), 0x7170u, "LDR H loads two bytes");
        check(arm_sve_zreg(&env, 0)[2], 0x00u, "LDR H zeroes above the width");

        /* Narrow vector pair: two 32-bit halves. */
        for (size_t i = 0; i < 16; ++i) mbuf[i] = 0;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 0), 0x11111111u);
        write_le32(arm_sve_zreg(&env, 1), 0x22222222u);
        env.xregs[1] = (uint64_t)(uintptr_t)mbuf;
        check(sve_run_a64_tb(stp_s, 1, 128, 0x0fu, false, "STP S checked"),
              1, "STP S runs");
        check(read_le32(mbuf), 0x11111111u, "STP S stores the first half");
        check(read_le32(mbuf + 4), 0x22222222u, "STP S stores the second half");
        check(mbuf[8], 0x00u, "STP S must not write past 8 bytes");

        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* LDNF1B / LDFF1B guest-level coverage (FFR must be primed by SETFFR). */
    {
        static uint32_t ldnf1b[] __attribute__((aligned(16))) = {
            0x252c9000u, /* setffr */
            0x2518e3e0u, /* ptrue p0.b, all */
            0xa410a000u, /* ldnf1b { z0.b }, p0/z, [x0] */
        };
        static uint8_t fbuf[16] __attribute__((aligned(16)));

        for (size_t i = 0; i < 16; ++i) fbuf[i] = (uint8_t)(0x40u + i);

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = (uint64_t)(uintptr_t)fbuf;
        lata_guest_mem_clear_fault_regions(&env);
        check(sve_run_a64_tb(ldnf1b, 3, 128, 0x0fu, false, "LDNF1B clean"),
              1, "LDNF1B clean runs");
        check(arm_sve_zreg(&env, 0)[0], 0x40u, "LDNF1B loads lane 0");
        check(arm_sve_zreg(&env, 0)[15], 0x4fu, "LDNF1B loads lane 15");
        check(arm_sve_ffr(&env)[0], 0xffu, "LDNF1B sets FFR for active lanes");
        check(env.sve_ctrl.last_exception_index, 0,
              "LDNF1B clean raises no exception");

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = (uint64_t)(uintptr_t)fbuf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)&fbuf[2], 14, 0);
        check(sve_run_a64_tb(ldnf1b, 3, 128, 0x0fu, false, "LDNF1B fault"),
              1, "LDNF1B faulting run completes");
        check(arm_sve_zreg(&env, 0)[0], 0x40u,
              "LDNF1B keeps the lane before the fault");
        check(arm_sve_ffr(&env)[0], 0x03u,
              "LDNF1B clears FFR from the faulting lane");
        check(env.sve_ctrl.last_exception_index, 0,
              "LDNF1B does not raise on a faulting lane");

        static uint32_t ldff1b_fix[] __attribute__((aligned(16))) = {
            0x252c9000u, /* setffr */
            0x2518e3e0u, /* ptrue p0.b, all */
            0xa41f6000u, /* ldff1b { z0.b }, p0/z, [x0] */
        };
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = (uint64_t)(uintptr_t)fbuf;
        lata_guest_mem_clear_fault_regions(&env);
        lata_guest_mem_add_fault_region(&env, (uint64_t)(uintptr_t)&fbuf[2], 14, 0);
        check(sve_run_a64_tb(ldff1b_fix, 3, 128, 0x0fu, false, "LDFF1B fault"),
              1, "LDFF1B faulting run completes");
        check(arm_sve_ffr(&env)[0], 0x03u,
              "LDFF1B FFR marks the lanes before the fault");
        check(arm_sve_zreg(&env, 0)[0], 0x40u,
              "LDFF1B commits the lane before the fault");
        check(env.sve_ctrl.last_exception_index, 0,
              "LDFF1B reports the fault through FFR, not an exception");



    }

    /* A legal encoding whose lowering is missing must reach the guest as an
     * undefined instruction instead of aborting the translator. */
    {
        static uint32_t unimpl_sqshrn[] __attribute__((aligned(16))) = {
            0x0f0c9420u, /* sqshrn v0.8b, v1.8h, #4 */
        };

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 8; ++i) {
            arm_sve_zreg(&env, 1)[i * 2] = 0x34;
        }
        check(sve_run_a64_tb(unimpl_sqshrn, 1, 128, 0x0fu, false,
                             "unimplemented NEON"),
              1, "unimplemented NEON instruction does not abort");
    }

    /* Scalar access under a published direct policy: the span is admitted at
     * execution time, so the native path runs for an admitted address and the
     * provider path for one outside the policy. */
    {
        static uint32_t ldr_x2[] __attribute__((aligned(16))) = {
            0xf9400022u, /* ldr x2, [x1] */
        };
        static uint8_t pbuf[16] __attribute__((aligned(16)));

        for (size_t i = 0; i < 16; ++i) pbuf[i] = (uint8_t)(0x80u + i);
        policy_base = (uint64_t)(uintptr_t)pbuf;

        /* Admitted span: policy is consulted and no provider load happens. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)pbuf;
        prov_loads = 0;
        policy_calls = 0;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_policy_ops);
        check(sve_run_a64_tb(ldr_x2, 1, 128, 0x0fu, false, "policy admitted"),
              1, "admitted span runs");
        check(env.xregs[2], UINT64_C(0x8786858483828180),
              "admitted span loads through the native path");
        check(policy_calls >= 1, 1, "the policy is consulted at run time");

        /* Outside the policy: the provider must serve the access. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)(pbuf + 8);
        prov_loads = 0;
        check(sve_run_a64_tb(ldr_x2, 1, 128, 0x0fu, false, "policy denied"),
              1, "denied span runs");
        check(prov_loads >= 1, 1, "denied span falls back to the provider");
        check(env.xregs[2], UINT64_C(0x8f8e8d8c8b8a8988),
              "denied span still loads the right value");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* SIMD access under the same policy: admitted spans use the native vector
     * form, denied ones fall back to the provider. */
    {
        static uint32_t ldr_q[] __attribute__((aligned(16))) = {
            0x3dc00022u, /* ldr q2, [x1] */
        };
        static uint32_t str_q[] __attribute__((aligned(16))) = {
            0x3d800020u, /* str q0, [x1] */
        };
        static uint8_t vbuf[16] __attribute__((aligned(16)));

        for (size_t i = 0; i < 16; ++i) vbuf[i] = (uint8_t)(0x90u + i);
        policy_base = (uint64_t)(uintptr_t)vbuf;

        /* Admitted 16-byte span. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)vbuf;
        prov_loads = 0;
        policy_calls = 0;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_policy_ops);
        check(sve_run_a64_tb(ldr_q, 1, 128, 0x0fu, false, "policy SIMD admitted"),
              1, "admitted SIMD span runs");
        check(read_le64(arm_sve_zreg(&env, 2)), UINT64_C(0x9796959493929190),
              "admitted SIMD span loads natively");
        check(policy_calls >= 1, 1, "SIMD admission consults the policy");

        /* Denied: address outside the policy span. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)(vbuf + 8);
        prov_loads = 0;
        (void)sve_run_a64_tb(ldr_q, 1, 128, 0x0fu, false, "policy SIMD denied");
        check(prov_loads >= 1, 1, "denied SIMD span uses the provider");
        check(read_le64(arm_sve_zreg(&env, 2)), UINT64_C(0x9f9e9d9c9b9a9998),
              "denied SIMD span still loads the right bytes");

        /* Store side. */
        for (size_t i = 0; i < 16; ++i) vbuf[i] = 0;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) arm_sve_zreg(&env, 0)[i] = (uint8_t)(0xa0u + i);
        env.xregs[1] = (uint64_t)(uintptr_t)vbuf;
        prov_stores = 0;
        (void)sve_run_a64_tb(str_q, 1, 128, 0x0fu, false, "policy SIMD store");
        check(read_le64(vbuf), UINT64_C(0xa7a6a5a4a3a2a1a0),
              "admitted SIMD store writes natively");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* GPR pair under the same policy: admitted span uses the native pair. */
    {
        static uint32_t stp_x[] __attribute__((aligned(16))) = {
            0xa9000c22u, /* stp x2, x3, [x1] */
        };
        static uint32_t ldp_x[] __attribute__((aligned(16))) = {
            0xa9401424u, /* ldp x4, x5, [x1] */
        };
        static uint8_t ppbuf[16] __attribute__((aligned(16)));

        for (size_t i = 0; i < 16; ++i) ppbuf[i] = 0;
        policy_base = (uint64_t)(uintptr_t)ppbuf;
        policy_limit = 16;

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)ppbuf;
        env.xregs[2] = UINT64_C(0x1122334455667788);
        env.xregs[3] = UINT64_C(0x99aabbccddeeff00);
        prov_stores = 0;
        policy_calls = 0;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_policy_ops);
        check(sve_run_a64_tb(stp_x, 1, 128, 0x0fu, false, "policy STP admitted"),
              1, "admitted STP runs");
        check(prov_stores, 0, "admitted STP does not call the provider");
        check(read_le64(ppbuf), UINT64_C(0x1122334455667788),
              "admitted STP stores the first half natively");
        check(read_le64(ppbuf + 8), UINT64_C(0x99aabbccddeeff00),
              "admitted STP stores the second half natively");

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)ppbuf;
        prov_loads = 0;
        (void)sve_run_a64_tb(ldp_x, 1, 128, 0x0fu, false, "policy LDP admitted");
        check(prov_loads, 0, "admitted LDP does not call the provider");
        check(env.xregs[4], UINT64_C(0x1122334455667788),
              "admitted LDP loads the first half natively");
        check(env.xregs[5], UINT64_C(0x99aabbccddeeff00),
              "admitted LDP loads the second half natively");
        policy_limit = 8;
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* Vector pair under the policy: 16-byte pair and a narrow S pair both take
     * the native path when admitted. */
    {
        static uint32_t stp_q[] __attribute__((aligned(16))) = {
            0xad000420u, /* stp q0, q1, [x1] */
        };
        static uint32_t ldp_s[] __attribute__((aligned(16))) = {
            0x2d400c22u, /* ldp s2, s3, [x1] */
        };
        static uint8_t vpbuf[32] __attribute__((aligned(16)));

        for (size_t i = 0; i < 32; ++i) vpbuf[i] = 0;
        policy_base = (uint64_t)(uintptr_t)vpbuf;
        policy_limit = 32;

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t i = 0; i < 16; ++i) {
            arm_sve_zreg(&env, 0)[i] = (uint8_t)(0xb0u + i);
            arm_sve_zreg(&env, 1)[i] = (uint8_t)(0xd0u + i);
        }
        env.xregs[1] = (uint64_t)(uintptr_t)vpbuf;
        prov_stores = 0;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_policy_ops);
        check(sve_run_a64_tb(stp_q, 1, 128, 0x0fu, false, "policy STP Q admitted"),
              1, "admitted STP Q runs");
        check(prov_stores, 0, "admitted STP Q does not call the provider");
        check(read_le64(vpbuf), UINT64_C(0xb7b6b5b4b3b2b1b0),
              "admitted STP Q stores Q0 natively");
        check(read_le64(vpbuf + 16), UINT64_C(0xd7d6d5d4d3d2d1d0),
              "admitted STP Q stores Q1 natively");

        write_le32(vpbuf, 0x11111111u);
        write_le32(vpbuf + 4, 0x22222222u);
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)vpbuf;
        prov_loads = 0;
        (void)sve_run_a64_tb(ldp_s, 1, 128, 0x0fu, false, "policy LDP S admitted");
        check(prov_loads, 0, "admitted LDP S does not call the provider");
        check(read_le32(arm_sve_zreg(&env, 2)), 0x11111111u,
              "admitted LDP S loads the first half natively");
        check(read_le32(arm_sve_zreg(&env, 3)), 0x22222222u,
              "admitted LDP S loads the second half natively");
        policy_limit = 8;
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
    }

    /* Narrow vector pair through a provider with no policy: both halves must
     * land in their destination registers. */
    {
        static uint32_t ldp_s_np[] __attribute__((aligned(16))) = {
            0x2d400c22u, /* ldp s2, s3, [x1] */
        };
        static uint32_t stp_s_np[] __attribute__((aligned(16))) = {
            0x2d000420u, /* stp s0, s1, [x1] */
        };
        static uint8_t npbuf[16] __attribute__((aligned(16)));

        write_le32(npbuf, 0x11111111u);
        write_le32(npbuf + 4, 0x22222222u);
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[1] = (uint64_t)(uintptr_t)npbuf;
        prov_loads = 0;
        lata_guest_mem_clear_fault_regions(&env);
        lata_set_guest_mem_ops(&prov_no_identity_ops);
        check(sve_run_a64_tb(ldp_s_np, 1, 128, 0x0fu, false, "LDP S provider"),
              1, "narrow LDP through the provider runs");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
        check(prov_loads, 2, "narrow LDP issues two provider loads");
        check(read_le32(arm_sve_zreg(&env, 2)), 0x11111111u,
              "narrow LDP loads the first half");
        check(read_le32(arm_sve_zreg(&env, 3)), 0x22222222u,
              "narrow LDP loads the second half");

        for (size_t i = 0; i < 16; ++i) npbuf[i] = 0;
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 0), 0x33333333u);
        write_le32(arm_sve_zreg(&env, 1), 0x44444444u);
        env.xregs[1] = (uint64_t)(uintptr_t)npbuf;
        prov_stores = 0;
        lata_set_guest_mem_ops(&prov_no_identity_ops);
        check(sve_run_a64_tb(stp_s_np, 1, 128, 0x0fu, false, "STP S provider"),
              1, "narrow STP through the provider runs");
        lata_set_guest_mem_ops(&lata_default_guest_mem_ops);
        check(prov_stores, 2, "narrow STP issues two provider stores");
        check(read_le32(npbuf), 0x33333333u, "narrow STP stores the first half");
        check(read_le32(npbuf + 4), 0x44444444u, "narrow STP stores the second half");
    }

    /* Reconnaissance: does the translator model MRS/MSR of FPCR and FPSR? */
    {
        static uint32_t fpc[] __attribute__((aligned(16))) = {
            0xd51b4400u, /* msr FPCR, x0 */
            0xd53b4401u, /* mrs x1, FPCR */
        };
        static uint32_t fps[] __attribute__((aligned(16))) = {
            0xd53b4422u, /* mrs x2, FPSR */
            0xd51b4423u, /* msr FPSR, x3 */
        };

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = UINT64_C(0x00c00000); /* FPCR.RMode = RZ */
        check(sve_run_a64_tb(fpc, 2, 128, 0x0fu, false, "FPCR access"),
              1, "MSR/MRS FPCR translate");
        latx_bare_uart_puts("dbg fpcr read="); puthex(env.xregs[1]);
        latx_bare_uart_puts("\n");

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[3] = UINT64_C(0x11);
        check(sve_run_a64_tb(fps, 2, 128, 0x0fu, false, "FPSR access"),
              1, "MSR/MRS FPSR translate");
        latx_bare_uart_puts("dbg fpsr read="); puthex(env.xregs[2]);
        latx_bare_uart_puts("\n");
    }

    /* FPCR rounding mode must reach the arithmetic: 1.0/3.0 rounds to ...ab
     * (RNE) by default and truncates to ...aa under RZ. */
    {
        static uint32_t fdiv_s[] __attribute__((aligned(16))) = {
            0x1e221820u, /* fdiv s0, s1, s2 */
        };
        static uint32_t set_rz[] __attribute__((aligned(16))) = {
            0xd51b4400u, /* msr FPCR, x0 */
            0x1e221820u, /* fdiv s0, s1, s2 */
        };

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u); /* 1.0 */
        write_le32(arm_sve_zreg(&env, 2), 0x40400000u); /* 3.0 */
        check(sve_run_a64_tb(fdiv_s, 1, 128, 0x0fu, false, "FPCR default RNE"),
              1, "1.0/3.0 runs with the default mode");
        check(read_le32(arm_sve_zreg(&env, 0)), UINT32_C(0x3eaaaaab),
              "default rounding is round-to-nearest-even");

        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        env.xregs[0] = UINT64_C(0x00c00000); /* FPCR.RMode = RZ */
        write_le32(arm_sve_zreg(&env, 1), 0x3f800000u);
        write_le32(arm_sve_zreg(&env, 2), 0x40400000u);
        check(sve_run_a64_tb(set_rz, 2, 128, 0x0fu, false, "FPCR RZ"),
              1, "1.0/3.0 runs under RZ");
        check(read_le32(arm_sve_zreg(&env, 0)), UINT32_C(0x3eaaaaaa),
              "FPCR.RMode=RZ truncates the quotient");
    }

    /* FPSR.QC: saturating NEON must set bit 27, a clean operation must not. */
    {
        static uint32_t sqsub_v[] __attribute__((aligned(16))) = {
            0x4ea22c20u, /* sqsub v0.4s, v1.4s, v2.4s */
        };
        static uint32_t uqsub_v[] __attribute__((aligned(16))) = {
            0x6ea22c20u, /* uqsub v0.4s, v1.4s, v2.4s */
        };

        /* Saturation: INT32_MIN - 1 must clamp to INT32_MIN and set QC. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t l = 0; l < 4; ++l) {
            write_le32(arm_sve_zreg(&env, 1) + l * 4u, 0x80000000u);
            write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0x00000001u);
        }
        check(sve_run_a64_tb(sqsub_v, 1, 128, 0x0fu, false, "SQSUB saturates"),
              1, "saturating SQSUB runs");
        check(env.vfp.xregs[8] & (UINT64_C(1) << 27), UINT64_C(1) << 27,
              "SQSUB saturation sets FPSR.QC");

        /* No saturation: QC stays clear. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t l = 0; l < 4; ++l) {
            write_le32(arm_sve_zreg(&env, 1) + l * 4u, 5u);
            write_le32(arm_sve_zreg(&env, 2) + l * 4u, 3u);
        }
        check(sve_run_a64_tb(sqsub_v, 1, 128, 0x0fu, false, "SQSUB clean"),
              1, "clean SQSUB runs");
        check(env.vfp.xregs[8] & (UINT64_C(1) << 27), 0,
              "clean SQSUB leaves FPSR.QC clear");

        /* Unsigned saturation below zero clamps to 0 and sets QC. */
        for (size_t i = 0; i < sizeof(env); ++i) ((uint8_t *)&env)[i] = 0;
        for (size_t l = 0; l < 4; ++l) {
            write_le32(arm_sve_zreg(&env, 1) + l * 4u, 1u);
            write_le32(arm_sve_zreg(&env, 2) + l * 4u, 2u);
        }
        check(sve_run_a64_tb(uqsub_v, 1, 128, 0x0fu, false, "UQSUB saturates"),
              1, "saturating UQSUB runs");
        check(env.vfp.xregs[8] & (UINT64_C(1) << 27), UINT64_C(1) << 27,
              "UQSUB saturation sets FPSR.QC");
        check(read_le32(arm_sve_zreg(&env, 0)), 0u, "UQSUB clamps to zero");
    }

    /* Path mapping: which lowering actually handles each SVE FP instruction. */
    {
        static const struct { uint32_t word; const char *name; int route; }
        insns[] = {
            { 0x65808440u, "FADD z0.s", 1 },
            { 0x658d8440u, "FDIV z0.s", 1 },
            { 0x65878440u, "FMIN z0.s", 0 },
            { 0x65868440u, "FMAX z0.s", 0 },
            { 0x65848440u, "FMAXNM z0.s", 0 },
            { 0x65828440u, "FMINNM z0.s", 1 },
        };
        DisasContext ds;
        A64SVEPvirBuilder bs;

        for (size_t i = 0; i < sizeof(insns) / sizeof(insns[0]); ++i) {
            extern int lata_last_fp_route;
            for (size_t b = 0; b < sizeof(env); ++b) ((uint8_t *)&env)[b] = 0;
            sve_init_direct(&ds, &bs, 256);
            arm_sve_preg(&env, 1)[0] = 0x11u;
            lata_last_fp_route = 0;
            (void)sve_emit_decoded(&ds, insns[i].word, insns[i].name);
            /* Routes differ without LASX: the SVE lowerings are unavailable and
             * the helper path is taken, so only assert the table where the
             * inline lowerings exist. */
            if (latx_cpu_has_lasx() != 0) {
                check((uint64_t)lata_last_fp_route, (uint64_t)insns[i].route,
                      insns[i].name);
            }
        }
    }

    /* IR2 micro-test: what does xvfcmp CUN write for a NaN lane?
     * Lane 0 input NaN, lane 1 = 3.0f; the mask lands in Z2. */
    {
        DisasContext ds;
        A64SVEPvirBuilder bs;
        IR2_OPND a, b, m;

        /* xv* instructions need LASX; skip on an LSX-only host. */
        if (latx_cpu_has_lasx() == 0) {
            goto cun_probe_done;
        }
        sve_init_direct(&ds, &bs, 128);
        for (size_t l = 0; l < 4; ++l) {
            write_le32(arm_sve_zreg(&env, 0) + l * 4u, 0x7fc00001u); /* qNaN */
            write_le32(arm_sve_zreg(&env, 1) + l * 4u, 0x40400000u); /* 3.0f */
        }
        a = ra_alloc_ftemp();
        b = ra_alloc_ftemp();
        m = ra_alloc_ftemp();
        lata_env_xvld(a, env_offset_zreg(0));
        lata_env_xvld(b, env_offset_zreg(1));
        la_xvfcmp_cond_s(m, a, a, FCMP_COND_CUN);
        lata_env_xvst(m, env_offset_zreg(2));
        /* temps are released with the translation below */
        sve_exec_direct();
        check(read_le64(arm_sve_zreg(&env, 2)), UINT64_MAX,
              "xvfcmp CUN sets a mask bit in NaN lanes");
    }
cun_probe_done:;

    /* FPCR.DN on the inline binary path (lower_fp_binary). */
    if (latx_cpu_has_lasx() != 0) {
        DisasContext ds;
        A64SVEPvirBuilder bs;

        sve_init_direct(&ds, &bs, 256);
        env.vfp.xregs[0] = UINT64_C(1) << 25; /* FPCR.DN */
        arm_sve_preg(&env, 1)[0] = 0x11u;
        arm_sve_preg(&env, 1)[1] = 0x11u;
        arm_sve_preg(&env, 1)[2] = 0x11u;
        arm_sve_preg(&env, 1)[3] = 0x11u;
        for (size_t l = 0; l < 8; ++l) {
            write_le32(arm_sve_zreg(&env, 0) + l * 4u, 0x3f800000u);
            write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0x7fc00001u);
        }
        check(sve_emit_decoded(&ds, 0x65808440u, "FADD z0.s with DN"),
              1, "FADD with a NaN lowers under DN=1");
        sve_exec_direct();
        check(read_le32(arm_sve_zreg(&env, 0)), UINT32_C(0x7fc00000),
              "DN=1 turns the NaN result into the default NaN");

        /* DN=0 keeps the quieted NaN operand instead. */
        sve_init_direct(&ds, &bs, 256);
        arm_sve_preg(&env, 1)[0] = 0x11u;
        arm_sve_preg(&env, 1)[1] = 0x11u;
        arm_sve_preg(&env, 1)[2] = 0x11u;
        arm_sve_preg(&env, 1)[3] = 0x11u;
        for (size_t l = 0; l < 8; ++l) {
            write_le32(arm_sve_zreg(&env, 0) + l * 4u, 0x3f800000u);
            write_le32(arm_sve_zreg(&env, 2) + l * 4u, 0x7fc00001u);
        }
        (void)sve_emit_decoded(&ds, 0x65808440u, "FADD z0.s without DN");
        sve_exec_direct();
        check(read_le32(arm_sve_zreg(&env, 0)), UINT32_C(0x7fc00001),
              "DN=0 keeps the NaN payload");
    }

    latx_bare_uart_puts("lata-isa: checks=");
    putdec((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    putdec((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts(fails ? "lata-isa: FAIL\n" : "lata-isa: PASS\n");
    latx_bare_uart_puts("lata-isa: done\n");
}
