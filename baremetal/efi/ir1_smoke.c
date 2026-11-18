/*
 * EFI smoke test for the rebuilt translation front end.
 *
 * Stage 1: decode x86 byte sequences through the tree-owned capstone adapter
 *          and check the IR1 opcode/inst length.
 * Stage 2: run one TB through tr_translate_tb() so IR1 -> IR2 lowering is
 *          exercised on real LoongArch (QEMU), without executing translated
 *          code and therefore without needing LBT.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "ir1.h"
#include "ir2.h"
#include "translate.h"
#include <capstone/capstone.h>

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

static int checks;
static int fails;

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

typedef struct {
    const char *name;
    uint8_t bytes[16];
    int len;
    int want;
} Vector;


/* ---------------- minimal LBT trap emulation (QEMU has no LBT) ------------
 * The generated prologue/epilogue move EFLAGS/TOP through LBT.  QEMU raises
 * #INE for those opcodes, so install our own EENTRY and emulate the small
 * subset with a software LBT4 image. */
typedef struct {
    uint64_t scr[4];
    uint32_t eflags;
    uint32_t top;
    int tm;
} SwLbt;

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

extern void latx_lbt_entry(void);

static int is_lbt_word(uint32_t w)
{
    uint32_t b0 = w & 0xff, b1 = (w >> 8) & 0xff;
    uint32_t b2 = (w >> 16) & 0xff, b3 = (w >> 24) & 0xff;

    if (b3 != 0) {
        return 0;
    }
    if (b2 == 0x5c) {
        return (b1 & 3) == 0;
    }
    if (b2 != 0x00) {
        return 0;
    }
    if (b1 == 0x70) {
        return (b0 & 0x1f) == 0;                 /* x86mttop imm */
    }
    if (b1 == 0x74) {
        return (b0 & 0xe0) == 0;                 /* x86mftop rd  */
    }
    if (b1 == 0x80) {
        return b0 == 0x09 || b0 == 0x29 || b0 == 0x08 || b0 == 0x28;
    }
    return 0;
}

void latx_lbt_handle(uint64_t *regs)
{
    uint64_t badi, era;
    uint32_t w, b0, b1, b2;
    uint32_t rd, imm;
    uint64_t era_advance = 4;
    int handled = 0;

    __asm__ volatile("csrrd %0, 0x6" : "=r"(era));     /* ERA  */
    (void)badi;
    {
        uint64_t estat;
        uint32_t is;

        __asm__ volatile("csrrd %0, 0x5" : "=r"(estat));  /* ESTAT */
        is = (uint32_t)((estat >> 16) & 0x3f);
        if (is == 0 && (estat & 0x800)) {
            /* Pending architectural timer interrupt: acknowledge and resume
             * at the interrupted instruction. */
            __asm__ volatile("csrwr %0, 0x44" :: "r"((uint64_t)1));
            return;
        }
        if (is != 0x0d) {
            latx_bare_uart_puts("trap: is=");
            puthex(is);
            latx_bare_uart_puts(" estat=");
            puthex(estat);
            latx_bare_uart_puts(" era=");
            puthex(era);
            latx_bare_uart_puts("\n");
            for (;;) {
            }
        }
    }
    /* QEMU's INE delivery leaves ERA at the following instruction in this
     * configuration.  Prefer the word just before ERA when it looks like an
     * LBT opcode, otherwise treat ERA as the faulting PC. */
    {
        uint32_t prev = *(volatile uint32_t *)(uintptr_t)(era - 4);
        uint32_t here = *(volatile uint32_t *)(uintptr_t)era;
        int prev_lbt = is_lbt_word(prev);
        int here_lbt = is_lbt_word(here);

        if (prev_lbt && !here_lbt) {
            w = prev;
            era_advance = 0;   /* ERA already points past the faulting insn */
        } else {
            w = here;
            era_advance = 4;
        }
    }
    b0 = w & 0xff; b1 = (w >> 8) & 0xff; b2 = (w >> 16) & 0xff;
    rd = b0 & 0x1f;
    imm = (b1 >> 2) & 0x3f;

    if (b2 == 0x5c) {
        if ((b0 & 0x20) == 0) {                 /* x86mfflag */
            regs[rd] = pack_flags(sw_lbt.eflags, imm);
            handled = 1;
        } else {                                /* x86mtflag */
            unpack_flags(&sw_lbt.eflags, regs[rd], imm);
            handled = 1;
        }
    } else if (b2 == 0x00 && b1 == 0x74) {      /* x86mftop */
        regs[rd] = sw_lbt.top;
        handled = 1;
    } else if (b2 == 0x00 && b1 == 0x70) {      /* x86mttop */
        sw_lbt.top = (b0 >> 5) & 7;
        handled = 1;
    } else if (b2 == 0x00 && b1 == 0x80) {
        if (b0 == 0x09) { sw_lbt.top = (sw_lbt.top + 1) & 7; handled = 1; }
        else if (b0 == 0x29) { sw_lbt.top = (sw_lbt.top - 1) & 7; handled = 1; }
        else if (b0 == 0x08) { sw_lbt.tm = 1; handled = 1; }
        else if (b0 == 0x28) { sw_lbt.tm = 0; handled = 1; }
    }

    if (!handled) {
        latx_bare_uart_puts("lbt: word=");
        puthex(w);
        latx_bare_uart_puts("\\n");
    }
    __asm__ volatile("csrwr %0, 0x6" :: "r"(era + era_advance));
}

__asm__(
".text\n"
".globl latx_lbt_entry\n"
"latx_lbt_entry:\n"
"  addi.d $sp, $sp, -264\n"
"  st.d $ra, $sp, 8\n"
"  st.d $tp, $sp, 16\n"
"  st.d $r3, $sp, 24\n"
"  st.d $r4, $sp, 32\n"
"  st.d $r5, $sp, 40\n"
"  st.d $r6, $sp, 48\n"
"  st.d $r7, $sp, 56\n"
"  st.d $r8, $sp, 64\n"
"  st.d $r9, $sp, 72\n"
"  st.d $r10, $sp, 80\n"
"  st.d $r11, $sp, 88\n"
"  st.d $r12, $sp, 96\n"
"  st.d $r13, $sp, 104\n"
"  st.d $r14, $sp, 112\n"
"  st.d $r15, $sp, 120\n"
"  st.d $r16, $sp, 128\n"
"  st.d $r17, $sp, 136\n"
"  st.d $r18, $sp, 144\n"
"  st.d $r19, $sp, 152\n"
"  st.d $r20, $sp, 160\n"
"  st.d $r21, $sp, 168\n"
"  st.d $r22, $sp, 176\n"
"  st.d $r23, $sp, 184\n"
"  st.d $r24, $sp, 192\n"
"  st.d $r25, $sp, 200\n"
"  st.d $r26, $sp, 208\n"
"  st.d $r27, $sp, 216\n"
"  st.d $r28, $sp, 224\n"
"  st.d $r29, $sp, 232\n"
"  st.d $r30, $sp, 240\n"
"  st.d $r31, $sp, 248\n"
"  move $a0, $sp\n"
"  bl latx_lbt_handle\n"
"  ld.d $ra, $sp, 8\n"
"  ld.d $tp, $sp, 16\n"
"  ld.d $r3, $sp, 24\n"
"  ld.d $r4, $sp, 32\n"
"  ld.d $r5, $sp, 40\n"
"  ld.d $r6, $sp, 48\n"
"  ld.d $r7, $sp, 56\n"
"  ld.d $r8, $sp, 64\n"
"  ld.d $r9, $sp, 72\n"
"  ld.d $r10, $sp, 80\n"
"  ld.d $r11, $sp, 88\n"
"  ld.d $r12, $sp, 96\n"
"  ld.d $r13, $sp, 104\n"
"  ld.d $r14, $sp, 112\n"
"  ld.d $r15, $sp, 120\n"
"  ld.d $r16, $sp, 128\n"
"  ld.d $r17, $sp, 136\n"
"  ld.d $r18, $sp, 144\n"
"  ld.d $r19, $sp, 152\n"
"  ld.d $r20, $sp, 160\n"
"  ld.d $r21, $sp, 168\n"
"  ld.d $r22, $sp, 176\n"
"  ld.d $r23, $sp, 184\n"
"  ld.d $r24, $sp, 192\n"
"  ld.d $r25, $sp, 200\n"
"  ld.d $r26, $sp, 208\n"
"  ld.d $r27, $sp, 216\n"
"  ld.d $r28, $sp, 224\n"
"  ld.d $r29, $sp, 232\n"
"  ld.d $r30, $sp, 240\n"
"  ld.d $r31, $sp, 248\n"
"  addi.d $sp, $sp, 264\n"
"  ertn\n"
);

static uint64_t latx_setup_lbt_handler(void)
{
    uint64_t old;

    __asm__ volatile("csrrd %0, 0xc" : "=r"(old));
    __asm__ volatile("csrwr %0, 0xc" :: "r"((uint64_t)latx_lbt_entry));
    return old;
}

extern uint64_t latx_run_body(void *tb, void *env);

__asm__(
".text\n"
".globl latx_run_body\n"
"latx_run_body:\n"
"  addi.d $sp, $sp, -256\n"
"  st.d $ra, $sp, 120\n"
"  st.d $fp, $sp, 112\n"
"  st.d $s0, $sp, 40\n"
"  st.d $s1, $sp, 48\n"
"  st.d $s2, $sp, 56\n"
"  st.d $s3, $sp, 64\n"
"  st.d $s4, $sp, 72\n"
"  st.d $s5, $sp, 80\n"
"  st.d $s6, $sp, 88\n"
"  st.d $s7, $sp, 96\n"
"  st.d $s8, $sp, 104\n"
"  move $s8, $a1\n"
"  ld.d $s0, $a1, 0\n"
"  ld.d $s3, $a1, 24\n"
"  jirl $zero, $a0, 0\n"
);

static CPUX86State env;
static TCGContext tcg_storage;
static uint8_t code_cache[1 << 16] __attribute__((aligned(16)));
static uint8_t ir1_dt_area[8192] __attribute__((aligned(16)));
static IR1_INST ir1_list[8];
static struct separated_data ir1_sd;
static TranslationBlock smoke_tb;
static TRANSLATION_DATA tr;
static ENV lsenv_storage;

int efi_main(void *image, void *systab)
{
    static const Vector vecs[] = {
        { "mov rax,rbx", { 0x48, 0x89, 0xd8 }, 3, dt_X86_INS_MOV },
        { "add eax,1",   { 0x83, 0xc0, 0x01 }, 3, dt_X86_INS_ADD },
        { "xor rax,rax", { 0x48, 0x31, 0xc0 }, 3, dt_X86_INS_XOR },
        { "ret",         { 0xc3 },             1, dt_X86_INS_RET },
        { "je rel32",    { 0x0f, 0x84, 0, 0, 0, 0 }, 6, dt_X86_INS_JE },
    };
    unsigned i;

    (void)image;
    (void)systab;
    latx_bare_uart_puts("ir1-smoke: start\n");
    {
        uint32_t a, b, c, d;
        int lasx = latx_cpu_has_lasx();

        cpu_x86_cpuid(&env, 1, 0, &a, &b, &c, &d);
        latx_bare_uart_puts("ir1-smoke: cpuid1 ecx=");
        puthex(c);
        check(((c >> 20) & 1) == 1, "SSE4.2 in baseline");
        check(((c >> 23) & 1) == 1, "POPCNT in baseline");
        check(((c >> 28) & 1) == (uint32_t)lasx, "AVX gated by LASX");

        cpu_x86_cpuid(&env, 7, 0, &a, &b, &c, &d);
        latx_bare_uart_puts(" ebx7=");
        puthex(b);
        check(((b >> 5) & 1) == (uint32_t)lasx, "AVX2 gated by LASX");
        check(((b >> 16) & 1) == 0, "no AVX-512F");

        latx_cpu_set_vector_features(1, 0); /* LU: LSX only -> x86-64-v2 */
        cpu_x86_cpuid(&env, 1, 0, &a, &b, &c, &d);
        check(((c >> 28) & 1) == 0, "LSX-only hides AVX");
        check(((c >> 20) & 1) == 1 && ((c >> 23) & 1) == 1,
              "LSX-only keeps v2 baseline");
        cpu_x86_cpuid(&env, 7, 0, &a, &b, &c, &d);
        check(((b >> 5) & 1) == 0, "LSX-only hides AVX2");
        latx_cpu_set_vector_features(lasx, lasx);
        latx_bare_uart_puts("\n");
    }


    /* The decoder needs lsenv for the code-mode flag. */
    env.sys.codemode = 1;
    lsenv_storage.cpu_state = &env;
    lsenv_storage.tr_data = &tr;
    lsenv = &lsenv_storage;

    for (i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
        uint8_t code[16] __attribute__((aligned(8))) = { 0 };
        IR1_INST ir1;
        uint8_t dtbuf[2048] __attribute__((aligned(16)));
        ADDRX next;
        int op;

        for (int b = 0; b < vecs[i].len; b++) {
            code[b] = vecs[i].bytes[b];
        }
        memset(&ir1, 0, sizeof(ir1));

        next = ir1_disasm(&ir1, code, (ADDRX)0x1000, 0, dtbuf);
        latx_bare_uart_puts("ir1-smoke: ");
        latx_bare_uart_puts(vecs[i].name);
        latx_bare_uart_puts(" next=");
        puthex((uint64_t)next);
        op = ir1.info ? (int)ir1.info->id : -1;
        latx_bare_uart_puts(" op=");
        puthex((uint64_t)(uint32_t)op);
        latx_bare_uart_puts(" want=");
        puthex((uint64_t)(uint32_t)vecs[i].want);
        latx_bare_uart_puts("\n");

        check(ir1.info != NULL, "decoder produced IR1");
        check((int)next == 0x1000 + vecs[i].len, "instruction length");
        check(op == vecs[i].want, "IR1 opcode id");
    }

    latx_bare_uart_puts("ir1-smoke: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails == 0 ? " ok\n" : " bad\n");
    /* ---- stage 2: IR1 -> IR2 lowering for one basic block ---- */
    latx_bare_uart_puts("ir1-smoke: translate start\n");
    {
        static const uint8_t tb_bytes[] = {
            0x48, 0x89, 0xd8,       /* mov rax, rbx */
            0x83, 0xc0, 0x01,       /* add eax, 1    */
            0xc3,                   /* ret           */
        };
        uint8_t code[32] __attribute__((aligned(8))) = { 0 };
        int off = 0;
        int n = 0;
        int rc;

        for (unsigned b = 0; b < sizeof(tb_bytes); b++) {
            code[b] = tb_bytes[b];
        }
        memset(&tcg_storage, 0, sizeof(tcg_storage));
        tcg_storage.code_gen_buffer = code_cache;
        tcg_storage.code_gen_buffer_size = sizeof(code_cache);
        tcg_storage.code_gen_ptr = code_cache;
        tcg_storage.code_gen_highwater = code_cache + sizeof(code_cache);
        tcg_ctx = &tcg_storage;
        tcg_storage.gen_insn_data = latx_tcg_gen_insn_data;

        memset(&ir1_sd, 0, sizeof(ir1_sd));
        memset(&smoke_tb, 0, sizeof(smoke_tb));
        memset(ir1_list, 0, sizeof(ir1_list));

        while (off < (int)sizeof(tb_bytes) && n < 8) {
            ADDRX next = ir1_disasm(&ir1_list[n], code + off,
                                    (ADDRX)(0x1000 + off), n, ir1_dt_area);
            if (next <= (ADDRX)(0x1000 + off)) {
                break;
            }
            off = (int)(next - 0x1000);
            n++;
        }
        latx_bare_uart_puts("ir1-smoke: ir1 count=");
        puthex((uint64_t)n);

        ir1_sd.ir1 = ir1_list;
        ir1_sd.is_first_tb = 1;
        ir1_sd.next_pc = 0x1000 + off;
        smoke_tb.pc = 0x1000;
        smoke_tb.size = (uint16_t)off;
        smoke_tb.icount = (uint16_t)n;
        smoke_tb.tc.ptr = code_cache;
        smoke_tb.s_data = &ir1_sd;

        rc = tr_translate_tb(&smoke_tb);
        latx_bare_uart_puts(" rc=");
        puthex((uint64_t)(int64_t)rc);
        latx_bare_uart_puts(" ir2=");
        puthex((uint64_t)tr.real_ir2_inst_num);
        latx_bare_uart_puts(" code_size=");
        puthex((uint64_t)smoke_tb.tc.size);
        latx_bare_uart_puts(" first=");
        puthex((uint64_t)((const uint32_t *)code_cache)[0]);
        latx_bare_uart_puts(" second=");
        puthex((uint64_t)((const uint32_t *)code_cache)[1]);
        latx_bare_uart_puts("\n");
        /* tr_translate_tb returns the assembled code size in bytes. */
        check(rc > 0, "tr_translate_tb produced code");
        check(((const uint32_t *)code_cache)[0] != 0, "code cache written");
    }
    /* ---- stage 3: EVEX/AVX-512 must be routed to the invalid path ---- */
    {
        static const uint8_t evex_bytes[] = { 0x62, 0xf1, 0x7c, 0x48, 0x58, 0xc1 };
        uint8_t code[32] __attribute__((aligned(8))) = { 0 };
        int n = 0;
        int rc;

        for (unsigned b = 0; b < sizeof(evex_bytes); b++) {
            code[b] = evex_bytes[b];
        }
        memset(&ir1_sd, 0, sizeof(ir1_sd));
        memset(&smoke_tb, 0, sizeof(smoke_tb));
        memset(ir1_list, 0, sizeof(ir1_list));
        memset(&tcg_storage, 0, sizeof(tcg_storage));
        tcg_storage.code_gen_buffer = code_cache;
        tcg_storage.code_gen_buffer_size = sizeof(code_cache);
        tcg_storage.code_gen_ptr = code_cache;
        tcg_storage.code_gen_highwater = code_cache + sizeof(code_cache);
        tcg_ctx = &tcg_storage;
        tcg_storage.gen_insn_data = latx_tcg_gen_insn_data;

        latx_bare_uart_puts("ir1-smoke: evex decode begin\n");
        {
            ADDRX next = ir1_disasm(&ir1_list[0], code, (ADDRX)0x2000, 0,
                                    ir1_dt_area);
            latx_bare_uart_puts("ir1-smoke: evex decode end\n");
            n = (next > 0x2000) ? 1 : 0;
        }
        latx_bare_uart_puts("ir1-smoke: evex translate begin\n");
        latx_bare_uart_puts("ir1-smoke: evex decoded=");
        puthex((uint64_t)n);
        check(n == 1, "EVEX instruction decodes");

        ir1_sd.ir1 = ir1_list;
        ir1_sd.is_first_tb = 1;
        ir1_sd.next_pc = 0x2006;
        smoke_tb.pc = 0x2000;
        smoke_tb.size = 6;
        smoke_tb.icount = 1;
        smoke_tb.tc.ptr = code_cache;
        smoke_tb.s_data = &ir1_sd;

        rc = tr_translate_tb(&smoke_tb);
        latx_bare_uart_puts(" rc=");
        puthex((uint64_t)(int64_t)rc);
        latx_bare_uart_puts("\n");
        check(rc > 0, "EVEX routed to invalid helper code");
    }
    latx_bare_uart_puts("ir1-smoke: translate done\n");
    /* ---- stage 4: execute a translated TB through the real stubs ---- */
    latx_bare_uart_puts("ir1-smoke: exec start\n");
    {
        /* UEFI may leave the FPU/vector units disabled; the generated stubs
         * touch FCSR, so enable them explicitly (CSR.EUEN = 0x2). */
        unsigned long euen;
        unsigned long mask = 0x1 | 0x2 | 0x40 | 0x80;

        __asm__ volatile("csrrd %0, 0x2" : "=r"(euen));
        euen |= mask;
        __asm__ volatile("csrwr %0, 0x2" :: "r"(euen));
    }
    {
        static const uint8_t exec_bytes[] = {
            0x48, 0x89, 0xd8,       /* mov rax, rbx  */
            0x48, 0x8d, 0x40, 0x01, /* lea rax,[rax+1] (no flags) */
            0xeb, 0xf8,             /* jmp 0x3000 (emits the TB exit) */
        };
        uint8_t code[32] __attribute__((aligned(8))) = { 0 };
        static uint8_t jmp_cache[4096] __attribute__((aligned(16)));
        uint8_t *pro_buf = code_cache;
        uint8_t *epi_buf = code_cache + 0x2000;
        uint8_t *tb_buf = code_cache + 0x4000;
        int off = 0, n = 0, rc, pro, epi;
        uintptr_t (*enter)(const void *, void *);

        for (unsigned b = 0; b < sizeof(exec_bytes); b++) {
            code[b] = exec_bytes[b];
        }
        memset(&tcg_storage, 0, sizeof(tcg_storage));
        tcg_storage.code_gen_buffer = code_cache;
        tcg_storage.code_gen_buffer_size = sizeof(code_cache);
        tcg_storage.code_gen_ptr = code_cache;
        tcg_storage.code_gen_highwater = code_cache + sizeof(code_cache);
        tcg_ctx = &tcg_storage;
        tcg_storage.gen_insn_data = latx_tcg_gen_insn_data;
        memset(jmp_cache, 0, sizeof(jmp_cache));
        env.tb_jmp_cache_ptr = jmp_cache;

        {
            uint64_t old_entry = latx_setup_lbt_handler();
            latx_bare_uart_puts("ir1-smoke: lbt handler installed, old=");
            puthex(old_entry);
            latx_bare_uart_puts("\n");
        }
        latx_bare_uart_puts("ir1-smoke: probing lbt handler\n");
        __asm__ volatile(".word 0x00008009"); /* x86inctop */
        latx_bare_uart_puts("ir1-smoke: lbt probe returned\n");
        latx_runtime_init();
        pro = 0;
        epi = latx_gen_epilogue(epi_buf);
        latx_bare_uart_puts("ir1-smoke: stubs pro=");
        puthex((uint64_t)pro);
        latx_bare_uart_puts(" epi=");
        puthex((uint64_t)epi);

        memset(&ir1_sd, 0, sizeof(ir1_sd));
        memset(&smoke_tb, 0, sizeof(smoke_tb));
        memset(ir1_list, 0, sizeof(ir1_list));
        memset(&env.regs, 0, sizeof(env.regs));
        env.regs[R_EAX] = 0x1111;
        env.regs[R_EBX] = 0x2222;
        env.eip = 0x3000;

        while (off < (int)sizeof(exec_bytes) && n < 8) {
            ADDRX next = ir1_disasm(&ir1_list[n], code + off,
                                    (ADDRX)(0x3000 + off), n, ir1_dt_area);
            if (next <= (ADDRX)(0x3000 + off)) {
                break;
            }
            off = (int)(next - 0x3000);
            n++;
        }
        ir1_sd.ir1 = ir1_list;
        ir1_sd.is_first_tb = 1;
        ir1_sd.next_pc = 0x3000 + off;
        smoke_tb.pc = 0x3000;
        smoke_tb.size = (uint16_t)off;
        smoke_tb.icount = (uint16_t)n;
        smoke_tb.tc.ptr = tb_buf;
        smoke_tb.s_data = &ir1_sd;

        rc = tr_translate_tb(&smoke_tb);
        latx_bare_uart_puts(" tb=");
        puthex((uint64_t)(int64_t)rc);
        latx_bare_uart_puts("\n");

        if (rc > 0) {
            int words = rc / 4;
            if (words > 40) {
                words = 40;
            }
            latx_bare_uart_puts("ir1-smoke: tb words:");
            for (int wi = 0; wi < words; wi++) {
                latx_bare_uart_puts(" ");
                puthex((uint64_t)((const uint32_t *)tb_buf)[wi]);
            }
            latx_bare_uart_puts("\n");
            if (rc >= 20) {
                uint32_t w3 = ((const uint32_t *)tb_buf)[3];
                uint32_t w4 = ((const uint32_t *)tb_buf)[4];
                int32_t off = (int32_t)((w4 >> 10) & 0xffff);
                int64_t imm18 = (int32_t)((w3 >> 5) & 0x3ffff);
                uint64_t t0, tgt;

                off = (off << 16) >> 16;
                imm18 = (imm18 << 14) >> 14;
                t0 = (uint64_t)(uintptr_t)tb_buf + 8 + ((uint64_t)imm18 << 18);
                tgt = t0 + (int64_t)off * 4;
                latx_bare_uart_puts("ir1-smoke: addr tb=");
                puthex((uint64_t)(uintptr_t)tb_buf);
                latx_bare_uart_puts(" epi=");
                puthex((uint64_t)(uintptr_t)epi_buf);
                latx_bare_uart_puts(" jmp_target=");
                puthex(tgt);
                latx_bare_uart_puts("\n");
            }
            /* The exit slot is normally patched by the TB linker.  Without a
             * run loop, rewrite it into a direct branch to the epilogue's
             * ret_0 entry (both live in the same code buffer). */
            if (rc >= 20) {
                uint32_t *w = (uint32_t *)tb_buf;
                uint64_t pc = (uint64_t)(uintptr_t)&w[2];
                int64_t off = ((int64_t)context_switch_native_to_bt_ret_0 -
                               (int64_t)pc) / 4;

                w[2] = 0x50000000u | (((uint32_t)off & 0xffffu) << 10);
                latx_bare_uart_puts("ir1-smoke: ret0=");
                puthex((uint64_t)context_switch_native_to_bt_ret_0);
                latx_bare_uart_puts(" off=");
                puthex((uint64_t)(int64_t)off);
                latx_bare_uart_puts(" w2=");
                puthex((uint64_t)w[2]);
                latx_bare_uart_puts("\n");
                w[3] = 0x03400000u;  /* nop */
                w[4] = 0x03400000u;  /* nop */
            }
            latx_bare_uart_puts("ir1-smoke: entering body\n");
            (void)latx_run_body(tb_buf, &env);
            latx_bare_uart_puts("ir1-smoke: body returned rax=");
            puthex(env.regs[R_EAX]);
            latx_bare_uart_puts(" rbx=");
            puthex(env.regs[R_EBX]);
            latx_bare_uart_puts(" eip=");
            puthex(env.eip);
            latx_bare_uart_puts("\n");
            check(env.regs[R_EAX] == 0x2223, "translated mov+lea result");
            check(env.regs[R_EBX] == 0x2222, "rbx preserved");
            check(env.eip == 0x3000, "exit pc is the branch target");
        }
    }
    latx_bare_uart_puts("ir1-smoke: summary checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails == 0 ? " ok\n" : " bad\n");
    latx_bare_uart_puts("ir1-smoke: done\n");
    return 0;
}
