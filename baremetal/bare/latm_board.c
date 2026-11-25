/*
 * Minimal MIPS64r2 board: LoongemuMemMap RAM/MMIO, a synthetic MIPS64 image,
 * and a flattened device tree.  This is the bring-up surface for the LATM
 * system-mode path; it intentionally does not carry a Linux kernel.
 */
#include "qemu/osdep.h"
#include <stdio.h>
#include <string.h>
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "cpu.h"
#include "internal.h"
#include "latm.h"
#include "latm-helpers.h"
#include "latm-platform.h"
#include "translate.h"
#include "loongemu/phys_mem.h"
#include "loongemu/vclock.h"
#include "loongemu/tb_cache.h"
#include "latm-system-board.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
void latx_bare_uart_putc(int c) { *UART0_THR = (unsigned char)c; }
void latx_bare_uart_puts(const char *s) { while (*s) latx_bare_uart_putc(*s++); }
static void puthex(uint64_t v) { char b[17]; for (int i=15;i>=0;i--) b[15-i]="0123456789abcdef"[(v>>(i*4))&15]; b[16]=0; latx_bare_uart_puts(b); }

#define RAM_SIZE (32u << 20)
#define DTB_GPA  0x100000u
#define UART_GPA 0x1f000000u
static uint8_t ram[RAM_SIZE] __attribute__((aligned(4096)));
static LoongemuMemMap mem_map;
static char uart_log[64];
static unsigned uart_len;
static LoongemuVClock vclock;
typedef struct Uart16550 {
    uint8_t data;
    uint8_t ier;
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    uint8_t tx_pending;
    uint8_t rx_fifo[16];
    unsigned rx_head;
    unsigned rx_tail;
} Uart16550;

static Uart16550 uart;
static uint32_t count_bias;
static uint32_t compare_value;
static int compare_armed;
static TbCache board_tb_cache;
static unsigned board_tb_hits;
static volatile int board_exit_code;
static uint64_t board_exit_jmp[16] __attribute__((aligned(16)));
static volatile int board_exit_active;

/* Minimal LoongArch setjmp/longjmp pair.  The generated TB may clobber the
 * callee-saved guest-mapped registers, so restore ra/sp/fp/s0..s8 exactly. */
__asm__(
    ".text\n"
    ".globl board_setjmp\n"
    ".type board_setjmp,@function\n"
    "board_setjmp:\n"
    "  st.d $ra, $a0, 0\n"
    "  st.d $sp, $a0, 8\n"
    "  st.d $fp, $a0, 16\n"
    "  st.d $s0, $a0, 24\n"
    "  st.d $s1, $a0, 32\n"
    "  st.d $s2, $a0, 40\n"
    "  st.d $s3, $a0, 48\n"
    "  st.d $s4, $a0, 56\n"
    "  st.d $s5, $a0, 64\n"
    "  st.d $s6, $a0, 72\n"
    "  st.d $s7, $a0, 80\n"
    "  st.d $s8, $a0, 88\n"
    "  move $a0, $zero\n"
    "  jirl $zero, $ra, 0\n"
    ".size board_setjmp, .-board_setjmp\n"
    ".globl board_longjmp\n"
    ".type board_longjmp,@function\n"
    "board_longjmp:\n"
    "  ld.d $ra, $a0, 0\n"
    "  ld.d $sp, $a0, 8\n"
    "  ld.d $fp, $a0, 16\n"
    "  ld.d $s0, $a0, 24\n"
    "  ld.d $s1, $a0, 32\n"
    "  ld.d $s2, $a0, 40\n"
    "  ld.d $s3, $a0, 48\n"
    "  ld.d $s4, $a0, 56\n"
    "  ld.d $s5, $a0, 64\n"
    "  ld.d $s6, $a0, 72\n"
    "  ld.d $s7, $a0, 80\n"
    "  ld.d $s8, $a0, 88\n"
    "  li.d $a0, 1\n"
    "  jirl $zero, $ra, 0\n"
    ".size board_longjmp, .-board_longjmp\n"
);
extern int board_setjmp(uint64_t *);
extern void board_longjmp(uint64_t *);

void cpu_loop_exit(struct CPUState *cs)
{
    (void)cs;
    board_exit_code = 1;
    if (board_exit_active) {
        board_longjmp(board_exit_jmp);
    }
}

static int uart_rx_available(void)
{
    return uart.rx_head != uart.rx_tail;
}

static void uart_rx_push(uint8_t v)
{
    unsigned next = (uart.rx_head + 1) % sizeof(uart.rx_fifo);
    if (next != uart.rx_tail) {
        uart.rx_fifo[uart.rx_head] = v;
        uart.rx_head = next;
    }
}

static uint8_t uart_read_reg(unsigned off)
{
    bool dlab = (uart.lcr & 0x80) != 0;

    switch (off & 7) {
    case 0:
        if (dlab) {
            return uart.dll;
        }
        if (uart_rx_available()) {
            uint8_t v = uart.rx_fifo[uart.rx_tail];
            uart.rx_tail = (uart.rx_tail + 1) % sizeof(uart.rx_fifo);
            return v;
        }
        return 0;
    case 1: return dlab ? uart.dlm : uart.ier;
    case 2: {
        uint8_t v;
        if ((uart.ier & 1) && uart_rx_available()) {
            v = 0x04; /* received data available */
        } else if ((uart.ier & 2) && uart.tx_pending) {
            v = 0x02; /* THR empty */
        } else {
            v = 0x01; /* no interrupt */
        }
        uart.tx_pending = 0;
        return v;
    }
    case 3: return uart.lcr;
    case 4: return uart.mcr;
    case 5: return 0x60 | (uart_rx_available() ? 0x01 : 0x00);
    case 6: return 0xb0; /* CTS | DSR | DCD */
    default: return uart.scr;
    }
}

static void uart_write_reg(unsigned off, uint8_t v)
{
    bool dlab = (uart.lcr & 0x80) != 0;

    switch (off & 7) {
    case 0:
        if (dlab) {
            uart.dll = v;
        } else {
            uart.data = v;
            if (uart_len + 1 < sizeof(uart_log)) {
                uart_log[uart_len++] = (char)v;
                uart_log[uart_len] = 0;
            }
            latx_bare_uart_putc(v);
            if (uart.ier & 2) {
                uart.tx_pending = 1;
            }
        }
        break;
    case 1:
        if (dlab) {
            uart.dlm = v;
        } else {
            uart.ier = v & 0x0f;
            if (uart.ier & 2) {
                uart.tx_pending = 1;
            }
        }
        break;
    case 2:
        uart.fcr = v;
        if (v & 0x04) {
            uart.tx_pending = 0;
        }
        if (v & 0x02) {
            uart.rx_head = uart.rx_tail = 0;
        }
        break;
    case 3: uart.lcr = v; break;
    case 4: uart.mcr = v; break;
    case 5: break;
    case 6: break;
    default: uart.scr = v; break;
    }
}

static int uart_irq_level(void)
{
    return ((uart.ier & 0x01) && uart_rx_available()) ||
           ((uart.ier & 0x02) && uart.tx_pending);
}

static uint32_t board_raw_count(void)
{
    return loongemu_vclock_read_mips_count(&vclock, 50000000);
}

static uint32_t board_read_count(CPUMIPSState *env, uint64_t hz)
{
    (void)env;
    (void)hz;
    return board_raw_count() + count_bias;
}

static void board_store_count(CPUMIPSState *env, uint32_t count)
{
    (void)env;
    count_bias = count - board_raw_count();
}

static void board_set_compare(CPUMIPSState *env, uint32_t compare)
{
    (void)env;
    compare_value = compare;
    compare_armed = 1;
}

static void board_tlb_flush(CPUMIPSState *env)
{
    (void)env;
    tbc_flush_all(&board_tb_cache);
}

static void board_request_interrupt(CPUMIPSState *env, uint32_t cause_bits)
{
    env->CP0_Cause |= cause_bits;
}

static int board_read(CPUMIPSState *env, uint64_t gpa, uint64_t *value,
                      uint32_t size)
{
    LoongemuMemLookup lk;
    (void)env;
    *value = 0;
    if (loongemu_mem_resolve(&mem_map, gpa, size, &lk) == LOONGEMU_MEM_RAM) {
        for (uint32_t i = 0; i < size; i++) {
            *value |= (uint64_t)ram[lk.offset + i] << (8 * i);
        }
        return LATM_MEM_OK;
    }
    if (gpa >= UART_GPA && gpa < UART_GPA + 0x100 && size == 1) {
        *value = uart_read_reg((unsigned)(gpa - UART_GPA));
        return LATM_MEM_OK;
    }
    return LATM_MEM_FAULT;
}

static int board_write(CPUMIPSState *env, uint64_t gpa, uint64_t value,
                       uint32_t size)
{
    LoongemuMemLookup lk;
    (void)env;
    if (loongemu_mem_resolve(&mem_map, gpa, size, &lk) == LOONGEMU_MEM_RAM) {
        for (uint32_t i = 0; i < size; i++) {
            ram[lk.offset + i] = (uint8_t)(value >> (8 * i));
        }
        return LATM_MEM_OK;
    }
    if (gpa >= UART_GPA && gpa < UART_GPA + 0x100 && size == 1) {
        uart_write_reg((unsigned)(gpa - UART_GPA), (uint8_t)value);
        return LATM_MEM_OK;
    }
    return LATM_MEM_FAULT;
}

static int board_atomic_ll(CPUMIPSState *env, uint64_t gpa, unsigned size,
                           uint64_t *value)
{
    LoongemuMemLookup lk;
    (void)env;
    if (!value || (size != 4 && size != 8) ||
        loongemu_mem_resolve(&mem_map, gpa, size, &lk) != LOONGEMU_MEM_RAM) {
        return LATM_MEM_FAULT;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)ram[lk.offset + i] << (8 * i);
    }
    *value = v;
    return LATM_MEM_OK;
}

static int board_atomic_sc(CPUMIPSState *env, uint64_t gpa, unsigned size,
                           uint64_t expected, uint64_t desired, int *ok)
{
    LoongemuMemLookup lk;
    (void)env;
    if (!ok || (size != 4 && size != 8) ||
        loongemu_mem_resolve(&mem_map, gpa, size, &lk) != LOONGEMU_MEM_RAM) {
        return LATM_MEM_FAULT;
    }
    *ok = 0;
    if (size == 4) {
        uint32_t old = (uint32_t)expected;
        *ok = __atomic_compare_exchange_n(
            (uint32_t *)(void *)(ram + lk.offset), &old, (uint32_t)desired,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else {
        uint64_t old = expected;
        *ok = __atomic_compare_exchange_n(
            (uint64_t *)(void *)(ram + lk.offset), &old, desired, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    return LATM_MEM_OK;
}

static int board_fetch(CPUMIPSState *env, uint64_t gpa, uint32_t *value)
{
    uint64_t v;
    if (board_read(env, gpa, &v, 4) != LATM_MEM_OK) return LATM_MEM_FAULT;
    *value = (uint32_t)v;
    return LATM_MEM_OK;
}

static void board_smc_broadcast(CPUMIPSState *env, uint64_t start,
                                uint64_t end);

/* The board never wired IOCSR before: nothing in these scenarios used it. The
 * two-vCPU IPI needs it, so a single uncore instance sits behind the platform
 * hooks and the dispatching CPU id tells the model who wrote. */
static LatmUncore uncore;
static unsigned board_cpu_id;
static unsigned board_iocsr_writes;

static uint64_t board_iocsr_read(CPUMIPSState *env, uint64_t addr, unsigned size)
{
    (void)env;
    return latm_uncore_iocsr_read_cpu(&uncore, addr, size, board_cpu_id);
}

static void board_iocsr_write(CPUMIPSState *env, uint64_t addr, uint64_t value,
                              unsigned size)
{
    (void)env;
    board_iocsr_writes++;
    latm_uncore_iocsr_write_cpu(&uncore, addr, value, size, board_cpu_id);
}

static const LatmPlatform platform = {
    .mem_read = board_read,
    .mem_write = board_write,
    .fetch32 = board_fetch,
    .read_count = board_read_count,
    .store_count = board_store_count,
    .set_compare = board_set_compare,
    .tlb_flush = board_tlb_flush,
    .smc_broadcast = board_smc_broadcast,
    .request_interrupt = board_request_interrupt,
    .atomic_ll = board_atomic_ll,
    .atomic_sc = board_atomic_sc,
    .iocsr_read = board_iocsr_read,
    .iocsr_write = board_iocsr_write,
};

/* Tiny big-endian FDT builder. */
static uint8_t fdt[8192];
static uint8_t fdt_struct[4096];
static uint8_t fdt_strings[1024];
static unsigned fdt_struct_len, fdt_strings_len;

static void be32(uint8_t *p, uint32_t v)
{
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
static void pad4(unsigned *n) { while (*n & 3) fdt_struct[(*n)++] = 0; }
static unsigned stroff(const char *s)
{
    unsigned n = 0;
    while (n < fdt_strings_len) {
        if (!strcmp((char *)&fdt_strings[n], s)) return n;
        n += strlen((char *)&fdt_strings[n]) + 1;
    }
    unsigned off = fdt_strings_len;
    size_t len = strlen(s) + 1;
    memcpy(&fdt_strings[fdt_strings_len], s, len);
    fdt_strings_len += len;
    return off;
}
static void fdt_begin(const char *name)
{
    be32(&fdt_struct[fdt_struct_len], 1); fdt_struct_len += 4;
    size_t n = strlen(name) + 1;
    memcpy(&fdt_struct[fdt_struct_len], name, n); fdt_struct_len += n;
    pad4(&fdt_struct_len);
}
static void fdt_end(void) { be32(&fdt_struct[fdt_struct_len], 2); fdt_struct_len += 4; }
static void fdt_prop(const char *name, const void *data, unsigned len)
{
    be32(&fdt_struct[fdt_struct_len], 3); fdt_struct_len += 4;
    be32(&fdt_struct[fdt_struct_len], len); fdt_struct_len += 4;
    be32(&fdt_struct[fdt_struct_len], stroff(name)); fdt_struct_len += 4;
    memcpy(&fdt_struct[fdt_struct_len], data, len); fdt_struct_len += len;
    pad4(&fdt_struct_len);
}
static void fdt_prop_u32(const char *name, uint32_t v)
{ uint8_t b[4]; be32(b,v); fdt_prop(name,b,4); }
static void fdt_prop_cells(const char *name, uint32_t a, uint32_t b,
                           uint32_t c, uint32_t d)
{ uint8_t x[16]; be32(x,a);be32(x+4,b);be32(x+8,c);be32(x+12,d); fdt_prop(name,x,16); }

static unsigned build_dtb(void)
{
    uint8_t zeros[8] = {0};
    fdt_struct_len = fdt_strings_len = 0;
    fdt_begin("");
    fdt_prop("#address-cells", "\0\0\0\2", 4);
    fdt_prop("#size-cells", "\0\0\0\2", 4);
    fdt_prop("compatible", "loongemu,latm-malta", sizeof("loongemu,latm-malta"));
    fdt_prop("model", "LoongEMU LATM Malta", sizeof("LoongEMU LATM Malta"));
    fdt_begin("memory@0");
    fdt_prop("device_type", "memory", sizeof("memory"));
    fdt_prop_cells("reg", 0, 0, 0, RAM_SIZE);
    fdt_end();
    fdt_begin("cpus");
    fdt_prop_u32("#address-cells", 1);
    fdt_prop_u32("#size-cells", 0);
    fdt_begin("cpu@0");
    fdt_prop("device_type", "cpu\0", 4);
    fdt_prop("compatible", "mips,mips64r2", sizeof("mips,mips64r2"));
    fdt_prop_u32("reg", 0);
    fdt_prop_u32("clock-frequency", 50000000);
    fdt_end(); fdt_end();
    fdt_begin("chosen");
    fdt_prop("bootargs", "console=ttyS0 earlycon=uart8250,mmio,0x1f000000", sizeof("console=ttyS0 earlycon=uart8250,mmio,0x1f000000"));
    fdt_end();
    fdt_begin("interrupt-controller");
    fdt_prop_u32("#interrupt-cells", 1);
    fdt_prop("interrupt-controller", zeros, 0);
    fdt_prop_u32("phandle", 1);
    fdt_end();
    fdt_begin("soc");
    fdt_prop("#address-cells", "\0\0\0\2", 4);
    fdt_prop("#size-cells", "\0\0\0\2", 4);
    fdt_prop("ranges", zeros, 0);
    fdt_begin("serial@1f000000");
    fdt_prop("compatible", "ns16550a", sizeof("ns16550a"));
    fdt_prop_u32("interrupt-parent", 1);
    fdt_prop_u32("interrupts", 2);
    fdt_prop_cells("reg", 0, UART_GPA, 0, 0x100);
    fdt_end(); fdt_end();
    fdt_end();
    be32(&fdt_struct[fdt_struct_len], 9); /* FDT_END */
    fdt_struct_len += 4;
    memset(fdt, 0, sizeof(fdt));
    unsigned off_struct = 56, off_rsv = 40;
    uint8_t hdr[48];
    memcpy(hdr, fdt, 48);
    be32(hdr+0, 0xd00dfeed); be32(hdr+4, 0); be32(hdr+8, off_struct);
    be32(hdr+12, off_struct + fdt_struct_len); be32(hdr+16, off_rsv);
    be32(hdr+20, 17); be32(hdr+24, 16); be32(hdr+28, 0);
    be32(hdr+32, fdt_strings_len); be32(hdr+36, fdt_struct_len);
    memcpy(fdt, hdr, 48);
    memset(fdt + off_rsv, 0, 16);
    memcpy(fdt + off_struct, fdt_struct, fdt_struct_len);
    memcpy(fdt + off_struct + fdt_struct_len, fdt_strings, fdt_strings_len);
    unsigned total = off_struct + fdt_struct_len + fdt_strings_len;
    be32(fdt + 4, total);
    return total;
}

static uint32_t uart_code[] = {
    0x3c08bf00, /* lui $t0,0xbf00 (kseg1 UART base) */
    0x64090080, 0xa1090003, /* LCR = DLAB */
    0x64090001, 0xa1090000, /* DLL = 1 */
    0x64090000, 0xa1090001, /* DLM = 0 */
    0x64090003, 0xa1090003, /* LCR = 8N1 */
    0x64090003, 0xa1090001, /* IER = RX|TX */
    0x810a0005,             /* lb $t2, 5($t0) -> LSR */
    0x1000ffff, 0x00000000, /* self loop */
};

static uint32_t kernel_code[] = {
    0x3c08bf00, /* lui $t0,0xbf00 */
    0x6409004c, 0xa1090000, /* 'L' */
    0x64090041, 0xa1090000, /* 'A' */
    0x64090054, 0xa1090000, /* 'T' */
    0x6409004d, 0xa1090000, /* 'M' */
    0x64090020, 0xa1090000, /* ' ' */
    0x64090062, 0xa1090000, /* 'b' */
    0x6409006f, 0xa1090000, /* 'o' */
    0x64090061, 0xa1090000, /* 'a' */
    0x64090072, 0xa1090000, /* 'r' */
    0x64090064, 0xa1090000, /* 'd' */
    0x6409000a, 0xa1090000, /* '\n' */
    0x1000ffff, 0x00000000,
};

static uint8_t code_buf[65536] __attribute__((aligned(4096)));
#define BOARD_TB_CACHE_CAP 128u

/* ---------------------------------------------------------------- two vCPUs
 *
 * The same guest runs on both CPUs; a0 carries the hart id, exactly the MIPS
 * boot convention. Each hart increments its own word and loops forever, so the
 * only thing under test is that two independent CPU contexts translate and
 * execute through one thread: per-CPU translator state
 * (latm_env/latm_tr_data), per-CPU TB cache and per-CPU host code buffer.
 *
 *   lui   $t0, 0x8000        ; kseg0 base, sign-extended
 *   dsll  $t1, $a0, 3        ; hart id -> 8-byte slot
 *   daddu $t0, $t0, $t1
 *   daddiu $t1, $zero, 0x1000
 *   daddu $t0, $t0, $t1      ; 0xffffffff80001000 + hart*8
 * 1: ld    $t2, 0($t0)
 *   daddiu $t2, $t2, 1
 *   sd    $t2, 0($t0)
 *   j     1b
 *   nop
 */
static const uint32_t smp_guest[] = {
    0x3c0c8000, /* lui    $t0, 0x8000 */
    0x000468f8, /* dsll   $t1, $a0, 3 */
    0x018d602d, /* daddu  $t0, $t0, $t1 */
    0x640d1000, /* daddiu $t1, $zero, 0x1000 */
    0x018d602d, /* daddu  $t0, $t0, $t1 */
    0xdd8e0000, /* ld     $t2, 0($t0) */
    0x65ce0001, /* daddiu $t2, $t2, 1 */
    0xfd8e0000, /* sd     $t2, 0($t0) */
    0x1000fffc, /* b      0x14 (PC-relative: the guest is linked at 0 but runs
                 * in kseg0, and `j` would take its region bits from PC) */
    0x00000000, /* nop */
};
/* IPI handler, entered at exception_base + 0x180: count the interrupt and
 * clear the mailbox bit through IOCSR (0x100c, write-1-to-clear). */
static const uint32_t ipi_handler[] = {
    /* $s0/$s1, not $k0/$k1: the LATM register allocator keeps k0/k1 for itself
     * and asserts on a guest write to them ("guest register writeback requires an
     * itemp"). An earlier version used $t0, which the interrupted loop was using
     * as its counter base, so after eret the loop incremented the handler's word
     * and the count read 6 for one interrupt. */
    0x3c108000, /* lui    $s0, 0x8000 */
    0x66101010, /* daddiu $s0, $s0, 0x1010 */
    0xde110000, /* ld     $s1, 0($s0) */
    0x66310001, /* daddiu $s1, $s1, 1 */
    0xfe110000, /* sd     $s1, 0($s0) */
    0x3410100c, /* ori    $s0, $zero, 0x100c */
    0x34110001, /* ori    $s1, $zero, 1 */
    0xca018918, /* iocsrwr $s1, $s0: major opcode 0x32, rd = value, rs = addr */
    0x42000018, /* eret */
};

/* CPU 0's IPI sender: IOCSR 0x1040 = (target << 16) | vector. */
static const uint32_t ipi_sender[] = {
    0x34051040, /* ori    $a1, $zero, 0x1040 */
    0x3c060001, /* lui    $a2, 0x1        (target 1, vector 0) */
    0xc8a13118, /* iocsrwr $a2, $a1 */
    0x1000ffff, /* b      . */
    0x00000000, /* nop */
};
#define SMP_GUEST_GPA 0x4000u
#define SMP_COUNTER_GPA 0x1000u
static CPUMIPSState env1;
static CPUMIPSTLBContext tlb1;
static uint8_t code_buf1[65536] __attribute__((aligned(4096)));
static uint32_t board_tb_next1 = 0x5000u;
static TbCache board_tb_cache1;
static TbCacheEntry board_tb_entries1[BOARD_TB_CACHE_CAP];
static uint32_t board_tb_buckets1[BOARD_TB_CACHE_CAP];
#define BOARD_TB_START 0x5000u
#define BOARD_TB_SLOT  0x2000u
static uint32_t board_tb_next = BOARD_TB_START;
static TranslationBlock tb;
static CPUMIPSState env;
static CPUMIPSTLBContext tlb;
static TbCacheEntry board_tb_entries[BOARD_TB_CACHE_CAP];
static uint32_t board_tb_buckets[BOARD_TB_CACHE_CAP];

static CPUMIPSState env1;

static unsigned board_cpu_of(const CPUMIPSState *e)
{
    return e == &env1 ? 1u : 0u;
}

static uint64_t board_asid_of(const CPUMIPSState *e)
{
    return e->CP0_EntryHi & e->CP0_EntryHi_ASID_mask;
}

static uint8_t board_mode_of(const CPUMIPSState *e)
{
    return (uint8_t)(e->hflags &
                     (MIPS_HFLAG_UM | MIPS_HFLAG_SM | MIPS_HFLAG_M16));
}

static uint64_t board_asid(void) { return board_asid_of(&env); }
static uint8_t board_mode(void) { return board_mode_of(&env); }

static void board_cache_invalidate(uint64_t start, uint64_t end)
{
    if (end > start) {
        tbc_invalidate_range(&board_tb_cache, board_asid(), start, end - start);
    }
}

static void board_smc_broadcast(CPUMIPSState *envp, uint64_t start, uint64_t end)
{
    (void)envp;
    board_cache_invalidate(start, end);
}
uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];

static void board_poll(void)
{
    if (uart_irq_level()) {
        board_request_interrupt(&env, 1u << (CP0Ca_IP + 2));
    } else {
        env.CP0_Cause &= ~(1u << (CP0Ca_IP + 2));
    }
    (void)cpu_mips_get_count(&env);
    if (env.active_tc.CP0_TCHalt) {
        if ((env.CP0_Status & (1u << CP0St_IE)) &&
            !(env.CP0_Status & (1u << CP0St_EXL)) &&
            !(env.CP0_Status & (1u << CP0St_ERL)) &&
            ((env.CP0_Cause & env.CP0_Status) & 0xff00u)) {
            env.active_tc.CP0_TCHalt = 0;
        }
    }
    latm_check_interrupts(&env);
}

static void board_run_host_of(CPUMIPSState *e, unsigned cpu_id, uint8_t *host)
{
    board_cpu_id = cpu_id;
    /*
     * Rebind the translator globals to this CPU before anything is emitted or
     * run: lsenv/tr_data point at per-CPU objects (latm_env/latm_tr_data on the
     * CPU state), and tr_data holds the register-allocation state, the IR2
     * lists and the instruction map. One bind per dispatch is what makes two
     * vCPUs share this thread safely.
     */
    lata_tr_data_init(e);
    if (board_setjmp(board_exit_jmp) == 0) {
        board_exit_active = 1;
        ((void (*)(void *, void *))(uintptr_t)
         ((TRANSLATION_DATA *)e->latm_tr_data)->cs_bt_to_native)(e, host);
        board_exit_active = 0;
    } else {
        board_exit_active = 0;
    }
}

static void board_run_host(uint8_t *host) { board_run_host_of(&env, 0, host); }

static int run_one_tb_of(CPUMIPSState *e, TbCache *cache, uint8_t *buf,
                         uint32_t *next, uint32_t buf_size)
{
    uint64_t asid = board_asid_of(e);
    uint8_t mode = board_mode_of(e);
    TbCacheEntry *cached = tbc_lookup_mips(cache, e->active_tc.PC,
                                           (uint32_t)asid, mode);
    if (cached && cached->code) {
        board_tb_hits++;
        board_run_host_of(e, board_cpu_of(e), (uint8_t *)(uintptr_t)cached->code);
        return 1;
    }
    if (*next + BOARD_TB_SLOT > buf_size) {
        *next = BOARD_TB_START;
        latm_tb_insn_map_flush(e->latm_tr_data);
        tbc_flush_all(cache);
    }
    uint8_t *host = buf + *next;
    *next += BOARD_TB_SLOT;
    memset(&tb, 0, sizeof(tb));
    tb.pc = e->active_tc.PC;
    tb.tc.ptr = host;
    tb.jmp_reset_offset[0] = 0xffff;
    tb.jmp_reset_offset[1] = 0xffff;
    tcg_ctx->code_ptr = host;
    tcg_ctx->gen_insn_data = latx_tcg_gen_insn_data;
    int max = 16;
    target_disasm(&tb, &max, e);
    if (!tb.icount || !tr_ir2_generate(&tb)) { tr_fini(); return 0; }
    int n = tr_ir2_assemble(host);
    tr_fini();
    __asm__ volatile("ibar 0" ::: "memory");
    if (n <= 0) return 0;
    TbCacheEntry *entry = tbc_insert_mips(cache, tb.pc, (uint32_t)asid, mode);
    if (!entry) {
        tbc_flush_all(cache);
        entry = tbc_insert_mips(cache, tb.pc, (uint32_t)asid, mode);
    }
    if (entry) {
        entry->code = (uintptr_t)host;
        entry->code_size = (uint32_t)n;
        entry->guest_size = tb.size;
    }
    board_run_host_of(e, board_cpu_of(e), host);
    return 1;
}

static int run_one_tb(void)
{
    return run_one_tb_of(&env, &board_tb_cache, code_buf, &board_tb_next,
                         (uint32_t)sizeof(code_buf));
}

static void board_put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void board_put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int board_system_smoke(void)
{
    /* KSEG1 UART; this is executed through the LatmSystem platform, not the
     * legacy board_mem_map callbacks. */
    static const uint32_t insns[] = {
        0x3c08bfe0, 0x350801e0, 0x3409004b, 0xa1090000, 0x0000000d,
    };
    static LatmSystem sys;
    uint8_t elf[128];
    uint64_t entry = 0;
    const size_t phoff = 64, code_off = 120;
    memset(elf, 0, sizeof(elf));
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    elf[16] = 2; elf[18] = 8; elf[20] = 1;
    board_put64(elf + 24, 0x2000);
    board_put64(elf + 32, phoff);
    board_put32(elf + 20, 64);
    elf[52] = 64; elf[54] = 56; elf[56] = 1;
    board_put32(elf + phoff + 0, 1);
    board_put32(elf + phoff + 4, 5);
    board_put64(elf + phoff + 8, code_off);
    board_put64(elf + phoff + 16, 0x2000);
    board_put64(elf + phoff + 24, 0x2000);
    board_put64(elf + phoff + 32, sizeof(insns));
    board_put64(elf + phoff + 40, sizeof(insns));
    board_put64(elf + phoff + 48, 0x1000);
    for (unsigned i = 0; i < sizeof(insns) / sizeof(insns[0]); i++) {
        board_put32(elf + code_off + i * 4, insns[i]);
    }
    if (!latm_system_init_3a4000(&sys, ram, 0x100000000ull, sizeof(ram),
                                 0x100000000ull, 0x400000ull,
                                 0x100400000ull, 0x400000ull)) {
        latx_bare_uart_puts("latm-board: system-init-fail\n");
        return 0;
    }
    if (!latm_system_load_elf(&sys, elf, code_off + sizeof(insns), &entry)) {
        latx_bare_uart_puts("latm-board: system-elf-fail\n");
        return 0;
    }
    tbc_flush_all(&board_tb_cache);
    latm_tb_insn_map_flush(env.latm_tr_data);
    env.latm_fetch_fault = 0;
    env.active_tc.PC = entry | 0xffffffff80000000ull;
    if (!run_one_tb()) {
        latx_bare_uart_puts("latm-board: system-tb-fail\n");
        return 0;
    }
    if (sys.uart.tx_count != 1) {
        latx_bare_uart_puts("latm-board: system-uart-fail\n");
        return 0;
    }
    return 1;
}

void kmain(void)
{
    latx_bare_uart_puts("latm-board: begin\n");
    loongemu_mem_map_init(&mem_map);
    loongemu_mem_map_add(&mem_map, 0, RAM_SIZE, (uintptr_t)ram,
                         LOONGEMU_MEM_RAM, "ram");
    loongemu_mem_map_add(&mem_map, UART_GPA, 0x100, 0,
                         LOONGEMU_MEM_MMIO, "uart");
    loongemu_vclock_init(&vclock, LOONGEMU_VCLOCK_VIRTUAL_STEP,
                         loongemu_host_timer_hz(), 100000000, 100000000,
                         62500000, 0, 0);
    if (!tbc_init(&board_tb_cache, board_tb_entries, board_tb_buckets,
                  BOARD_TB_CACHE_CAP)) {
        latx_bare_uart_puts("latm-board: FAIL tb-cache init\n");
        return;
    }
    latm_platform_set(&platform);
    latm_set_system_memory(true);
    unsigned fdt_len = build_dtb();
    memcpy(ram + DTB_GPA, fdt, fdt_len);
    memcpy(ram, kernel_code, sizeof(kernel_code));
    memset(&env, 0, sizeof(env));
    env.SEGBITS = 42; env.PABITS = 36;
    latm_tlb_init(&env, &tlb, 16);
    env.exception_base = 0xffffffff80000000ull;
    env.CP0_IntCtl = 0xe0000000u;
    env.active_tc.PC = 0xffffffff80000000ull;
    lata_tr_data_init(&env);
    tcg_ctx->code_ptr = code_buf + 0x1000;
    lata_gen_prologue_env(&env, tcg_ctx);
    tcg_ctx->code_ptr = code_buf + 0x3000;
    lata_gen_epilogue_env(&env, tcg_ctx);
    /* ---------------------------------------------------------- two vCPUs --
     * Two CPU contexts, each with its own translator state, TB cache and host
     * code buffer, driven alternately through one thread. Everything below the
     * loop is asserted, not printed for a human.
     */
    if (tbc_init(&board_tb_cache1, board_tb_entries1, board_tb_buckets1,
                 BOARD_TB_CACHE_CAP)) {
        memcpy(ram + SMP_GUEST_GPA, smp_guest, sizeof(smp_guest));
        memset(ram + SMP_COUNTER_GPA, 0, 16);
        memset(&env1, 0, sizeof(env1));
        env1.SEGBITS = env.SEGBITS;
        env1.PABITS = env.PABITS;
        latm_tlb_init(&env1, &tlb1, 16);
        env1.exception_base = env.exception_base;
        env1.CP0_IntCtl = env.CP0_IntCtl;
        lata_tr_data_init(&env1);
        tcg_ctx->code_ptr = code_buf1 + 0x1000;
        lata_gen_prologue_env(&env1, tcg_ctx);
        tcg_ctx->code_ptr = code_buf1 + 0x3000;
        lata_gen_epilogue_env(&env1, tcg_ctx);
        env1.active_tc.PC = 0xffffffff80004000ull;
        env1.active_tc.gpr[4] = 1;
        env.active_tc.PC = 0xffffffff80004000ull;
        env.active_tc.gpr[4] = 0;
        env1.active_tc.CP0_TCHalt = 0;
        env.active_tc.CP0_TCHalt = 0;
        for (int i = 0; i < 16; i++) {
            run_one_tb();
            run_one_tb_of(&env1, &board_tb_cache1, code_buf1, &board_tb_next1,
                          (uint32_t)sizeof(code_buf1));
        }
        uint64_t c0 = 0, c1 = 0;
        memcpy(&c0, ram + SMP_COUNTER_GPA, 8);
        memcpy(&c1, ram + SMP_COUNTER_GPA + 8, 8);
        uint64_t smp_pc0 = env.active_tc.PC;
        uint64_t smp_pc1 = env1.active_tc.PC;
        int smp_ok = c0 == 16 && c1 == 16 &&
                     smp_pc0 == 0xffffffff80004014ull &&
                     smp_pc1 == 0xffffffff80004014ull;
        latx_bare_uart_puts(smp_ok ? "latm-smp: PASS\n" : "latm-smp: FAIL\n");
        latx_bare_uart_puts("latm-smp: pc0=");
        puthex(env.active_tc.PC);
        latx_bare_uart_puts(" pc1=");
        puthex(env1.active_tc.PC);
        latx_bare_uart_puts(" c0=");
        puthex(c0);
        latx_bare_uart_puts(" c1=");
        puthex(c1);
        latx_bare_uart_puts("\n");
        /* ---- slice 3: an IPI from CPU 0 must run CPU 1's handler ---- */
        memcpy(ram + 0x180, ipi_handler, sizeof(ipi_handler));
        memcpy(ram + 0x5000, ipi_sender, sizeof(ipi_sender));
        memset(ram + 0x1010, 0, 8);
        env.CP0_Status = 0;
        env.active_tc.PC = 0xffffffff80005000ull;
        (void)run_one_tb();
        uint32_t mailbox = (uint32_t)uncore.mailbox[1].status;
        uint32_t ipi_ip = 1u << (CP0Ca_IP + 4);
        env1.CP0_Cause = 0;
        env1.CP0_Status = (1u << CP0St_IE) | ipi_ip;
        if (mailbox & 1u) {
            board_request_interrupt(&env1, ipi_ip);
        }
        latm_check_interrupts(&env1);
        uint64_t handler_pc = env1.active_tc.PC;
        for (int i = 0; i < 6; i++) {
            (void)run_one_tb_of(&env1, &board_tb_cache1, code_buf1,
                                &board_tb_next1, (uint32_t)sizeof(code_buf1));
            /*
             * The IPI is level-triggered: once the handler has cleared the
             * mailbox bit, the CPU must stop seeing the interrupt. Without this
             * deassert the hart re-enters the handler on every dispatch while
             * Cause.IP4 stays set.
             */
            if ((uncore.mailbox[1].status & 1u) == 0) {
                env1.CP0_Cause &= ~ipi_ip;
            }
        }
        uint64_t loop_after = 0;
        memcpy(&loop_after, ram + 0x1008, 8);
        uint64_t handled = 0;
        memcpy(&handled, ram + 0x1010, 8);
        latx_bare_uart_puts("latm-ipi: mailbox=");
        puthex(mailbox);
        latx_bare_uart_puts(" vector-pc=");
        puthex(handler_pc);
        latx_bare_uart_puts(" handled=");
        puthex(handled);
        latx_bare_uart_puts(" status=");
        puthex(uncore.mailbox[1].status);
        latx_bare_uart_puts(" loop=");
        puthex(loop_after);
        latx_bare_uart_puts("\n");
        /*
         * Asserted, now exactly: the guest's IOCSR write reached the model, the
         * interrupt vector was 0x180 for CPU 1, the handler ran **once** and
         * cleared the mailbox through IOCSR, the mailbox bit is gone, the hart
         * is back at the instruction it was interrupted at, and it has resumed
         * its own loop (its counter moved past the 16 it had before the IPI).
         */
        int ipi_ok = (mailbox & 1u) != 0 &&
                     handler_pc == 0xffffffff80000180ull &&
                     handled == 1 &&
                     loop_after > 16 &&
                     env1.CP0_EPC == 0xffffffff80004014ull &&
                     (uncore.mailbox[1].status & 1u) == 0;
        latx_bare_uart_puts(ipi_ok ? "latm-ipi: PASS\n" : "latm-ipi: FAIL\n");

    }

    for (int i = 0; i < 8 && !board_exit_code; i++) {
        board_poll();
        if (board_exit_code) break;
        if (env.active_tc.CP0_TCHalt) {
            loongemu_vclock_advance_ns(&vclock, 100);
            continue;
        }
        if (!run_one_tb()) break;
        loongemu_vclock_advance_ns(&vclock, 100);
    }
    /* TB link cache: ASID change must invalidate the old host mapping. */
    env.CP0_EntryHi = 0;
    latm_jmp_cache_add(&env, 0xffffffff80000000ull, 0xdead0000ull, 4);
    uint64_t cache_hit = latm_jmp_cache_lookup(&env,
                                              0xffffffff80000000ull);
    latm_helper_cp0_write(&env, 10, 0, 0x22);
    uint64_t cache_miss = latm_jmp_cache_lookup(&env,
                                               0xffffffff80000000ull);
    latx_bare_uart_puts("\nlatm-board: tb-cache-hits=");
    puthex(board_tb_hits);
    latx_bare_uart_puts(" tb-cache-count=");
    puthex(tbc_count(&board_tb_cache));
    latx_bare_uart_puts("\nlatm-board: cache-hit=");
    puthex(cache_hit);
    latx_bare_uart_puts(" cache-after-asid=");
    puthex(cache_miss);

    /* Guest-visible 16550 register path: DLAB, divisor, LCR, IER, LSR. */
    memcpy(ram + 0x3000, uart_code, sizeof(uart_code));
    board_exit_code = 0;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    env.active_tc.PC = 0xffffffff80003000ull;
    (void)run_one_tb();
    latx_bare_uart_puts("\nlatm-board: uart-lsr=");
    puthex(env.active_tc.gpr[10]);
    latx_bare_uart_puts(" uart-ier=");
    puthex(uart.ier);

    /* 16550 RX FIFO path through the guest MMIO load. */
    uart_rx_push('R');
    ((uint32_t *)ram)[0x6000 / 4 + 0] = 0x3c08bf00u; /* lui $t0,0xbf00 */
    ((uint32_t *)ram)[0x6000 / 4 + 1] = 0x810a0000u; /* lb $t2,0($t0) */
    ((uint32_t *)ram)[0x6000 / 4 + 2] = 0x1000ffffu;
    ((uint32_t *)ram)[0x6000 / 4 + 3] = 0x00000000u;
    board_exit_code = 0;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    env.active_tc.PC = 0xffffffff80006000ull;
    (void)run_one_tb();
    latx_bare_uart_puts(" uart-rx=");
    puthex(env.active_tc.gpr[10]);

    /* Timer/WAIT wakeup: arm Compare behind the free-running vclock count. */
    latx_bare_uart_puts("\n");
    board_exit_code = 0;
    env.CP0_Status = (1u << CP0St_IE) | (1u << (CP0Ca_IP + 7));
    env.CP0_Cause = 0;
    env.active_tc.CP0_TCHalt = 1;
    compare_value = 1;
    compare_armed = 1;
    env.CP0_Compare = compare_value;
    env.active_tc.PC = 0xffffffff80000000ull;
    board_poll();
    latx_bare_uart_puts("\nlatm-board: timer-pc=");
    puthex(env.active_tc.PC);
    latx_bare_uart_puts(" timer-exit=");
    puthex((uint64_t)board_exit_code);

    board_exit_code = 0;
    compare_armed = 0;
    env.CP0_Compare = 0;
    env.CP0_Status = (1u << CP0St_IE) | (1u << (CP0Ca_IP + 2));
    env.CP0_Cause = 0;
    env.active_tc.CP0_TCHalt = 0;
    uart.ier |= 0x02;
    uart.tx_pending = 1;
    board_poll();
    latx_bare_uart_puts("\nlatm-board: uart-irq-pc=");
    puthex(env.active_tc.PC);
    latx_bare_uart_puts(" uart-irq-exit=");
    puthex((uint64_t)board_exit_code);

    /* Precise exception: BREAK helper's GETPC() must map back to this PC. */
    ((uint32_t *)ram)[0x1000 / 4] = 0x0000000du;
    board_exit_code = 0;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    env.active_tc.PC = 0xffffffff80001000ull;
    (void)run_one_tb();
    latx_bare_uart_puts("\nlatm-board: break-epc=");
    puthex(env.CP0_EPC);
    latx_bare_uart_puts(" break-cause=");
    puthex((env.CP0_Cause >> 2) & 0x1f);
    latx_bare_uart_puts(" break-exit=");
    puthex((uint64_t)board_exit_code);

    /* Delay-slot precise exception: EPC must be the branch PC with BD=1. */
    ((uint32_t *)ram)[0x2000 / 4 + 0] = 0x10000001u; /* beq $0,$0,+1 */
    ((uint32_t *)ram)[0x2000 / 4 + 1] = 0x0000000du; /* break in delay */
    ((uint32_t *)ram)[0x2000 / 4 + 2] = 0x00000000u;
    board_exit_code = 0;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    env.active_tc.PC = 0xffffffff80002000ull;
    (void)run_one_tb();
    latx_bare_uart_puts("\nlatm-board: delay-epc=");
    puthex(env.CP0_EPC);
    latx_bare_uart_puts(" delay-bd=");
    puthex((env.CP0_Cause >> 31) & 1);
    latx_bare_uart_puts(" delay-exit=");
    puthex((uint64_t)board_exit_code);

    /* Precise memory fault: the system-memory helper must report its PC. */
    ((uint32_t *)ram)[0x4000 / 4 + 0] = 0x3c089000u; /* lui $t0,0x9000 */
    ((uint32_t *)ram)[0x4000 / 4 + 1] = 0x8d090000u; /* lw $t1,0($t0) */
    ((uint32_t *)ram)[0x4000 / 4 + 2] = 0x1000ffffu;
    ((uint32_t *)ram)[0x4000 / 4 + 3] = 0x00000000u;
    board_exit_code = 0;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    env.active_tc.PC = 0xffffffff80004000ull;
    (void)run_one_tb();
    latx_bare_uart_puts("\nlatm-board: load-epc=");
    puthex(env.CP0_EPC);
    latx_bare_uart_puts(" load-cause=");
    puthex((env.CP0_Cause >> 2) & 0x1f);
    latx_bare_uart_puts(" load-exit=");
    puthex((uint64_t)board_exit_code);

    /* Actual guest WAIT instruction: halt, then timer wakes and vectors. */
    ((uint32_t *)ram)[0x5000 / 4] = 0x42000020u; /* wait */
    uart.ier = 0;
    uart.tx_pending = 0;
    board_exit_code = 0;
    env.CP0_Status = (1u << CP0St_IE) | (1u << (CP0Ca_IP + 7));
    env.CP0_Cause = 0;
    compare_value = board_read_count(&env, 50000000) + 1;
    compare_armed = 1;
    env.CP0_Compare = compare_value;
    env.active_tc.PC = 0xffffffff80005000ull;
    (void)run_one_tb();
    uint64_t wait_halt = env.active_tc.CP0_TCHalt;
    loongemu_vclock_advance_ns(&vclock, 100);
    board_poll();
    latx_bare_uart_puts("\nlatm-board: wait-halt=");
    puthex(wait_halt);
    latx_bare_uart_puts(" wait-pc=");
    puthex(env.active_tc.PC);
    latx_bare_uart_puts(" wait-exit=");
    puthex((uint64_t)board_exit_code);

    latx_bare_uart_puts("\nlatm-board: uart=");
    latx_bare_uart_puts(uart_log);
    latx_bare_uart_puts("\nlatm-board: fdt=");
    puthex(((uint32_t)fdt[0] << 24) | ((uint32_t)fdt[1] << 16) |
           ((uint32_t)fdt[2] << 8) | fdt[3]);
    latx_bare_uart_puts(" dtb-gpa=");
    puthex(DTB_GPA);
    latx_bare_uart_puts(" dtb-len=");
    puthex(fdt_len);
    latx_bare_uart_puts("\nlatm-board: system-first-tb=");
    puthex(board_system_smoke());
    latx_bare_uart_puts("\nlatm-board: done\n");
}
void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(kstack)\n  addi.d $sp, $sp, %pc_lo12(kstack)\n  lu12i.w $t0, 16\n"
        "  add.d $sp, $sp, $t0\n  bl kmain\n1:\n  b 1b\n");
static char kstack[65536] __attribute__((aligned(16), used));
FILE *stderr;
