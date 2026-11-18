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
#define ALIAS_VA       0x202000u
#define ALIAS_PA       0x100000
#define RO_PC          0x201000u
#define STORE_PC       (PROGRAM_PC + 0xc3)   /* the faulting store */
#define IDT_DESC_PC    0x1400
#define IDT_TABLE_PC   0x14e0

static int g_bad_fetch;

/*
 * Guest physical memory is the machine's low RAM, read and written through
 * this hook so tables, code and data all live in one place: the image is
 * copied into the machine's low RAM at physical address 0, which is what makes
 * a page-table walk and a translated store refer to the same bytes.
 */
uint64_t latx_platform_phys_read(uint64_t pa, int size)
{
    uint64_t value = 0;

    for (int i = size - 1; i >= 0; i--) {
        value <<= 8;
        value |= *(volatile uint8_t *)(uintptr_t)(pa + (uint64_t)i);
    }
    return value;
}

void latx_platform_phys_write(uint64_t pa, int size, uint64_t value)
{
    for (int i = 0; i < size; i++) {
        *(volatile uint8_t *)(uintptr_t)(pa + (uint64_t)i) =
            (uint8_t)(value >> (8 * i));
    }
}

/*
 * The physical fetch the walker ends up calling once paging is on.  It reads
 * the same memory the guest writes, because guest physical memory *is* the
 * machine's low RAM here.
 */
uint8_t latx_bare_ldub_code(uint64_t vaddr)
{
    if (vaddr >= GUEST_RAM_SIZE) {
        g_bad_fetch++;
    }
    return *(volatile uint8_t *)(uintptr_t)vaddr;
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
 * The tables this guest builds:
 *
 *   PML4[0] -> PDPT @0x3000
 *   PDPT[0] -> PD   @0x4000
 *   PD[0] = 0x83              2 MiB identity, writable (code, tables, data)
 *   PD[1] -> PT   @0x5000
 *   PT[1] = 0x00201001        VA 0x201000 -> PA 0x201000, read-only      <- the faulting page
 *   PT[2] = 0x100003          VA 0x202000 -> PA 0x100000 <- non-identity
 *
 * The guest stores to 0x201000 (refused: leaf is read-only), the runtime
 * delivers #PF through IDT[14] to the handler at the end of this image, the
 * handler records CR2/error-code/frame-RIP and makes the leaf writable, and
 * IRETs.  The store is then retried and succeeds, and a store through the
 * non-identity alias must appear at PA 0x100000.
 *
 * Offsets are relative to PROGRAM_PC.  EFER.LME is bit 8 (value 0x100);
 * `mov r32, imm32` is always five bytes; `lgdt/lidt [disp32]` need the SIB
 * form (modrm 0x14, SIB 0x25), since modrm 0x15 is RIP-relative.
 *
 *    *   +0x00: b9 80 00 00 c0                         mov  ecx, 0xc0000080
 *   +0x05: b8 00 01 00 00                         mov  eax, 0x100
 *   +0x0a: 31 d2                                  xor  edx, edx
 *   +0x0c: 0f 30                                  wrmsr
 *   +0x0e: 0f 01 14 25 00 12 00 00                lgdt [0x1200]
 *   +0x16: c7 04 25 c0 15 00 00 e7 10 08 00       mov  dword [0x15c0], offset|selector
 *   +0x21: c7 04 25 c4 15 00 00 00 8e 00 00       mov  dword [0x15c4], type_attr
 *   +0x2c: c7 04 25 c8 15 00 00 00 00 00 00       mov  dword [0x15c8], 0
 *   +0x37: c7 04 25 cc 15 00 00 00 00 00 00       mov  dword [0x15cc], 0
 *   +0x42: 0f 01 1c 25 00 14 00 00                lidt [0x1400]
 *   +0x4a: 48 c7 04 25 00 20 00 00 03 30 00 00    mov qword [0x2000], 0x3003
 *   +0x56: 48 c7 04 25 00 30 00 00 03 40 00 00    mov qword [0x3000], 0x4003
 *   +0x62: 48 c7 04 25 00 40 00 00 83 00 00 00    mov qword [0x4000], 0x83
 *   +0x6e: 48 c7 04 25 08 40 00 00 03 50 00 00    mov qword [0x4008], 0x5003
 *   +0x7a: 48 c7 04 25 08 50 00 00 81 00 00 00    mov qword [0x5008], 0x81
 *   +0x86: 48 c7 04 25 10 50 00 00 03 00 10 00    mov qword [0x5010], 0x100003
 *   +0x92: b8 20 00 00 00                         mov  eax, 0x20
 *   +0x97: 0f 22 e0                               mov  cr4, eax
 *   +0x9a: b8 00 20 00 00                         mov  eax, 0x2000
 *   +0x9f: 0f 22 d8                               mov  cr3, eax
 *   +0xa2: b8 01 00 01 80                         mov  eax, 0x80010001
 *   +0xa7: 0f 22 c0                               mov  cr0, eax
 *   +0xaa: c6 04 25 00 10 20 00 01                mov  byte [0x201000], 1   ; #PF
 *   +0xb2: c6 04 25 00 10 20 00 02                mov  byte [0x201000], 2   ; after the handler
 *   +0xba: 48 c7 04 25 00 20 20 00 11 22 33 44    mov  qword [0x202000], 0x44332211
 *   +0xc6: 48 8b 04 25 00 20 20 00                mov  rax, [0x202000]
 *   +0xce: 41 89 c6                               mov  r14d, eax
 *   +0xd1: be 00 00 00 00                         mov  esi, <str>
 *   +0xd6: 8a 06                                  mov  al, [rsi]
 *   +0xd8: 84 c0                                  test al, al
 *   +0xda: 74 0c                                  jz   +0x0c
 *   +0xdc: ba f8 03 00 00                         mov  edx, 0x3f8
 *   +0xe1: ee                                     out  dx, al
 *   +0xe2: 48 ff c6                               inc  rsi
 *   +0xe5: eb ef                                  jmp  -0x11
 *   +0xe7: 90                                     nop
 *   +0xe8: eb fe                                  jmp  $
 * Handler for IDT[14]; the gate at +0x16 points at +0xea (0x10e8).
 *   +0xea: 48 89 24 25 18 16 00 00                mov  qword [0x1618], rsp   ; frame pointer
 *   +0xf2: 0f 20 d2                               mov  rdx, cr2
 *   +0xf5: 48 89 14 25 00 16 00 00                mov  qword [0x1600], rdx   ; CR2
 *   +0xfd: 48 8b 4c 24 08                         mov  rcx, [rsp+8]          ; frame RIP
 *   +0x102: 48 89 0c 25 10 16 00 00                mov  qword [0x1610], rcx
 *   +0x10a: 8b 0c 24                               mov  ecx, [rsp]            ; error code
 *   +0x10d: 89 0c 25 04 16 00 00                   mov  dword [0x1620], ecx
 *   +0x114: ff 04 25 08 16 00 00                   inc  dword [0x1628]        ; runs
 *   +0x11b: 48 c7 04 25 08 50 00 00 83 00 00 00    mov  qword [0x5008], 0x03   ; fix the PTE
 *   +0x127: 48 83 c4 08                            add  rsp, 8                ; drop the error code
 *   +0x12b: 48 cf                                  iretq
 */
static const uint8_t program[] = {
    0xb9, 0x80, 0x00, 0x00, 0xc0, 0xb8, 0x00, 0x01,
    0x00, 0x00, 0x31, 0xd2, 0x0f, 0x30, 0x0f, 0x01,
    0x14, 0x25, 0x00, 0x12, 0x00, 0x00, 0xc7, 0x04,
    0x25, 0xc0, 0x15, 0x00, 0x00, 0x09, 0x11, 0x08,
    0x00, 0xc7, 0x04, 0x25, 0xc4, 0x15, 0x00, 0x00,
    0x00, 0x8e, 0x00, 0x00, 0xc7, 0x04, 0x25, 0xc8,
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7,
    0x04, 0x25, 0xcc, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x01, 0x1c, 0x25, 0x00, 0x14,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x00, 0x20,
    0x00, 0x00, 0x03, 0x30, 0x00, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x00, 0x30, 0x00, 0x00, 0x03, 0x40,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x00, 0x40,
    0x00, 0x00, 0x83, 0x00, 0x00, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x08, 0x40, 0x00, 0x00, 0x03, 0x50,
    0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x08, 0x50,
    0x00, 0x00, 0x01, 0x10, 0x20, 0x00, 0x48, 0xc7,
    0x04, 0x25, 0x10, 0x50, 0x00, 0x00, 0x03, 0x00,
    0x10, 0x00, 0x31, 0xc0, 0x31, 0xd2, 0x0f, 0x32,
    0x89, 0xc3, 0xb8, 0x20, 0x00, 0x00, 0x00, 0x0f,
    0x22, 0xe0, 0x0f, 0x20, 0xe0, 0x41, 0x89, 0xc5,
    0xb8, 0x00, 0x20, 0x00, 0x00, 0x0f, 0x22, 0xd8,
    0x0f, 0x20, 0xd8, 0x41, 0x89, 0xc4, 0xb8, 0x01,
    0x00, 0x01, 0x80, 0x0f, 0x22, 0xc0, 0x0f, 0x20,
    0xc0, 0x89, 0xc7, 0xc6, 0x04, 0x25, 0x00, 0x10,
    0x20, 0x00, 0x01, 0xc6, 0x04, 0x25, 0x00, 0x10,
    0x20, 0x00, 0x02, 0x48, 0xc7, 0x04, 0x25, 0x00,
    0x20, 0x20, 0x00, 0x11, 0x22, 0x33, 0x44, 0x48,
    0x8b, 0x04, 0x25, 0x00, 0x20, 0x20, 0x00, 0x41,
    0x89, 0xc6, 0xba, 0xfd, 0x03, 0x00, 0x00, 0xec,
    0x0f, 0xb6, 0xc0, 0x89, 0xc5, 0xbe, 0x00, 0x00,
    0x00, 0x00, 0x8a, 0x06, 0x84, 0xc0, 0x74, 0x46,
    0xba, 0xf8, 0x03, 0x00, 0x00, 0xee, 0x48, 0xff,
    0xc6, 0xeb, 0xef, 0x0f, 0x20, 0xd2, 0x48, 0x89,
    0x14, 0x25, 0x00, 0x16, 0x00, 0x00, 0x48, 0x8b,
    0x4c, 0x24, 0x08, 0x48, 0x89, 0x0c, 0x25, 0x10,
    0x16, 0x00, 0x00, 0x8b, 0x0c, 0x24, 0x89, 0x0c,
    0x25, 0x20, 0x16, 0x00, 0x00, 0xff, 0x04, 0x25,
    0x28, 0x16, 0x00, 0x00, 0x48, 0xc7, 0x04, 0x25,
    0x08, 0x50, 0x00, 0x00, 0x03, 0x10, 0x20, 0x00,
    0x48, 0x83, 0xc4, 0x08, 0x48, 0xcf, 0xeb, 0xfe,
};

/* NUL terminated; the guest walks it and writes it out port by port. */
static const char banner[] = "loongemu: long mode\n";

static const uint8_t gdt_descriptor[] = {
    0x1f, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define HALT_PC (PROGRAM_PC + 0x146)
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

    memset((void *)GUEST_BASE, 0, GUEST_RAM_SIZE);
    memcpy((void *)(uintptr_t)PROGRAM_PC, program, sizeof(program));

    /*
     * With the page-table check enabled, everything the guest reads must be
     * inside the range its own tables map -- host addresses below 2 MiB, the
     * 2 MiB leaf the guest builds.  So the descriptor and the banner are
     * placed in that range at the addresses the program names, instead of in
     * this image's .bss.
     */
    memcpy((void *)(uintptr_t)GDT_PC, gdt_descriptor, sizeof(gdt_descriptor));
    /* IDT descriptor for lidt: 16-bit limit, 64-bit base of the table. */
    {
        static const uint8_t idt_descriptor[10] = {
            0xff, 0x00,                     /* limit 0xff (16 gates)  */
            0xe0, 0x14, 0x00, 0x00,         /* base 0x14e0             */
            0x00, 0x00, 0x00, 0x00,
        };

        memcpy((void *)(uintptr_t)IDT_DESC_PC, idt_descriptor,
               sizeof(idt_descriptor));
    }
    memcpy((void *)(uintptr_t)STR_PC, banner, sizeof(banner));
    /* The non-identity target starts clear so the guest's store is visible. */
    *(volatile uint64_t *)(uintptr_t)ALIAS_PA = 0;
    {
        uint32_t str_addr = STR_PC;

        memcpy((void *)(uintptr_t)(PROGRAM_PC + 0xf6), &str_addr,
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
        /*
         * The halt block is `jmp $`.  Stopping once the guest is inside it --
         * rather than only at its exact start -- keeps the harness from
         * depending on how the translator materializes a self-jump.
         */
        if (pc >= HALT_PC && pc <= HALT_PC + 1) {
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
            puthex(env.idt.base);
            puthex(env.idt.limit);

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
    latx_bare_uart_puts("tb-sys: handler runs=");
    puthex(*(volatile uint32_t *)(uintptr_t)0x1628);
    latx_bare_uart_puts(" cr2=");
    puthex(*(volatile uint64_t *)(uintptr_t)0x1600);
    latx_bare_uart_puts(" ec=");
    puthex(*(volatile uint32_t *)(uintptr_t)0x1620);
    latx_bare_uart_puts("\n");
    latx_bare_uart_puts("tb-sys: alias r14=");
    puthex(env.regs[R_R14]);
    latx_bare_uart_puts(" pa100000=");
    puthex(*(volatile uint64_t *)(uintptr_t)ALIAS_PA);
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
    /*
     * The fault was delivered to the guest's own IDT[14] handler, which
     * recorded what it saw.  No driver-side fault is expected: the guest owns
     * the exception now.
     */
    check(faults == 0, "no fault escaped to the driver");
    check(*(volatile uint32_t *)(uintptr_t)0x1628 == 1,
          "the #PF handler ran exactly once");
    check(*(volatile uint64_t *)(uintptr_t)0x1600 == RO_PC,
          "handler saw CR2 = the faulting store address");
    check(*(volatile uint64_t *)(uintptr_t)0x1610 == STORE_PC,
          "the frame RIP is the faulting instruction");
    check(*(volatile uint32_t *)(uintptr_t)0x1620 == 0x3u,
          "error code is P|W (supervisor write)");
    check(*(volatile uint64_t *)(uintptr_t)0x5008 == 0x00201003u,
          "the handler made the leaf writable");
    /*
     * The first store faults and is retried; the retry is the second store,
     * which writes 2.  Seeing 2 also proves the handler's IRET returned to the
     * faulting instruction rather than past it.
     */
    check(*(volatile uint8_t *)(uintptr_t)RO_PC == 2,
          "the retried store landed after the PTE was fixed");
    /*
     * The same tables, translated by the runtime's walker: 2 MiB leaf at the
     * PD level, identity mapped, writable, supervisor only.
     */
    {
        LatxX86Translation t;
        LatxX86WalkResult r;
        LatxX86Translation probe;

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

    /*
     * Non-identity mapping: the guest stored 0x44332211 through VA
     * 0x40000000, which PDPT[1]/PD2 translate to PA 0x100000.  The harness
     * reads that physical page directly -- the translator cannot help here --
     * and the value the guest read back into r14d must agree.
     */
    {
        LatxX86Translation probe;

        check(latx_x86_walk(&env, ALIAS_VA, LATX_X86_ACCESS_READ, &probe) ==
              LATX_X86_WALK_OK && probe.gpa == ALIAS_PA &&
              probe.page_bits == 12,
              "walk maps the aliased VA to PA 0x100000");
    }
    check(*(volatile uint64_t *)(uintptr_t)ALIAS_PA == 0x44332211ull,
          "store through the aliased VA reached PA 0x100000");
    check(env.regs[R_R14] == 0x44332211u, "read through the aliased VA agrees");

    /* the 4-level tables the guest built, read back through the data path */
    check(*(volatile uint64_t *)0x2000 == 0x3003u, "PML4 entry");
    check(*(volatile uint64_t *)0x3000 == 0x4003u, "PDPT entry");
    check(*(volatile uint64_t *)0x4000 == 0x83u, "PD entry (2 MiB leaf)");
    /* every byte of the banner came out of the guest's loop */
    check(uart_tx_count == (int)sizeof(banner) - 1 &&
          memcmp(uart_tx, banner, sizeof(banner) - 1) == 0,
          "out 0x3f8 printed the banner");
    check(env.regs[R_EBP] == LSR_THRE_TEMT, "in 0x3fd read the LSR");
    /*
     * The halt block is `jmp $` and its exit must leave eip pointing at that
     * jmp: a direct jump stores ir1_target_addr(), and for `jmp $` the target
     * is the instruction itself.  Anything else would step past it.
     */
    check(env.eip == HALT_PC, "the halt block's jmp $ leaves eip on itself");
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
