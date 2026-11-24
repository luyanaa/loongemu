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
#include "env.h"
#include "translate.h"
#include "fpu/softfloat.h"
#include "latm-platform.h"

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
static uint8_t code_buf2[CODE_BUF_SIZE] __attribute__((aligned(4096)));
#define PROLOGUE_OFFSET 0x1000
#define EPILOGUE_OFFSET 0x3000
#define TB_OFFSET       0x5000
#define CHAIN_TARGET_OFFSET 0x6000
#define CHAIN_SOURCE_OFFSET 0x7000

static TranslationBlock tb;
static CPUMIPSState env;

static int platform_read(CPUMIPSState *cpu, uint64_t gpa, uint64_t *value,
                         uint32_t size)
{
    (void)cpu;
    *value = 0;
    for (uint32_t i = 0; i < size; i++) {
        *value |= (uint64_t)*(volatile uint8_t *)(uintptr_t)(gpa + i) << (8 * i);
    }
    return LATM_MEM_OK;
}

static int platform_write(CPUMIPSState *cpu, uint64_t gpa, uint64_t value,
                          uint32_t size)
{
    (void)cpu;
    for (uint32_t i = 0; i < size; i++) {
        *(volatile uint8_t *)(uintptr_t)(gpa + i) = (uint8_t)(value >> (8 * i));
    }
    return LATM_MEM_OK;
}

static volatile uint64_t smc_broadcasts;
void latm_platform_smc_broadcast(uint64_t start, uint64_t end)
{
    (void)start;
    (void)end;
    smc_broadcasts++;
}

/* daddiu $2,$0,5; daddiu $3,$0,7; daddu $4,$2,$3; dsubu $5,$4,$2;
 * dsll $6,$5,3; dsra $7,$6,1 */
static uint32_t loongson_code[] __attribute__((aligned(4096))) = {
    0x64080007, 0x64090006, 0x71095010, /* gsmult $10,$8,$9 = 42 */
    0x640b0014, 0x640c0006, 0x716c6814, /* gsdiv $13,$11,$12 = 3 */
    0x640e0014, 0x640f0006, 0x71cf801c, /* gsmod $16,$14,$15 = 2 */
};
static uint8_t cross_page_code[8192] __attribute__((aligned(4096)));
static uint32_t hcall_arg_code[] __attribute__((aligned(4096))) = {
    0x64010007, 0x64020006,
    0x70221810, /* gsmult $3,$1,$2: sources use a1/a2 */
};
static uint32_t loongson_mem_code[] __attribute__((aligned(4096))) = {
    0x64080400, 0x64090011, 0x640a0022,
    0xe909002a,                         /* gssq $9,$10,0($8) */
    0xc90b002c,                         /* gslq $11,$12,0($8) */
};

static uint32_t arith_code[] __attribute__((aligned(4096))) = {
    0x64020005, 0x64030007, 0x0043202d, 0x0082282f, 0x000530f8, 0x0006387b,
};

/* daddiu $2,$0,1; daddiu $3,$0,1; beq $2,$3,target;
 * daddiu $4,$0,0x22 (delay); daddiu $5,$0,0x33 (skipped);
 * target: daddiu $6,$0,0x44 */
static uint32_t branch_taken_code[] __attribute__((aligned(4096))) = {
    0x64020001, 0x64030001, 0x10430002, 0x64040022, 0x64050033, 0x64060044,
};

/* daddiu $8,$0,1; daddiu $9,$0,1; bne $8,$9,skip;
 * daddiu $10,$0,0x22 (delay); daddiu $11,$0,0x33 (fallthrough); skip: nop */
static uint32_t branch_not_code[] __attribute__((aligned(4096))) = {
    0x64080001, 0x64090001, 0x15090002, 0x640a0022, 0x640b0033, 0x00000000,
};

/* daddiu $2,$0,6; daddiu $3,$0,7; dmultu $2,$3; mflo $4; mfhi $5;
 * daddiu $6,$0,0x123; rotr $7,$6,8; drotr $8,$6,12 */
static uint32_t hilo_shift_code[] __attribute__((aligned(4096))) = {
    0x64020006, 0x64030007, 0x0043001d, 0x00002012, 0x00002810,
    0x64060123, 0x00263a02, 0x0026433a,
};

/* jal target; daddiu $9,$0,0x22 (delay); daddiu $10,$0,0x33 (skipped);
 * target: daddiu $11,$0,0x44 */
static uint32_t jal_code[] __attribute__((aligned(4096))) = {
    0x64020001, 0x0c000000, 0x64090022, 0x640a0033, 0x640b0044,
};

/* beql $2,$2,target; daddiu $13,$0,0x22 (delay); daddiu $14,$0,0x33;
 * target: daddiu $15,$0,0x44 */
static uint32_t likely_taken_code[] __attribute__((aligned(4096))) = {
    0x50420002, 0x640d0022, 0x640e0033, 0x640f0044,
};

/* beql $2,$3,target; daddiu $16,$0,0x22 (must be skipped);
 * daddiu $17,$0,0x33 (fallthrough); target: nop */
static uint32_t likely_not_code[] __attribute__((aligned(4096))) = {
    0x64020001, 0x64030002, 0x50430002, 0x64100022, 0x64110033, 0x00000000,
};

/* Store/load word, LWL+LWR, store/load double, LDL+LDR, SDL+LD. */
static uint32_t mem_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0x3c031122, 0x34633344, 0xac430000, 0x64040000,
    0x98440000, 0x88440003, 0x3c051122, 0x34a53344, 0x00052c38,
    0x34a55566, 0x00052c38, 0x34a57788, 0xfc450008, 0x64060000,
    0x6c460008, 0x6846000f, 0xb4450010, 0xdc470010,
};

/* BC1 with nonzero FCC and a helper in the delay slot. */
static uint32_t fpu_bc_code[] __attribute__((aligned(4096))) = {
    0x3c020400, 0x44c2f800, 0x64050400, 0x45090002,
    0xc0a40000, 0x64060033, 0x64070044,
};
static uint32_t fpu_bc_likely_not_code[] __attribute__((aligned(4096))) = {
    0x3c020400, 0x44c2f800, 0x64050400, 0x450a0002,
    0xc0a40000, 0x64060033, 0x64070044,
};

/* Branch condition and JR target must survive a helper in the delay slot. */
static uint32_t delay_branch_code[] __attribute__((aligned(4096))) = {
    0x64020001, 0x64030001, 0x64050400, 0x10430002,
    0xc0a40000, 0x64060033, 0x64070044,
};
static uint32_t delay_jr_code[] __attribute__((aligned(4096))) = {
    0x64050400, 0x3c020000, 0x34420000, 0x00400008,
    0xc0a40000, 0x00000000,
};

/* SC without LL, and LL(A)->SC(B). */
static uint32_t sc_no_ll_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0x64030055, 0xe0440000,
};
static uint32_t ll_sc_alias_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0xc0440000, 0x64050404, 0xe0a60000,
};

/* LL/SC success, then LL/SC invalidated by an intervening normal store. */
static uint32_t llsc_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0x64030123, 0xac430000, 0xc0440000, 0x64050456,
    0xe0450000, 0xc0460000, 0xac430000, 0xe0470000,
};

/* C3 recovery block: env-resident deferred write then a guest load.
 * daddiu $23,$0,7 ; ld $3,0($6). $23 is NOT_DEF (env-resident) so its value
 * stays in a host temp across the ld; $3 is dedicated. */
static uint32_t recov_code[] __attribute__((aligned(4096))) = {
    0x64170007, 0xdcc30000,
};
#define RECOV_INSNS 2

/* COP1X: LDXC1/SDXC1, LWXC1/SWXC1, LUXC1/SUXC1. */
static uint32_t cop1x_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0x64030000, 0x3c043ff8, 0x0004203c, 0xfc440000,
    0x4c430001, 0x4c430009, 0xdc450000, 0x3c063f80, 0xac460020,
    0x4c430040, 0x4c430808, 0x8c470020, 0x4c430085, 0x4c43100d,
    0xdc480000,
};

/* COP1X MADD/MSUB/NMADD/NMSUB.D with f3=1, f1=1.5, f2=2.25. */
static uint32_t cop1x_madd_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0x3c033ff8, 0x0003183c, 0xfc430000,
    0x3c054002, 0x0005283c, 0xfc450008, 0x3c063ff0,
    0x0006303c, 0xfc460010, 0xd4410000, 0xd4420008,
    0xd4430010, 0x4c620821, 0x4c620929, 0x4c620971,
    0x4c6209b9, 0xf4400018, 0xf4440020, 0xf4450028,
    0xf4460030, 0xdc440018, 0xdc450020, 0xdc460028,
    0xdc470030,
};

/* 1.5 + 2.25 through LDC1/add.d/SDC1. */
static uint32_t fpu_code[] __attribute__((aligned(4096))) = {
    0x64020400, 0x3c033ff8, 0x0003183c, 0xfc430000,
    0x3c054002, 0x0005283c, 0xfc450008, 0xd4410000,
    0xd4420008, 0x46220800, 0xf4400010, 0xdc440010,
};

/* lui/ori load the address of jr_code+4 into $2; jr $2; delay. */
static uint32_t jr_code[] __attribute__((aligned(4096))) = {
    0x3c020000, 0x34420000, 0x00400008, 0x64030022,
};

/* lui/ori target; cache 0x10,0($2); cache 0,0($2); synci 0($2). */
static uint32_t cache_code[] __attribute__((aligned(4096))) = {
    0x3c020000, 0x34420000, 0xbc500000, 0xbc400000, 0x045f0000,
};

/* Target TB for the indirect TB-link test: r5=0x55, r6=0x66. */
static uint32_t chain_target_code[] __attribute__((aligned(4096))) = {
    0x64050055, 0x64060066,
};

/* lui/ori target; jr $2; daddiu $3,$0,0x22 (delay). */
static uint32_t chain_source_code[] __attribute__((aligned(4096))) = {
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
extern int lata_gen_prologue_env(void *cpu, TCGContext *tcg_ctx);
extern int lata_gen_epilogue_env(void *cpu, TCGContext *tcg_ctx);
extern int tr_ir2_assemble(const void *code_start_addr);
extern bool tr_ir2_generate(TranslationBlock *tb);
extern void tr_init(void *tb);
extern void tr_fini(void);

static int assemble_block(uint32_t *guest, int insns, uint8_t *host)
{
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    tcg_ctx->code_ptr = host;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tb.pc = (uint64_t)(uintptr_t)guest;
    tb.tc.ptr = host;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;

    int max_insns = insns;
    target_disasm(&tb, &max_insns, &env);
    if (tb.icount == 0 || !tr_ir2_generate(&tb)) {
        tr_fini();
        return -1;
    }
    int n = tr_ir2_assemble(host);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    return n;
}

static void run_chain(int use_jiscr1)
{
    uint8_t *target_host = code_buf + CHAIN_TARGET_OFFSET;
    uint8_t *source_host = code_buf + CHAIN_SOURCE_OFFSET;
    uint32_t target_pc = (uint32_t)(uintptr_t)chain_target_code;

    latx_bare_uart_puts(use_jiscr1 ? "latm-isa: chain-jiscr1 begin\n"
                                   : "latm-isa: chain-jiscr0 begin\n");
    {
        void *le = env.latm_env, *lt = env.latm_tr_data, *lc = env.latm_jmp_cache;
        for (size_t i = 0; i < sizeof(env); i++) {
            ((uint8_t *)&env)[i] = 0;
        }
        env.latm_env = le;
        env.latm_tr_data = lt;
        env.latm_jmp_cache = lc;
    }
    for (size_t i = 0; i < CODE_BUF_SIZE; i++) {
        code_buf[i] = 0;
    }
    latm_jmp_cache_flush(&env);

    tcg_ctx->code_ptr = code_buf + PROLOGUE_OFFSET;
    lata_gen_prologue_env(&env, tcg_ctx);
    tcg_ctx->code_ptr = code_buf + EPILOGUE_OFFSET;
    lata_gen_epilogue_env(&env, tcg_ctx);

    if (assemble_block(chain_target_code, 2, target_host) <= 0) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL chain target assemble\n");
        return;
    }
    latm_jmp_cache_add(&env, (uint64_t)(uintptr_t)chain_target_code,
                       (uint64_t)(uintptr_t)target_host, 8);

    chain_source_code[0] = 0x3c020000u | ((target_pc >> 16) & 0xffffu);
    chain_source_code[1] = 0x34420000u | (target_pc & 0xffffu);
    latm_link_use_jiscr1 = use_jiscr1 != 0;
    if (assemble_block(chain_source_code, 4, source_host) <= 0) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL chain source assemble\n");
        latm_link_use_jiscr1 = false;
        return;
    }

    ((void (*)(void *, void *))(uintptr_t)
     ((TRANSLATION_DATA *)env.latm_tr_data)->cs_bt_to_native)(
        &env, source_host);
    latm_link_use_jiscr1 = false;

    check(env.active_tc.gpr[3], 0x22, "chain delay slot executed");
    check(env.active_tc.gpr[5], 0x55, "chain target executed");
    check(env.active_tc.gpr[6], 0x66, "chain target second instruction");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(chain_target_code + 2),
          "chain target exit PC");
    latx_bare_uart_puts(use_jiscr1 ? "latm-isa: chain-jiscr1 done\n"
                                   : "latm-isa: chain-jiscr0 done\n");
}

static volatile int isa_stop_on_exception;
static volatile int isa_exception_seen;

static int cross_page_fetch(CPUMIPSState *env, uint64_t gpa, uint32_t *value)
{
    (void)env;
    if (gpa >= (uint64_t)(uintptr_t)(cross_page_code + 4096)) {
        return LATM_MEM_FAULT;
    }
    *value = *(const uint32_t *)(uintptr_t)gpa;
    return LATM_MEM_OK;
}

static const LatmPlatform fault_platform = {
    .fetch32 = cross_page_fetch,
};

static void run_block(uint32_t *guest, int insns, const char *name)
{
    uint8_t *tb_code = code_buf + TB_OFFSET;

    latx_bare_uart_puts("latm-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" begin\n");

    {
        void *le = env.latm_env, *lt = env.latm_tr_data, *lc = env.latm_jmp_cache;
        for (size_t i = 0; i < sizeof(env); i++) {
            ((uint8_t *)&env)[i] = 0;
        }
        env.latm_env = le;
        env.latm_tr_data = lt;
        env.latm_jmp_cache = lc;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    for (size_t i = 0; i < CODE_BUF_SIZE; i++) {
        code_buf[i] = 0;
    }
    env.active_fpu.fcr31_rw_bitmask = 0xff83ffff;

    env.active_tc.PC = (uint64_t)(uintptr_t)guest;

    /* The prologue/epilogue live in their own slots; the TB gets a clean one. */
    tcg_ctx->code_ptr = code_buf + PROLOGUE_OFFSET;
    lata_gen_prologue_env(&env, tcg_ctx);
    tcg_ctx->code_ptr = code_buf + EPILOGUE_OFFSET;
    lata_gen_epilogue_env(&env, tcg_ctx);

    tcg_ctx->code_ptr = tb_code;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;

    tb.pc = (uint64_t)(uintptr_t)guest;
    tb.tc.ptr = tb_code;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;

    int max_insns = insns;
    target_disasm(&tb, &max_insns, &env);
    if (tb.icount == 0) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL ");
        latx_bare_uart_puts(name);
        latx_bare_uart_puts(" decoded=0\n");
        tr_fini();
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

    ((void (*)(void *, void *))(uintptr_t)
     ((TRANSLATION_DATA *)env.latm_tr_data)->cs_bt_to_native)(
        &env, tb_code);

    latx_bare_uart_puts("latm-isa: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts(" done\n");
}

/* daddiu $23,$0,0x11; daddiu $23,$23,0x22;
 * daddu $8,$24,$24; daddu $9,$24,$24.
 * $23/$24 are env-resident (mips_la_map == NOT_DEF); $8/$9 are pinned
 * to fixed LA regs. Two writes to $23 coalesce to one env st.d at the
 * TB-end flush; two reads of $24 coalesce to one env ld.d. */
static uint32_t regcache_code[] __attribute__((aligned(4096))) = {
    0x64170011, 0x66f70022, 0x0318402d, 0x0318482d, 0x03e00008, 0x00000000,
};

extern const int mips_la_map[32];

static void run_regcache_ir2_test(void)
{
    latx_bare_uart_puts("latm-isa: regcache-ir2 begin\n");

    {
        void *le = env.latm_env, *lt = env.latm_tr_data, *lc = env.latm_jmp_cache;
        for (size_t i = 0; i < sizeof(env); i++) {
            ((uint8_t *)&env)[i] = 0;
        }
        env.latm_env = le;
        env.latm_tr_data = lt;
        env.latm_jmp_cache = lc;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    for (size_t i = 0; i < CODE_BUF_SIZE; i++) {
        code_buf[i] = 0;
    }
    env.active_tc.PC = (uint64_t)(uintptr_t)regcache_code;
    latm_bind_env(&env);
    tcg_ctx->code_ptr = code_buf + TB_OFFSET;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tb.pc = (uint64_t)(uintptr_t)regcache_code;
    tb.tc.ptr = code_buf + TB_OFFSET;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;

    int max_insns = 6;
    target_disasm(&tb, &max_insns, &env);
    if (tb.icount == 0 || !tr_ir2_generate(&tb)) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL regcache-ir2 translate\n");
        tr_fini();
        return;
    }

    TRANSLATION_DATA *td = (TRANSLATION_DATA *)env.latm_tr_data;
    int env_reg = env_ir2_opnd._reg_num;
    unsigned st23 = 0, ld24 = 0;
    for (IR2_INST *inst = td->first_ir2; inst != NULL;
         inst = ir2_next(inst)) {
        if (inst->op_count != 3 || inst->_opnd[1]._reg_num != env_reg) {
            continue;
        }
        int off = inst->_opnd[2]._imm32;
        if (ir2_opcode(inst) == LISA_ST_D && off == env_offset_gpr(23)) {
            st23++;
        }
        if (ir2_opcode(inst) == LISA_LD_D && off == env_offset_gpr(24)) {
            ld24++;
        }
    }
    tr_fini();

    check(st23, 1, "regcache-ir2 st.d $23 coalesced");
    check(ld24, 1, "regcache-ir2 ld.d $24 coalesced");
    latx_bare_uart_puts("latm-isa: regcache-ir2 st.d=");
    puthex(st23);
    latx_bare_uart_puts(" ld.d=");
    puthex(ld24);
    latx_bare_uart_puts("\n");
}

/* C3: precise host-fault recovery. Build a TB with an env-resident deferred
 * write (daddiu $23) ahead of a guest load (ld $3,0($6)). Walk the span map:
 * the daddiu span is NATIVE_INTERNAL, the ld span is GUEST_ACCESS carrying a
 * bind for $23, and apply() must restore $23 and the PC from host_gprs. */
static void test_tb_recovery(void)
{
    uint8_t *tb_code = code_buf + TB_OFFSET;
    /* daddiu $23,$0,7 ; ld $3,0($6) ; then let the block exit normally */
    uint32_t *guest = recov_code;

    {
        void *le = env.latm_env, *lt = env.latm_tr_data, *lc = env.latm_jmp_cache;
        for (size_t i = 0; i < sizeof(env); i++) {
            ((uint8_t *)&env)[i] = 0;
        }
        env.latm_env = le;
        env.latm_tr_data = lt;
        env.latm_jmp_cache = lc;
    }
    for (size_t i = 0; i < sizeof(tb); i++) {
        ((uint8_t *)&tb)[i] = 0;
    }
    for (size_t i = 0; i < CODE_BUF_SIZE; i++) {
        code_buf[i] = 0;
    }
    env.active_tc.PC = (uint64_t)(uintptr_t)guest;
    tcg_ctx->code_ptr = tb_code;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    tb.pc = (uint64_t)(uintptr_t)guest;
    tb.tc.ptr = tb_code;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;

    TRANSLATION_DATA *td = (TRANSLATION_DATA *)env.latm_tr_data;
    int max_insns = RECOV_INSNS;
    target_disasm(&tb, &max_insns, &env);
    if (tb.icount == 0 || !tr_ir2_generate(&tb)) {
        fails++;
        checks++;
        latx_bare_uart_puts("latm-isa: FAIL tb-recovery translate\n");
        tr_fini();
        return;
    }
    int n = tr_ir2_assemble(tb_code);
    tr_fini();
    check(n > 0, 1, "tb-recovery assemble");

    uintptr_t base = (uintptr_t)tb_code;
    const LatmTbInsnMap *map = NULL;

    /* First span: daddiu $23 — NATIVE_INTERNAL; apply refuses. */
    const LatmTbInsnSpan *s0 = latm_tb_recovery_resolve(td, base, &map);
    check(s0 != NULL, 1, "tb-recovery resolve first span");
    if (s0) {
        check(s0->site, LATM_RECOV_NATIVE_INTERNAL, "tb-recovery daddiu site");
        uint64_t hg[32] = {0};
        check(latm_tb_recovery_apply(s0, map, hg, &env), 0,
              "tb-recovery apply refuses NATIVE_INTERNAL");
    }

    /* Find the ld span (GUEST_ACCESS) by scanning host words. */
    const LatmTbInsnSpan *ld = NULL;
    for (uint32_t off = 0; off < (uint32_t)n; off += 4) {
        const LatmTbInsnMap *m2 = NULL;
        const LatmTbInsnSpan *s = latm_tb_recovery_resolve(td, base + off, &m2);
        if (s && s->site == LATM_RECOV_GUEST_ACCESS) {
            ld = s;
            map = m2;
            break;
        }
    }
    check(ld != NULL, 1, "tb-recovery found ld span");
    if (!ld) {
        latx_bare_uart_puts("latm-isa: tb-recovery no-ld\n");
        return;
    }
    check(ld->guest_pc, (uint64_t)(uintptr_t)(guest + 1),
          "tb-recovery ld guest_pc");
    check(ld->bind_cnt >= 1, 1, "tb-recovery ld has bind");

    /* The bind must name env-resident $23 in a host temp. Replay it. */
    int8_t t23 = -1;
    if (map) {
        for (int i = 0; i < ld->bind_cnt; i++) {
            const LatmRecoveryBinding *b = &map->binds[ld->bind_first + i];
            if (b->guest_reg == 23) {
                t23 = b->temp;
            }
        }
    }
    check(t23 >= 0, 1, "tb-recovery $23 bound to a host temp");

    uint64_t hg[32];
    for (int i = 0; i < 32; i++) hg[i] = 0xdead0000 + i;
    if (t23 >= 0) hg[t23] = 0x515253540000ull;
    /* dedicated $3 lives in a3 (mips_la_map[3]) — give it a fault-time value */
    hg[mips_la_map[3]] = 0xaabbccdd;

    env.active_tc.gpr[23] = 0;
    env.active_tc.gpr[3] = 0;
    env.active_tc.PC = 0;
    check(latm_tb_recovery_apply(ld, map, hg, &env), 1,
          "tb-recovery apply ok");
    check(env.active_tc.gpr[23], 0x515253540000ull,
          "tb-recovery restores $23 from bind");
    check(env.active_tc.gpr[3], 0xaabbccdd,
          "tb-recovery restores dedicated $3");
    check(env.active_tc.PC, ld->guest_pc,
          "tb-recovery restores PC");
    latx_bare_uart_puts("latm-isa: tb-recovery done\n");
}

void kmain(void)
{
    latx_bare_uart_puts("latm-isa: begin\n");

    lata_tr_data_init(&env);
    {
        CPUMIPSState env2;
        for (size_t i = 0; i < sizeof(env2); i++) {
            ((uint8_t *)&env2)[i] = 0;
        }
        lata_tr_data_init(&env2);
        latm_bind_env(&env);
        tcg_ctx->code_ptr = code_buf + PROLOGUE_OFFSET;
        lata_gen_prologue_env(&env, tcg_ctx);
        tcg_ctx->code_ptr = code_buf + EPILOGUE_OFFSET;
        lata_gen_epilogue_env(&env, tcg_ctx);
        check((uint64_t)(uintptr_t)env2.latm_tr_data !=
              (uint64_t)(uintptr_t)env.latm_tr_data, 1,
              "per-vCPU translation data");
        check(((TRANSLATION_DATA *)env2.latm_tr_data)->ir1_list !=
              ((TRANSLATION_DATA *)env.latm_tr_data)->ir1_list, 1,
              "per-vCPU IR1 scratch");
        check((uint64_t)(uintptr_t)env2.latm_jmp_cache !=
              (uint64_t)(uintptr_t)env.latm_jmp_cache, 1,
              "per-vCPU TB link cache");
        latm_bind_env(&env2);
        tcg_ctx->code_ptr = code_buf2 + PROLOGUE_OFFSET;
        lata_gen_prologue_env(&env2, tcg_ctx);
        tcg_ctx->code_ptr = code_buf2 + EPILOGUE_OFFSET;
        lata_gen_epilogue_env(&env2, tcg_ctx);
        check(((TRANSLATION_DATA *)env2.latm_tr_data)->cs_bt_to_native !=
              ((TRANSLATION_DATA *)env.latm_tr_data)->cs_bt_to_native, 1,
              "per-vCPU context-switch entry");
        latm_bind_env(&env);
    }

    run_block(loongson_code, 9, "loongson");
    check(env.active_tc.gpr[10], 42, "gsmult");
    check(env.active_tc.gpr[13], 3, "gsdiv");
    check(env.active_tc.gpr[16], 2, "gsmod");
    run_block(hcall_arg_code, 3, "hcall-args");
    check(env.active_tc.gpr[3], 42, "hcall parallel argument copy");
    {
        uint32_t *last = (uint32_t *)(cross_page_code + 4096 - 4);
        last[0] = 0x10420001; /* beq $2,$2,+1 */
        last[1] = 0x64030022; /* daddiu $3,$0,0x22 (delay) */
        last[2] = 0x00000000;
        run_block(last, 3, "branch-cross-page");
        check(env.active_tc.gpr[3], 0x22, "cross-page delay slot executed");
    }
    {
        uint32_t *last = (uint32_t *)(cross_page_code + 4096 - 4);
        int max_insns = 2;
        latx_bare_uart_puts("latm-isa: fetch-fault begin\n");
        memset(&tb, 0, sizeof(tb));
        tb.pc = (uint64_t)(uintptr_t)last;
        tb.tc.ptr = code_buf + TB_OFFSET;
        isa_stop_on_exception = 1;
        isa_exception_seen = 0;
        latm_platform_set(&fault_platform);
        target_disasm(&tb, &max_insns, &env);
        tr_fini();
        latm_platform_set(NULL);
        isa_stop_on_exception = 0;
        check(tb.icount, 0, "cross-page fetch fault rejects TB");
        check(env.latm_fetch_fault, 1, "cross-page fetch fault recorded");
        check(isa_exception_seen, 1, "cross-page fetch raises exception");
        latx_bare_uart_puts("latm-isa: fetch-fault done\n");
    }
    run_block(loongson_mem_code, 5, "loongson-mem");
    check(env.active_tc.gpr[11], 0x11, "gssq/gslq first");
    check(env.active_tc.gpr[12], 0x22, "gssq/gslq second");

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

    {
        static LatmPlatform plat = { .mem_read = platform_read,
                                     .mem_write = platform_write };
        latm_platform_set(&plat);
        latm_set_system_memory(true);
        run_block(mem_code, 19, "memory-system");
        latm_set_system_memory(false);
        latm_platform_set(NULL);
    }
    check(env.active_tc.gpr[2], 0x400, "memory base");
    check(env.active_tc.gpr[3], 0x11223344u, "memory word source");
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

    run_block(delay_branch_code, 7, "delay-branch-helper");
    check(env.active_tc.gpr[6], 0, "branch condition survives delay helper");
    check(env.active_tc.PC,
          (uint64_t)(uintptr_t)(delay_branch_code + 6),
          "branch target survives delay helper");

    {
        uint32_t target = (uint32_t)(uintptr_t)(delay_jr_code + 5);
        delay_jr_code[1] = 0x3c020000u | ((target >> 16) & 0xffffu);
        delay_jr_code[2] = 0x34420000u | (target & 0xffffu);
    }
    run_block(delay_jr_code, 5, "delay-jr-helper");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(delay_jr_code + 5),
          "JR target survives delay helper");

    run_block(llsc_code, 9, "llsc");
    check(env.active_tc.gpr[5], 1, "sc succeeds after ll");
    check(env.active_tc.gpr[7], 0, "sc fails after intervening store");

    run_block(sc_no_ll_code, 3, "sc-no-ll");
    check(env.active_tc.gpr[4], 0, "sc without ll fails");
    run_block(ll_sc_alias_code, 4, "ll-sc-alias");
    check(env.active_tc.gpr[6], 0, "ll(A)->sc(B) fails");

    enable_fpu();
    run_block(cop1x_code, 16, "cop1x");
    check(env.active_tc.gpr[5], 0x3ff8000000000000ull, "ldxc1/sdxc1");
    check(env.active_tc.gpr[7], 0x3f800000u, "lwxc1/swxc1");
    check(env.active_tc.gpr[8], 0x3ff8000000000000ull, "luxc1/suxc1");

    run_block(cop1x_madd_code, 25, "cop1x-madd");
    check(env.active_tc.gpr[4], 0x4011800000000000ull, "madd.d");
    check(env.active_tc.gpr[5], 0x4003000000000000ull, "msub.d");
    check(env.active_tc.gpr[6], 0xc011800000000000ull, "nmadd.d");
    check(env.active_tc.gpr[7], 0xc003000000000000ull, "nmsub.d");

    run_block(fpu_code, 12, "fpu-add");
    check(env.active_tc.gpr[4], 0x400e000000000000ull, "1.5 + 2.25");

    run_block(fpu_bc_code, 7, "fpu-bc-fcc2");
    check(env.active_tc.gpr[6], 0, "bc1 delay slot runs on taken");
    check(env.active_tc.PC, (uint64_t)(uintptr_t)(fpu_bc_code + 6),
          "bc1t FCC2 target");

    run_block(fpu_bc_likely_not_code, 6, "fpu-bc-likely");
    check(env.active_tc.gpr[4], 0, "likely-not-taken skips delay slot");
    check(env.active_tc.PC,
          (uint64_t)(uintptr_t)(fpu_bc_likely_not_code + 5),
          "bc1fl FCC2 fallthrough");

    run_chain(0);
    run_chain(1);

    {
        static const uint32_t flush_ops[] = { 0xbc500000, 0xbc400000, 0x045f0000 };
        uint32_t target = (uint32_t)(uintptr_t)chain_target_code;
        cache_code[0] = 0x3c020000u | ((target >> 16) & 0xffffu);
        cache_code[1] = 0x34420000u | (target & 0xffffu);
        latm_jmp_cache_add(&env, (uint64_t)(uintptr_t)chain_target_code,
                           (uint64_t)(uintptr_t)code_buf + 0x6000, 8);
        check(latm_jmp_cache_lookup(
                  &env, (uint64_t)(uintptr_t)chain_target_code),
              (uint64_t)(uintptr_t)code_buf + 0x6000, "cache key hit ASID0");
        env.CP0_EntryHi = 1;
        check(latm_jmp_cache_lookup(
                  &env, (uint64_t)(uintptr_t)chain_target_code), 0,
              "cache key rejects different ASID");
        env.CP0_EntryHi = 0;
        env.hflags = MIPS_HFLAG_UM;
        check(latm_jmp_cache_lookup(
                  &env, (uint64_t)(uintptr_t)chain_target_code), 0,
              "cache key rejects different mode");
        env.hflags = 0;
        /* Same 2-way set: third insert must evict the oldest. */
        latm_jmp_cache_add(&env, 0x1000, 0x11110000, 8);
        latm_jmp_cache_add(&env, 0x1800, 0x22220000, 8);
        latm_jmp_cache_add(&env, 0x2000, 0x33330000, 8);
        check(latm_jmp_cache_lookup(&env, 0x1000), 0,
              "2-way cache evicts oldest");
        check(latm_jmp_cache_lookup(&env, 0x1800), 0x22220000,
              "2-way cache keeps middle");
        check(latm_jmp_cache_lookup(&env, 0x2000), 0x33330000,
              "2-way cache keeps newest");
        for (unsigned i = 0; i < sizeof(flush_ops) / sizeof(flush_ops[0]); i++) {
            cache_code[2] = flush_ops[i];
            latm_jmp_cache_add(&env, (uint64_t)(uintptr_t)chain_target_code,
                               (uint64_t)(uintptr_t)code_buf + 0x6000, 8);
            run_block(cache_code, 3, "cache-synci");
            check(latm_jmp_cache_lookup(
                      &env, (uint64_t)(uintptr_t)chain_target_code), 0,
                  "CACHE/SYNCI invalidates TB link cache");
        }
        check(smc_broadcasts >= 3, 1, "CACHE/SYNCI broadcasts SMC invalidation");
    }

    run_regcache_ir2_test();
    test_tb_recovery();

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
void cpu_loop_exit(struct CPUState *cs)
{
    (void)cs;
    if (isa_stop_on_exception) {
        isa_exception_seen = 1;
        return;
    }
    latx_bare_uart_puts("latm-isa: cpu_loop_exit\n");
    for (;;) { }
}
FILE *stderr;
