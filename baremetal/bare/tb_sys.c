/*
 * System-instruction smoke test (P1).
 *
 * The guest program is a 64-bit snippet that performs the moves a firmware or
 * a kernel does before it can run anything else, and the harness checks the
 * resulting machine state instead of output:
 *
 *   wrmsr  0xc0000080 (EFER) = LME     -> env->efer
 *   rdmsr  0xc0000080                  -> ebx (round trip through helper)
 *   lgdt   [0x1200]                    -> env->gdt.base / limit
 *   mov    cr3, 0x2000                 -> env->cr[3] and a read back
 *   mov    cr0, PE|PG                  -> env->cr[0], LMA follows LME+PG
 *   out    0x3f8, 'K' / in 0x3fd       -> the platform port hooks
 *
 * The port hooks are the weak latx_platform_io_read/write symbols the
 * translator's address_space_io plumbing calls, so this also proves that the
 * IN/OUT translation reaches a device.  The device here is the 16550 subset
 * a serial console needs: a transmit register on 0x3f8 and a line status
 * register on 0x3fd that always reports "transmitter empty".
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
#include "tb_harness.h"

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

/* ---------------- device layer: a 16550 subset ---------------- */

#define COM1_THR 0x3f8
#define COM1_LSR 0x3fd
#define LSR_THRE_TEMT 0x60

static uint8_t uart_tx[8];
static int uart_tx_count;

uint64_t latx_platform_io_read(uint32_t port, int size)
{
    (void)size;
    if (port == COM1_LSR) {
        return LSR_THRE_TEMT;
    }
    if (port == COM1_THR) {
        return 0;                    /* no receiver attached */
    }
    return 0xffffffffull;
}

void latx_platform_io_write(uint32_t port, int size, uint64_t value)
{
    (void)size;
    if (port == COM1_THR) {
        if (uart_tx_count < (int)sizeof(uart_tx)) {
            uart_tx[uart_tx_count] = (uint8_t)value;
        }
        uart_tx_count++;
        latx_bare_uart_putc((int)(uint8_t)value);
    }
}

/* ---------------- guest memory ---------------- */

/*
 * Instruction fetch goes through latx_bare_ldub_code() and lands in this
 * array.  The translator's *data* path, on the other hand, uses the guest
 * virtual address as a host address -- that is what "direct mapped" means
 * here -- so any operand the guest reads must live at an address the guest
 * can name, i.e. at its real host address.  The GDT descriptor below is the
 * one such operand in this test, and the lgdt displacement in the guest code
 * is patched at run time to point at it.
 */
#define GUEST_RAM_SIZE (64 * 1024)
#define GUEST_BASE     0x0
#define PROGRAM_PC     0x0ffe

static uint8_t guest_ram[GUEST_RAM_SIZE] __attribute__((aligned(4096)));

static int g_bad_fetch;

uint8_t latx_bare_ldub_code(uint64_t vaddr)
{
    uint64_t off = vaddr - GUEST_BASE;

    if (off >= GUEST_RAM_SIZE) {
        g_bad_fetch++;
        return 0xcc;
    }
    return guest_ram[off];
}

/* ---------------- translator state ---------------- */

static CPUX86State env;
static TCGContext tcg_storage;
static TRANSLATION_DATA tr;
static ENV lsenv_storage;
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

extern int latx_gen_prologue(void *code_buf);
typedef void (*TbEntry)(void *tb_code, void *env);

/* ---------------- guest program ---------------- */

/*
 * PROGRAM_PC is deliberately not page aligned: the first instruction starts
 * at 0x0ffe, so its immediate straddles the 4 KiB boundary at 0x1000 and
 * instruction fetch has to cross a page to decode it.
 *
 * Offsets below are relative to PROGRAM_PC.  Two encoding details matter
 * here: EFER.LME is bit 8 (the value written is 0x100, not 1), and
 * `mov r32, imm32` is always five bytes -- there is no imm8 form, so a short
 * encoding would silently swallow the next instruction.
 *
 * lgdt [disp32] in 64-bit mode must go through SIB: modrm 0x14 (mod=00,
 * reg=/2, rm=100) with SIB 0x25 (no index, base=101 -> disp32), i.e.
 * 0f 01 14 25 <disp32>.  With modrm 0x15 the operand is RIP-relative.
 *
 *   +0x00: b9 80 00 00 c0        mov  ecx, 0xc0000080     ; EFER
 *   +0x05: b8 00 01 00 00        mov  eax, 0x100          ; LME
 *   +0x0a: 31 d2                 xor  edx, edx
 *   +0x0c: 0f 30                 wrmsr
 *   +0x0e: 31 c0                 xor  eax, eax
 *   +0x10: 31 d2                 xor  edx, edx
 *   +0x12: 0f 32                 rdmsr
 *   +0x14: 89 c3                 mov  ebx, eax            ; EFER as read back
 *   +0x16: 0f 01 14 25 00 12 00 00  lgdt [0x1200]
 *   +0x1e: b8 00 20 00 00        mov  eax, 0x2000
 *   +0x23: 0f 22 d8              mov  cr3, eax
 *   +0x26: 0f 20 d8              mov  eax, cr3
 *   +0x29: 89 c6                 mov  esi, eax
 *   +0x2b: b8 01 00 00 80        mov  eax, 0x80000001     ; PG | PE
 *   +0x30: 0f 22 c0              mov  cr0, eax
 *   +0x33: 0f 20 c0              mov  eax, cr0
 *   +0x36: 89 c7                 mov  edi, eax
 *   +0x38: ba f8 03 00 00        mov  edx, 0x3f8
 *   +0x3d: b0 4b                 mov  al, 'K'
 *   +0x3f: ee                    out  dx, al
 *   +0x40: ba fd 03 00 00        mov  edx, 0x3fd
 *   +0x45: ec                    in   al, dx
 *   +0x46: 0f b6 c0              movzx eax, al
 *   +0x49: 89 c5                 mov  ebp, eax
 *   +0x4b: eb fe                 jmp  +0x4b
 */
static const uint8_t program[] = {
    0xb9, 0x80, 0x00, 0x00, 0xc0,
    0xb8, 0x00, 0x01, 0x00, 0x00,
    0x31, 0xd2,
    0x0f, 0x30,
    0x31, 0xc0,
    0x31, 0xd2,
    0x0f, 0x32,
    0x89, 0xc3,
    0x0f, 0x01, 0x14, 0x25, 0x00, 0x12, 0x00, 0x00,
    0xb8, 0x00, 0x20, 0x00, 0x00,
    0x0f, 0x22, 0xd8,
    0x0f, 0x20, 0xd8,
    0x89, 0xc6,
    0xb8, 0x01, 0x00, 0x00, 0x80,
    0x0f, 0x22, 0xc0,
    0x0f, 0x20, 0xc0,
    0x89, 0xc7,
    0xba, 0xf8, 0x03, 0x00, 0x00,
    0xb0, 0x4b,
    0xee,
    0xba, 0xfd, 0x03, 0x00, 0x00,
    0xec,
    0x0f, 0xb6, 0xc0,
    0x89, 0xc5,
    0xeb, 0xfe,
};

/* limit 0x1f, base 0x1000 -- the shape a 64-bit lgdt operand must have. */
static uint8_t gdt_image[16] __attribute__((aligned(16)));

static const uint8_t gdt_descriptor[] = {
    0x1f, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define HALT_PC (PROGRAM_PC + 0x4b)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

void kmain(void)
{
    uint64_t pc;
    int steps = 0;
    int translated = 0;
    uint32_t hflags;

    latx_bare_uart_puts("tb-sys: start\n");

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

    memset(gdt_image, 0, sizeof(gdt_image));
    memcpy(gdt_image, gdt_descriptor, sizeof(gdt_descriptor));
    memcpy(guest_ram + (PROGRAM_PC - GUEST_BASE), program, sizeof(program));

    /*
     * lgdt m16&64 names its operand with an absolute disp32 at
     * program offset 0x1a; point it at the descriptor image's host address,
     * which is the only address the direct-mapped data path can reach.
     */
    {
        uint32_t desc_addr = (uint32_t)(uintptr_t)gdt_image;

        memcpy(guest_ram + (PROGRAM_PC - GUEST_BASE) + 0x1a, &desc_addr,
               sizeof(desc_addr));
    }

    memset(&env, 0, sizeof(env));
    memset(&tr, 0, sizeof(tr));
    memset(&lsenv_storage, 0, sizeof(lsenv_storage));
    memset(&tcg_storage, 0, sizeof(tcg_storage));
    memset(jmp_cache, 0, sizeof(jmp_cache));
    memset(code_cache, 0, sizeof(code_cache));
    memset(epi_buf, 0, sizeof(epi_buf));

    env.sys.codemode = 1;
    env.hflags = HF_LMA_MASK | HF_CS64_MASK;
    /*
     * Start in protected mode without paging and with EFER clear, so the
     * guest's own wrmsr/mov cr0 are what put the CPU into long mode and the
     * checks below observe transitions rather than pre-set state.
     */
    env.cr[0] = 0x1;                       /* PE only */
    env.eip = PROGRAM_PC;
    env.regs[R_ESP] = 0x8000;
    env.cr[3] = 0;
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
    /* EFER.LME is only writable when CPUID advertises long mode. */
    latx_x86_env_init_features(&env);

    latx_gen_epilogue(epi_buf);
    latx_gen_prologue(pro_buf);
    qemu_loglevel |= CPU_LOG_TB_NOCHAIN;
    check(tbc_init(&tb_cache, tb_entries, tb_buckets, TB_TABLE_CAP) == 1,
          "tb cache init");

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
            rc = tb_harness_translate_at(pc, code, CODE_CACHE_SIZE - code_used);
            if (rc < 0) {
                latx_bare_uart_puts("tb-sys: translate failed at ");
                puthex(pc);
                latx_bare_uart_puts("\n");
                check(0, "translate");
                break;
            }
            e->code = (uint64_t)(uintptr_t)code;
            e->code_size = (uint32_t)rc;
            code_used += (((uint64_t)rc + 255) & ~UINT64_C(255));
            translated++;
        }

        ((TbEntry)(void *)(uintptr_t)pro_buf)((void *)(uintptr_t)e->code,
                                              &env);
    }

    latx_bare_uart_puts("tb-sys: steps=");
    puthex((uint64_t)steps);
    latx_bare_uart_puts(" translated=");
    puthex((uint64_t)translated);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: efer=");
    puthex(env.efer);
    latx_bare_uart_puts(" cr0=");
    puthex(env.cr[0]);
    latx_bare_uart_puts(" cr3=");
    puthex(env.cr[3]);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: ebx=");
    puthex(env.regs[R_EBX]);
    latx_bare_uart_puts(" esi=");
    puthex(env.regs[R_ESI]);
    latx_bare_uart_puts(" edi=");
    puthex(env.regs[R_EDI]);
    latx_bare_uart_puts(" ebp=");
    puthex(env.regs[R_EBP]);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: gdt.base=");
    puthex(env.gdt.base);
    latx_bare_uart_puts(" gdt.limit=");
    puthex(env.gdt.limit);
    latx_bare_uart_puts("\n");

    /* wrmsr EFER then mov cr0: LME stays, LMA appears once PG is on. */
    check((env.efer & EFER_LME) != 0, "LME set by wrmsr");
    check((env.efer & EFER_LMA) != 0, "LMA set by mov cr0 with PG");
    check(env.regs[R_EBX] == EFER_LME, "rdmsr reads EFER back");
    /* lgdt */
    check(env.gdt.base == 0x1000, "lgdt base");
    check(env.gdt.limit == 0x1f, "lgdt limit");
    /* mov cr3 plus the read back */
    check(env.cr[3] == 0x2000, "mov cr3 stored");
    check(env.regs[R_ESI] == 0x2000, "mov r, cr3 reads it back");
    /* mov cr0 plus the read back */
    check(env.cr[0] == 0x80000001u, "mov cr0 stored");
    check(env.regs[R_EDI] == 0x80000001u, "mov r, cr0 reads it back");
    /* port I/O reached the device layer */
    check(uart_tx_count == 1 && uart_tx[0] == 'K', "out 0x3f8 sent 'K'");
    check(env.regs[R_EBP] == LSR_THRE_TEMT, "in 0x3fd read the LSR");
    check(env.eip == HALT_PC, "reached the halt block");
    check(g_bad_fetch == 0, "no out-of-image fetch");

    latx_bare_uart_puts("tb-sys: checks=");
    puthex((uint64_t)checks);
    latx_bare_uart_puts(" fails=");
    puthex((uint64_t)fails);
    latx_bare_uart_puts(fails ? " BAD\n" : " ok\n");
    latx_bare_uart_puts("tb-sys: done\n");
    for (;;) {
    }
}
