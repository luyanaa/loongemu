/*
 * Long-mode bring-up test (P1).
 *
 * The guest is a 64-bit snippet that does what a firmware or a kernel does
 * before it can run anything else, and the harness checks the resulting
 * machine state rather than trusting the guest to report:
 *
 *   wrmsr  0xc0000080 (EFER) = LME     -> env->efer
 *   rdmsr  0xc0000080                  -> ebx (round trip through the helper)
 *   lgdt   [0x1200]                    -> env->gdt.base / limit
 *   4 stores                           -> a PML4/PDPT/PD/PT set in guest RAM
 *   mov    cr3, 0x2000                 -> env->cr[3], read back into r12d
 *   mov    cr0, PE|PG                  -> env->cr[0], LMA follows LME+PG
 *   in     0x3fd                       -> ebp (line status, transmitter empty)
 *   loop   over a NUL-terminated string -> every byte out port 0x3f8
 *
 * The last part is the acceptance criterion: the banner arrives complete,
 * which means a backward branch, a data load, a port write and the halt all
 * survive translation in long mode.
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

#include "latx-options.h"
#include "latx-x86-paging.h"

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

static uint8_t uart_tx[64];
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
#define GDT_PC         0x1200
#define STR_PC         0x6000
#define STORE_PC       0x1500

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
 * Offsets below are relative to PROGRAM_PC.  Three encoding details matter
 * here: EFER.LME is bit 8 (the value written is 0x100, not 1), `mov r32,
 * imm32` is always five bytes -- there is no imm8 form, so a short encoding
 * would silently swallow the next instruction -- and `lgdt [disp32]` in
 * 64-bit mode must go through SIB (modrm 0x14, SIB 0x25); with modrm 0x15 the
 * operand is RIP-relative.
 *
 * The three stores build a valid 4-level table set: PML4[0] -> PDPT[0] -> PD,
 * with PD[0] a 2 MiB leaf.  It starts read-only (0x81) and CR0.WP is set, so
 * the store at +0x6b takes a real #PF; the driver acts as the guest's kernel,
 * makes the leaf writable and resumes at the store.
 *
 *    *   +0x00: b9 80 00 00 c0                       mov  ecx, 0xc0000080     ; EFER
 *   +0x05: b8 00 01 00 00                       mov  eax, 0x100          ; LME
 *   +0x0a: 31 d2                                xor  edx, edx
 *   +0x0c: 0f 30                                wrmsr
 *   +0x0e: 31 c0                                xor  eax, eax
 *   +0x10: 31 d2                                xor  edx, edx
 *   +0x12: 0f 32                                rdmsr
 *   +0x14: 89 c3                                mov  ebx, eax            ; EFER as read back
 *   +0x16: 0f 01 14 25 00 12 00 00              lgdt [0x1200]
 *   +0x1e: 48 c7 04 25 00 20 00 00 03 30 00 00  mov  qword [0x2000], 0x3003  ; PML4[0] -> PDPT
 *   +0x2a: 48 c7 04 25 00 30 00 00 03 40 00 00  mov  qword [0x3000], 0x4003  ; PDPT[0] -> PD
 *   +0x36: 48 c7 04 25 00 40 00 00 81 00 00 00  mov  qword [0x4000], 0x81    ; PD[0]: 2 MiB, read-only
 *   +0x42: b8 20 00 00 00                       mov  eax, 0x20           ; PAE
 *   +0x47: 0f 22 e0                             mov  cr4, eax
 *   +0x4a: 0f 20 e0                             mov  eax, cr4
 *   +0x4d: 41 89 c5                             mov  r13d, eax           ; CR4 as read back
 *   +0x50: b8 00 20 00 00                       mov  eax, 0x2000
 *   +0x55: 0f 22 d8                             mov  cr3, eax
 *   +0x58: 0f 20 d8                             mov  eax, cr3
 *   +0x5b: 41 89 c4                             mov  r12d, eax           ; CR3 as read back
 *   +0x5e: b8 01 00 01 80                       mov  eax, 0x80010001     ; PG | WP | PE
 *   +0x63: 0f 22 c0                             mov  cr0, eax
 *   +0x66: 0f 20 c0                             mov  eax, cr0
 *   +0x69: 89 c7                                mov  edi, eax            ; CR0 as read back
 *   +0x6b: c6 04 25 00 15 00 00 01              mov  byte [0x1500], 1    ; #PF: leaf is read-only
 *   +0x73: ba fd 03 00 00                       mov  edx, 0x3fd
 *   +0x78: ec                                   in   al, dx
 *   +0x79: 0f b6 c0                             movzx eax, al
 *   +0x7c: 89 c5                                mov  ebp, eax            ; LSR as read back
 *   +0x7e: be 00 00 00 00                       mov  esi, <str>          ; patched to 0x6000
 *   +0x83: 8a 06                                mov  al, [rsi]
 *   +0x85: 84 c0                                test al, al
 *   +0x87: 74 0c                                jz   +0x0c
 *   +0x89: ba f8 03 00 00                       mov  edx, 0x3f8
 *   +0x8e: ee                                   out  dx, al
 *   +0x8f: 48 ff c6                             inc  rsi
 *   +0x92: eb ef                                jmp  -0x11
 *   +0x94: 90                                   nop
 *   +0x95: eb fe                                jmp  $
 */
static const uint8_t program[] = {
    0xb9, 0x80, 0x00, 0x00, 0xc0, 0xb8, 0x00, 0x01,
    0x00, 0x00, 0x31, 0xd2, 0x0f, 0x30, 0x31, 0xc0,
    0x31, 0xd2, 0x0f, 0x32, 0x89, 0xc3, 0x0f, 0x01,
    0x14, 0x25, 0x00, 0x12, 0x00, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x00, 0x20, 0x00, 0x00, 0x03, 0x30,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x00, 0x30,
    0x00, 0x00, 0x03, 0x40, 0x00, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x00, 0x40, 0x00, 0x00, 0x81, 0x00,
    0x00, 0x00, 0xb8, 0x20, 0x00, 0x00, 0x00, 0x0f,
    0x22, 0xe0, 0x0f, 0x20, 0xe0, 0x41, 0x89, 0xc5,
    0xb8, 0x00, 0x20, 0x00, 0x00, 0x0f, 0x22, 0xd8,
    0x0f, 0x20, 0xd8, 0x41, 0x89, 0xc4, 0xb8, 0x01,
    0x00, 0x01, 0x80, 0x0f, 0x22, 0xc0, 0x0f, 0x20,
    0xc0, 0x89, 0xc7, 0xc6, 0x04, 0x25, 0x00, 0x15,
    0x00, 0x00, 0x01, 0xba, 0xfd, 0x03, 0x00, 0x00,
    0xec, 0x0f, 0xb6, 0xc0, 0x89, 0xc5, 0xbe, 0x00,
    0x00, 0x00, 0x00, 0x8a, 0x06, 0x84, 0xc0, 0x74,
    0x0c, 0xba, 0xf8, 0x03, 0x00, 0x00, 0xee, 0x48,
    0xff, 0xc6, 0xeb, 0xef, 0x90, 0xeb, 0xfe,
};

/* NUL terminated; the guest walks it and writes it out port by port. */
static const char banner[] = "loongemu: long mode\n";

static const uint8_t gdt_descriptor[] = {
    0x1f, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define HALT_PC (PROGRAM_PC + 0x95)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

void kmain(void)
{
    uint64_t pc;
    int steps = 0;
    int translated = 0;
    int faults = 0;
    uint64_t fault_cr2 = 0;
    uint32_t fault_ec = 0;
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

    memcpy(guest_ram + (PROGRAM_PC - GUEST_BASE), program, sizeof(program));

    /*
     * With the page-table check enabled, everything the guest reads must be
     * inside the range its own tables map -- host addresses below 2 MiB, the
     * 2 MiB leaf the guest builds.  So the descriptor and the banner are
     * placed in that range at the addresses the program names, instead of in
     * this image's .bss.
     */
    memcpy((void *)(uintptr_t)GDT_PC, gdt_descriptor, sizeof(gdt_descriptor));
    memcpy((void *)(uintptr_t)STR_PC, banner, sizeof(banner));
    {
        uint32_t str_addr = STR_PC;

        memcpy(guest_ram + (PROGRAM_PC - GUEST_BASE) + 0x7f, &str_addr,
               sizeof(str_addr));
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
    /* Consult the guest's page tables for every data access. */
    option_guest_paging = 1;

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

        if (env.exception_index != 0) {
            faults++;
            fault_cr2 = env.cr[2];
            fault_ec = (uint32_t)env.error_code;
            latx_bare_uart_puts("  #PF at ");
            puthex(pc);
            latx_bare_uart_puts(" cr2=");
            puthex(fault_cr2);
            latx_bare_uart_puts(" ec=");
            puthex(fault_ec);
            latx_bare_uart_puts("\n");

            /*
             * Act as the guest's kernel: the store hit a read-only leaf, so
             * make it writable, invalidate and resume at the instruction the
             * fault stub left in eip.  Anything else is a harness failure.
             */
            if (env.exception_index != EXCP0E_PAGE || fault_cr2 != STORE_PC ||
                faults != 1) {
                check(0, "unexpected guest fault");
                break;
            }
            *(volatile uint64_t *)0x4000 = 0x83;
            helper_invlpg(&env, STORE_PC);
            env.exception_index = 0;
        }
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
    check(env.regs[R_R12] == 0x2000, "mov r, cr3 reads it back");
    /* mov cr0 plus the read back */
    check(env.cr[0] == 0x80010001u, "mov cr0 stored (PG|WP|PE)");
    check(env.regs[R_EDI] == 0x80010001u, "mov r, cr0 reads it back");
    /* the fault the checked store took, and the resume afterwards */
    check(faults == 1, "read-only leaf raised exactly one #PF");
    check(fault_cr2 == STORE_PC, "CR2 holds the faulting store address");
    check(fault_ec == 0x3u, "error code is P|W (supervisor write)");
    check(*(volatile uint8_t *)(uintptr_t)STORE_PC == 1,
          "the store executed after the PTE was fixed");
    /*
     * The same tables, translated by the runtime's walker: 2 MiB leaf at the
     * PD level, identity mapped, writable, supervisor only.
     */
    {
        LatxX86Translation t;
        LatxX86WalkResult r;

        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1000 && t.page_bits == 21,
              "walk maps 0x1000 with a 2 MiB leaf");
        check((t.perm & LATX_X86_ACCESS_WRITE) != 0, "walk reports writable");

        r = latx_x86_walk(&env, 0x1ff123, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1ff123,
              "walk keeps the offset inside the leaf");

        r = latx_x86_walk(&env, 0x200000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_NOT_PRESENT, "walk faults past the leaf");

        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER,
                          &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(&env, r,
                                        LATX_X86_ACCESS_READ | LATX_X86_ACCESS_USER) == 0x5,
              "supervisor leaf faults for a user access (U/S error)");

        /*
         * Read-only leaf.  CR0.WP decides whether a supervisor write faults,
         * so both sides of that switch are checked; a user write always
         * faults.  The guest leaves WP set, so clear it for the first case
         * and restore the guest's value at the end.
         */
        uint64_t saved_cr0 = env.cr[0];

        env.cr[0] &= ~(uint64_t)CR0_WP_MASK;
        *(volatile uint64_t *)0x4000 = 0x81;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK,
              "WP clear: supervisor may write a read-only leaf");
        env.cr[0] |= CR0_WP_MASK;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(&env, r, LATX_X86_ACCESS_WRITE) == 0x3,
              "WP set: supervisor write faults (P|W error)");
        r = latx_x86_walk(&env, 0x1000,
                          LATX_X86_ACCESS_WRITE | LATX_X86_ACCESS_USER, &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(
                  &env, r, LATX_X86_ACCESS_WRITE | LATX_X86_ACCESS_USER) == 0x7,
              "user write to supervisor leaf faults (P|W|U error)");
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "read-only leaf still readable");
        env.cr[0] = saved_cr0;

        /* NX on the leaf, with EFER.NXE enabled, blocks fetch only. */
        env.efer |= MSR_EFER_NXE;
        *(volatile uint64_t *)0x4000 = 0x8000000000000083ull;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_EXEC, &t);
        check(r == LATX_X86_WALK_PROTECTION &&
              latx_x86_fault_error_code(&env, r, LATX_X86_ACCESS_EXEC) == 0x11,
              "NX leaf faults on fetch (P|I-D error)");
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "NX leaf still readable");

        /*
         * Cached walks.  The cache must agree with the walker, must go stale
         * when only the PTE changes (that is the guest's job to invalidate),
         * and must be dropped by INVLPG and by a CR3 write.
         */
        latx_x86_tlb_flush_all();
        *(volatile uint64_t *)0x4000 = 0x81;      /* read-only leaf */
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK && t.gpa == 0x1000, "cached walk fills");
        env.cr[0] |= CR0_WP_MASK;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION,
              "cached leaf permissions are re-checked without a walk");
        /* Back to writable in the table, but the cache still says read-only. */
        *(volatile uint64_t *)0x4000 = 0x83;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_PROTECTION, "no invalidation keeps the old leaf");
        /* INVLPG goes through helper_invlpg -> tlb_flush_page. */
        helper_invlpg(&env, 0x1000);
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK, "INVLPG drops the stale leaf");
        env.cr[0] &= ~(uint64_t)CR0_WP_MASK;
        /* A CR3 write drops everything, as a real address-space switch must. */
        *(volatile uint64_t *)0x4000 = 0x81;
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_OK, "cache refilled after INVLPG");
        *(volatile uint64_t *)0x4000 = 0x83;
        cpu_x86_update_cr3(&env, env.cr[3]);
        r = latx_x86_walk_cached(&env, 0x1000, LATX_X86_ACCESS_WRITE, &t);
        check(r == LATX_X86_WALK_OK, "CR3 write drops the cache");

        /* PS at the PML4 level is a reserved encoding, not a 512 GiB page. */
        *(volatile uint64_t *)0x4000 = 0x83;
        *(volatile uint64_t *)0x2000 = 0x3003 | 0x80;
        r = latx_x86_walk(&env, 0x1000, LATX_X86_ACCESS_READ, &t);
        check(r == LATX_X86_WALK_RESERVED, "PS at the PML4 level is reserved");
        *(volatile uint64_t *)0x2000 = 0x3003;
    }

    /* the 4-level tables the guest built, read back through the data path */
    check(*(volatile uint64_t *)0x2000 == 0x3003u, "PML4 entry");
    check(*(volatile uint64_t *)0x3000 == 0x4003u, "PDPT entry");
    check(*(volatile uint64_t *)0x4000 == 0x83u, "PD entry (2 MiB leaf)");
    /* every byte of the banner came out of the guest's loop */
    check(uart_tx_count == (int)sizeof(banner) - 1 &&
          memcmp(uart_tx, banner, sizeof(banner) - 1) == 0,
          "out 0x3f8 printed the banner");
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
