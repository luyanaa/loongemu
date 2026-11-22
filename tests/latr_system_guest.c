/*
 * RISC-V virt system differential guest.
 * Phases: Sv39, Sv48, Sv57, M-mode timer (CLINT), PLIC/UART0 external IRQ.
 * Output is a single deterministic "SYS:..." line on UART0.
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

#define UART ((volatile u8 *)0x10000000u)
#define CLINT_MTIMECMP ((volatile u64 *)0x02004000u)
#define CLINT_MTIME ((volatile u64 *)0x0200bff8u)
#define PLIC_PRIO ((volatile u32 *)0x0c000000u)
#define PLIC_ENABLE ((volatile u32 *)0x0c002000u)
#define PLIC_THRESHOLD ((volatile u32 *)0x0c200000u)
#define PLIC_CLAIM ((volatile u32 *)0x0c200004u)
#define TEST_EXIT ((volatile u32 *)0x00100000u)
#define SCODE_VA 0x40000000UL
#define SDATA_VA 0x40001000UL

static u64 pt_root[512] __attribute__((aligned(4096)));
static u64 pt_a[512] __attribute__((aligned(4096)));
static u64 pt_b[512] __attribute__((aligned(4096)));
static u64 pt_c[512] __attribute__((aligned(4096)));
static u64 pt_d[512] __attribute__((aligned(4096)));
static u64 pt_pool[8][512] __attribute__((aligned(4096)));
static u64 smode_data[512] __attribute__((aligned(4096)));
u64 mstack[4096] __attribute__((aligned(16)));
u64 trap_stack[1024] __attribute__((aligned(16)));
static volatile u64 result;
static volatile u32 phase;
static volatile u32 timer_seen;
static volatile u32 plic_seen;

extern char smode_stub[];
extern void after_smode_asm(void);
extern void trap_entry(void);

__asm__(
".section .text\n"
".globl smode_stub\n"
"smode_stub:\n"
"  li t0, 0x40001000\n"
"  ld t1, 0(t0)\n"
"  addi t1, t1, 1\n"
"  sd t1, 0(t0)\n"
"  ecall\n"
"1: j 1b\n"
".globl after_smode_asm\n"
"after_smode_asm:\n"
"  la sp, mstack\n"
"  li t0, 32768\n"
"  add sp, sp, t0\n"
"  call after_smode\n"
"1: j 1b\n"
".globl trap_entry\n"
"trap_entry:\n"
"  la sp, trap_stack\n"
"  li t0, 8192\n"
"  add sp, sp, t0\n"
"  addi sp, sp, -256\n"
"  sd ra, 0(sp)\n"
"  sd t0, 8(sp)\n"
"  sd t1, 16(sp)\n"
"  sd t2, 24(sp)\n"
"  sd t3, 32(sp)\n"
"  sd t4, 40(sp)\n"
"  sd t5, 48(sp)\n"
"  sd t6, 56(sp)\n"
"  sd a0, 64(sp)\n"
"  sd a1, 72(sp)\n"
"  sd a2, 80(sp)\n"
"  sd a3, 88(sp)\n"
"  sd a4, 96(sp)\n"
"  sd a5, 104(sp)\n"
"  sd a6, 112(sp)\n"
"  sd a7, 120(sp)\n"
"  csrr a0, mcause\n"
"  csrr a1, mepc\n"
"  call trap_handler\n"
"  ld ra, 0(sp)\n"
"  ld t0, 8(sp)\n"
"  ld t1, 16(sp)\n"
"  ld t2, 24(sp)\n"
"  ld t3, 32(sp)\n"
"  ld t4, 40(sp)\n"
"  ld t5, 48(sp)\n"
"  ld t6, 56(sp)\n"
"  ld a0, 64(sp)\n"
"  ld a1, 72(sp)\n"
"  ld a2, 80(sp)\n"
"  ld a3, 88(sp)\n"
"  ld a4, 96(sp)\n"
"  ld a5, 104(sp)\n"
"  ld a6, 112(sp)\n"
"  ld a7, 120(sp)\n"
"  addi sp, sp, 256\n"
"  mret\n");

static void putc(u8 c)
{
    *UART = c;
}

static void puts(const char *s)
{
    while (*s) {
        putc((u8)*s++);
    }
}

static void puthex(u64 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    int n = 16;

    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        buf[i] = digits[v & 15u];
        v >>= 4;
    }
    while (n > 1 && buf[16 - n] == '0') {
        n--;
    }
    puts(&buf[16 - n]);
}

static void csr_write(const char *name, u64 value);
#define CSR_WRITE(name, value) \
    __asm__ volatile("csrw " #name ", %0" :: "r"((u64)(value)))

static u64 csr_read_mstatus(void)
{
    u64 v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static void csr_set_mstatus(u64 bits)
{
    __asm__ volatile("csrs mstatus, %0" :: "r"(bits));
}

static void csr_clear_mstatus(u64 bits)
{
    __asm__ volatile("csrc mstatus, %0" :: "r"(bits));
}

static void csr_set_mie(u64 bits)
{
    __asm__ volatile("csrs mie, %0" :: "r"(bits));
}

static void csr_clear_mie(u64 bits)
{
    __asm__ volatile("csrc mie, %0" :: "r"(bits));
}

static void clear_tables(void)
{
    for (int i = 0; i < 512; i++) {
        pt_root[i] = 0;
        pt_a[i] = 0;
        pt_b[i] = 0;
        pt_c[i] = 0;
        pt_d[i] = 0;
    }
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 512; i++) {
            pt_pool[t][i] = 0;
        }
    }
}

static int pool_next;
static u64 *pool_alloc(void)
{
    return pt_pool[pool_next++];
}

static u64 pte_leaf(u64 pa)
{
    return ((pa >> 12) << 10) | 0xcfu; /* V R W X A D */
}

static u64 pte_table(u64 *table)
{
    return (((u64)(unsigned long)table >> 12) << 10) | 1u;
}

static void map_page(u64 va, u64 pa, int levels)
{
    u64 *tab = pt_root;

    for (int level = levels - 1; level > 0; level--) {
        u64 idx = (va >> (12 + 9 * level)) & 0x1ffu;
        if ((tab[idx] & 1u) == 0) {
            u64 *next = pool_alloc();
            tab[idx] = pte_table(next);
        }
        tab = (u64 *)(unsigned long)(((tab[idx] >> 10) << 12));
    }
    tab[(va >> 12) & 0x1ffu] = pte_leaf(pa);
}

static void enter_s(u64 pc, u64 satp)
{
    u64 status = csr_read_mstatus();

    status &= ~(3UL << 11);
    status |= (1UL << 11);  /* MPP = S */
    status &= ~(1UL << 7);  /* MPIE = 0: no M interrupts in S */
    status &= ~(1UL << 3);  /* MIE = 0 */
    CSR_WRITE(satp, satp);
    CSR_WRITE(mepc, pc);
    CSR_WRITE(mstatus, status);
    __asm__ volatile("mret" ::: "memory");
}

static void run_mode(int mode)
{
    putc('r');
    putc((u8)('0' + (mode - 8)));
    u64 code_pa = ((u64)(unsigned long)smode_stub) & ~0xfffUL;
    u64 code_off = ((u64)(unsigned long)smode_stub) & 0xfffUL;
    u64 data_pa = (u64)(unsigned long)smode_data;
    int levels = mode == 8 ? 3 : mode == 9 ? 4 : 5;

    clear_tables();
    pool_next = 0;
    smode_data[0] = 0x1234;
    map_page(SCODE_VA, code_pa, levels);
    map_page(SDATA_VA, data_pa, levels);
    phase = (u32)(mode == 8 ? 0 : mode == 9 ? 1 : 2);
    enter_s(SCODE_VA + code_off, ((u64)mode << 60) |
             (((u64)(unsigned long)pt_root >> 12)));
}

void trap_handler(u64 cause, u64 epc)
{
    if (cause & (1UL << 63)) {
        u64 code = cause & 0xffu;
        if (code == 7) { /* machine timer */
            putc('!');
            timer_seen = 1;
            csr_clear_mie(1UL << 7);
            return;
        }
        if (code == 11) { /* machine external */
            u32 source = *PLIC_CLAIM;
            if (source == 10) {
                plic_seen = 1;
                *((volatile u8 *)(0x10000000u + 1)) = 0; /* IER = 0 */
                *PLIC_CLAIM = source;
                csr_clear_mie(1UL << 11);
            }
            return;
        }
        return;
    }
    if (cause == 9) { /* S-mode ecall: return to M-mode resume stub */
        result = smode_data[0];
        CSR_WRITE(satp, 0);
        {
            u64 status = csr_read_mstatus();
            status &= ~(3UL << 11);
            status |= (3UL << 11); /* MPP = M */
            status |= (1UL << 7);  /* MPIE = 1 */
            CSR_WRITE(mstatus, status);
        }
        CSR_WRITE(mepc, (u64)(unsigned long)after_smode_asm);
        return;
    }
    puts("E");
    puthex(cause);
    puts(":PC=");
    puthex(epc);
    puts(";");
    for (;;) {
    }
}

void after_smode(void)
{
    puts(phase == 0 ? "M39=" : phase == 1 ? "M48=" : "M57=");
    puthex(result);
    puts(";");
    if (phase == 0) {
        run_mode(9);
    } else if (phase == 1) {
        run_mode(10);
    } else {
        /* Timer test. */
        putc('t');
        *CLINT_MTIMECMP = *CLINT_MTIME + 1000;
        putc('w');
        csr_set_mie(1UL << 7);
        putc('m');
        csr_set_mstatus(1UL << 3);
        putc('s');
        while (!timer_seen) {
        }
        putc('T');
        csr_clear_mie(1UL << 7);
        puts("T=1;");

        /* PLIC/UART0 THRE test. */
        PLIC_PRIO[10] = 1;
        PLIC_ENABLE[10 / 32] |= 1u << (10 % 32);
        *PLIC_THRESHOLD = 0;
        *((volatile u8 *)(0x10000000u + 1)) = 0x02; /* IER: THRE */
        csr_set_mie(1UL << 11);
        csr_set_mstatus(1UL << 3);
        while (!plic_seen) {
        }
        csr_clear_mie(1UL << 11);
        puts("P=1;SYS-OK\n");
        *TEST_EXIT = 0x5555;
        for (;;) {
        }
    }
}

void kmain(void)
{
    /* QEMU with -bios none has no PMP rules; S-mode would fault. */
    __asm__ volatile(
        "li t0, -1\n"
        "csrw pmpaddr0, t0\n"
        "li t0, 0x0f\n"
        "csrw pmpcfg0, t0\n");
    CSR_WRITE(mtvec, (u64)(unsigned long)trap_entry);
    puts("SYS:");
    run_mode(8);
}

__attribute__((naked, noreturn, section(".text.start"))) void _start(void)
{
    __asm__ volatile(
        "la sp, mstack\n"
        "li t0, 32768\n"
        "add sp, sp, t0\n"
        "call kmain\n"
        "1: j 1b\n");
}
