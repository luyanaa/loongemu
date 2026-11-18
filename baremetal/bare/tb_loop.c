/*
 * Multi-TB dynamic execution loop (P1).
 *
 * The single-block smoke test built one hand-made TB and patched its exit
 * with a fixed jump.  This harness runs a real x86-64 program instead:
 *
 *   - guest code lives in a flat 64 KiB RAM image, and latx_bare_ldub_code()
 *     reads from it, so instruction fetch is bounded only by the image and
 *     crosses 4 KiB pages without special handling;
 *   - every guest PC gets its own TB from a translation cache keyed by
 *     (pc, cs_base, cr3, cpl), so a loop re-enters already-translated code
 *     and a CR3 write or an SMC store can invalidate it;
 *   - the loop translates on miss, runs the block, and reads the next PC out
 *     of the environment, which is what makes multi-block control flow --
 *     backward jumps, calls and returns -- work without hand-written links.
 *
 * The program under test computes 5*2 in a loop, calls a function, and then
 * spins; the assertions check the loop ran, the call returned, and the
 * translator produced more than one block.
 *
 * STATUS: the harness builds and translates, but execution does not return.
 * What the bring-up established, and what is still wrong:
 *
 *   - qemu_loglevel_mask() in latx/compat/qemu/log.h was hardcoded to return
 *     0, so CPU_LOG_TB_NOCHAIN could never take effect and every block exit
 *     went to a link the harness never built.  It now reads qemu_loglevel.
 *   - Entry uses the generated prologue/epilogue pair instead of the hand
 *     rolled frame, because that pair is a matched call/return: it saves the
 *     callee-saved set at fixed sp offsets, loads the guest registers, and
 *     returns through $ra.  The old harness saved the same registers at the
 *     same offsets but never loaded the guest state the block reads.
 *   - ir1_disasm() exits the process on `dec r32` (0xff /1) in this build:
 *     replacing `ff c9` with the equivalent `83 e9 01` moved the failure
 *     from decode to execution.  That is a decoder defect, independent of
 *     this harness.
 *   - With the prologue installed, the first block is entered at its code
 *     address and never returns; with a zeroed prologue the same block ran
 *     (its LBT instructions trapped) and returned.  So the block itself
 *     executes, and the remaining fault is in the entry/exit convention.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "ir1.h"
#include "ir2.h"
#include "translate.h"

#include "loongemu/tb_cache.h"
#include "lbt_emu.h"

size_t latx_bare_arena_used(void);
size_t latx_bare_arena_size(void);

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

/* ---------------- guest memory ---------------- */

#define GUEST_RAM_SIZE (64 * 1024)
#define GUEST_BASE     0x0
#define PROGRAM_PC     0x1000

static uint8_t guest_ram[GUEST_RAM_SIZE] __attribute__((aligned(4096)));

/*
 * Strong definition of the fetch hook the translator calls.  Reads are
 * per-byte, so an instruction straddling a page boundary needs no special
 * case; what matters is that the address resolves inside the image and that
 * an out-of-range fetch is visible rather than silent.
 */
static int g_bad_fetch;

uint8_t latx_bare_ldub_code(uint64_t vaddr)
{
    uint64_t off = vaddr - GUEST_BASE;

    if (off >= GUEST_RAM_SIZE) {
        g_bad_fetch++;
        return 0xcc;          /* int3: stop rather than execute garbage */
    }
    return guest_ram[off];
}

/* ---------------- translator state ---------------- */

static CPUX86State env;
static TCGContext tcg_storage;
static TRANSLATION_DATA tr;
static ENV lsenv_storage;
static IR1_INST ir1_list[16];
static struct separated_data ir1_sd;
static uint8_t ir1_dt_area[8192] __attribute__((aligned(16)));
static uint8_t jmp_cache[4096] __attribute__((aligned(16)));

#define CODE_CACHE_SIZE (256 * 1024)
static uint8_t code_cache[CODE_CACHE_SIZE] __attribute__((aligned(16)));
static uint8_t epi_buf[4096] __attribute__((aligned(16)));
static uint8_t pro_buf[4096] __attribute__((aligned(16)));

static uint64_t code_used;

#define TB_TABLE_CAP 64
static TbCacheEntry tb_entries[TB_TABLE_CAP];
static uint32_t tb_buckets[TB_TABLE_CAP];
static TbCache tb_cache;

unsigned char kstack[65536] __attribute__((aligned(16)));
void kmain(void);

__asm__(".text\n.globl _start\n_start:\n  la $sp, kstack\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");

/*
 * Entry into translated code is the generated prologue, not a hand-written
 * frame: prologue and epilogue are a matched pair that save the callee-saved
 * set at fixed sp offsets, load the guest registers from env, and return
 * through $ra.  With CPU_LOG_TB_NOCHAIN set the block's own exit jumps to the
 * epilogue instead of a link slot, so one call runs exactly one block and
 * comes back here.
 */
extern int latx_gen_prologue(void *code_buf);
typedef void (*TbEntry)(void *tb_code, void *env);

/* ---------------- guest program ---------------- */

/*
 *   mov  eax, 0
 *   mov  ecx, 5
 * L:add  eax, 2
 *   dec  ecx
 *   jnz  L
 *   call f
 *   mov  ebx, eax
 * H:jmp  H
 * f:add  eax, 1
 *   ret
 */
static const uint8_t program[] = {
    0x48, 0x89, 0xd8,                         /* mov rax, rbx (probe) */
    0x31, 0xc0,                               /* xor eax, eax   */
    0xb9, 0x05, 0x00, 0x00, 0x00,             /* mov ecx, 5     */
    0x83, 0xc0, 0x02,                         /* add eax, 2     */
    0x83, 0xe9, 0x01,                         /* sub ecx, 1     */
    0x75, 0xf9,                               /* jnz -7         */
    0xe8, 0x0a, 0x00, 0x00, 0x00,             /* call f         */
    0x89, 0xc3,                               /* mov ebx, eax   */
    0xeb, 0xfe,                               /* jmp $          */
    0x83, 0xc0, 0x01,                         /* add eax, 1     */
    0xc3,                                     /* ret            */
};

/* Byte offsets in program[]: the self-jump is at +0x18, the function at +0x1a. */
#define HALT_PC (PROGRAM_PC + 0x1b)
#define FUNC_PC (PROGRAM_PC + 0x1d)

/* ---------------- per-PC translation ---------------- */

static const int control_flow_ops[] = {
    dt_X86_INS_JMP, dt_X86_INS_CALL, dt_X86_INS_RET, dt_X86_INS_JAE,
    dt_X86_INS_JA, dt_X86_INS_JBE, dt_X86_INS_JB, dt_X86_INS_JCXZ,
    dt_X86_INS_JECXZ, dt_X86_INS_JE, dt_X86_INS_JGE, dt_X86_INS_JG,
    dt_X86_INS_JLE, dt_X86_INS_JL, dt_X86_INS_JNE, dt_X86_INS_JNO,
    dt_X86_INS_JNP, dt_X86_INS_JNS, dt_X86_INS_JO, dt_X86_INS_JP,
    dt_X86_INS_JS,
};

static int is_control_flow(const IR1_INST *ir1)
{
    int op = ir1_opcode(ir1);

    for (unsigned i = 0; i < sizeof(control_flow_ops) / sizeof(int); i++) {
        if (op == control_flow_ops[i]) {
            return 1;
        }
    }
    return 0;
}

/*
 * Translate the block starting at pc into the code cache and return the
 * number of bytes emitted.  No exit patch is needed: with NOCHAIN set the
 * translator itself emits the jump to the epilogue, which is the entry the
 * harness calls through.
 */
static int translate_at(uint64_t pc, uint8_t *code, uint64_t limit)
{
    TranslationBlock tb;
    int off = 0, n = 0;
    int rc;

    memset(&tb, 0, sizeof(tb));
    memset(ir1_list, 0, sizeof(ir1_list));
    memset(&ir1_sd, 0, sizeof(ir1_sd));

    {
        /*
         * Decode from a local 32-byte copy, matching what the single-block
         * harness does: ir1_disasm copies a maximum instruction (15 bytes)
         * plus lookahead, and handing it a pointer into the guest image
         * makes the bounds of that read depend on where in the image the
         * block sits.
         */
        uint8_t insn_bytes[32] = { 0 };

        for (unsigned b = 0; b < sizeof(insn_bytes); b++) {
            insn_bytes[b] = latx_bare_ldub_code(pc + b);
        }

    while (off < 32 && n < 8) {
        ADDRX next = ir1_disasm(&ir1_list[n], insn_bytes + off,
                                (ADDRX)(pc + off), n, ir1_dt_area);

        if (next <= (ADDRX)(pc + off)) {
            break;
        }
        off = (int)(next - pc);
        n++;
        if (is_control_flow(&ir1_list[n - 1])) {
            break;
        }
    }
    }
    if (n == 0) {
        return -1;
    }

    ir1_sd.ir1 = ir1_list;
    ir1_sd.is_first_tb = 1;
    ir1_sd.next_pc = pc + off;
    tb.pc = pc;
    tb.size = (uint16_t)off;
    tb.icount = (uint16_t)n;
    tb.tc.ptr = code;
    tb.s_data = &ir1_sd;

    rc = tr_translate_tb(&tb);
    if (rc <= 0 || (uint64_t)rc > limit) {
        return -1;
    }
    return rc;
}

void kmain(void)
{
    uint64_t pc;
    int steps = 0;
    int translated = 0;
    uint32_t hflags;

    latx_bare_uart_puts("tb-loop: start\n");

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

    memcpy(guest_ram + (PROGRAM_PC - GUEST_BASE), program, sizeof(program));

    memset(&env, 0, sizeof(env));
    memset(&tr, 0, sizeof(tr));
    memset(&lsenv_storage, 0, sizeof(lsenv_storage));
    memset(&tcg_storage, 0, sizeof(tcg_storage));
    memset(jmp_cache, 0, sizeof(jmp_cache));
    memset(code_cache, 0, sizeof(code_cache));
    memset(epi_buf, 0, sizeof(epi_buf));

    env.sys.codemode = 1;
    env.tb_jmp_cache_ptr = jmp_cache;
    env.eip = PROGRAM_PC;
    env.cr[3] = 0;                 /* one address space for this program */
    lsenv_storage.cpu_state = &env;
    lsenv_storage.tr_data = &tr;
    lsenv = &lsenv_storage;

    tcg_storage.code_gen_buffer = code_cache;
    tcg_storage.code_gen_buffer_size = CODE_CACHE_SIZE;
    tcg_storage.code_gen_ptr = code_cache;
    tcg_storage.code_gen_highwater = code_cache + CODE_CACHE_SIZE;
    tcg_ctx = &tcg_storage;
    tcg_storage.gen_insn_data = latx_tcg_gen_insn_data;


    latx_runtime_init();

    latx_gen_epilogue(epi_buf);
    latx_gen_prologue(pro_buf);
    /* Make every block exit to the epilogue rather than a link slot. */
    qemu_loglevel |= CPU_LOG_TB_NOCHAIN;


    check(tbc_init(&tb_cache, tb_entries, tb_buckets, TB_TABLE_CAP) == 1,
          "tb cache init");

    /*
     * The run loop.  Each iteration is one translated block: look it up by
     * the full key, translate on a miss, execute, then take the next PC from
     * the environment.
     */
    for (steps = 0; steps < 128; steps++) {
        TbCacheEntry *e;
        uint8_t *code;
        int rc;

        hflags = env.hflags;
        pc = env.eip;
        if (pc == HALT_PC) {
            break;
        }

        e = tbc_lookup(&tb_cache, pc, 0,
                       env.cr[3], (uint8_t)(hflags & HF_CPL_MASK));
        if (e == 0) {
            e = tbc_insert(&tb_cache, pc, 0, env.cr[3],
                           (uint8_t)(hflags & HF_CPL_MASK));
            if (e == 0) {
                check(0, "tb cache full");
                break;
            }
            code = code_cache + code_used;
            rc = translate_at(pc, code, CODE_CACHE_SIZE - code_used);
            if (rc < 0) {
                latx_bare_uart_puts("tb-loop: translate failed at ");
                puthex(pc);
                latx_bare_uart_puts("\n");
                check(0, "translate");
                break;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = (uint32_t)rc;
            code_used += ((uint64_t)rc + 15) & ~UINT64_C(15);
            translated++;
        }

        ((TbEntry)(void *)(uintptr_t)pro_buf)((void *)(uintptr_t)e->code,
                                              &env);

    }

    latx_bare_uart_puts("tb-loop: steps=");
    puthex((uint64_t)steps);
    latx_bare_uart_puts(" translated=");
    puthex((uint64_t)translated);
    latx_bare_uart_puts(" cached=");
    puthex((uint64_t)tbc_count(&tb_cache));
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-loop: eax=");
    puthex(env.regs[R_EAX]);
    latx_bare_uart_puts(" ebx=");
    puthex(env.regs[R_EBX]);
    latx_bare_uart_puts(" ecx=");
    puthex(env.regs[R_ECX]);
    latx_bare_uart_puts(" eip=");
    puthex(env.eip);
    latx_bare_uart_puts("\n");

    /* 5 iterations of +2, then the call adds 1. */
    check(env.regs[R_EAX] == 11, "loop + call result");
    check(env.regs[R_EBX] == 11, "return value stored");
    check(env.regs[R_ECX] == 0, "loop counter drained");
    check(env.eip == HALT_PC, "reached the halt block");
    check(translated >= 3, "more than one block was translated");
    check(g_bad_fetch == 0, "no out-of-image fetch");

    latx_bare_uart_puts("tb-loop: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("tb-loop: done\n");
    for (;;) {
    }
}
