/*
 * LoongArch LVZ / privileged-CSR probe for QEMU.
 *
 * QEMU may implement LBT state save/restore without executing LBT opcodes, and
 * may or may not implement the LVZ CSRs.  This probe installs its own EENTRY,
 * issues csrrd/csrwr for a fixed list of CSRs, and records which ones trap
 * (#INE) versus return a value.  All writes use value 0 so guest mode is never
 * entered.
 */
#include <stdint.h>
#include <stddef.h>

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)

static void putc_(char c) { *UART0_THR = (unsigned char)c; }
static void puts_(const char *s) { while (*s) { putc_(*s++); } }
static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int sh;

    puts_("0x");
    for (sh = 60; sh >= 0; sh -= 4) {
        putc_(d[(v >> sh) & 0xf]);
    }
}

/* CSRs under test (numbers from the LoongArch manual / Linux header). */
enum {
    CSR_CRMD = 0x0, CSR_EUEN = 0x2, CSR_ECFG = 0x4, CSR_ESTAT = 0x5,
    CSR_ERA = 0x6, CSR_BADI = 0x8, CSR_EENTRY = 0xc,
    CSR_GTLBC = 0x15, CSR_GSTAT = 0x50, CSR_TLBRENTRY = 0x88,
};

static volatile int fault_csr = -1;
static volatile int fault_count;
static volatile int lbt_fault;
static volatile uint32_t lbt_word = 0x003f0007u;
static volatile int hvc_count;
static volatile int guest_mode_seen;
volatile uint64_t host_scr;
volatile uint64_t pre_scr;
volatile uint64_t guest_scr;

extern void lvz_entry(void);
extern char lvz_host_after[];
extern void lvz_guest_probe(void);

void lvz_handle(uint64_t *regs)
{
    uint64_t era, estat;
    uint32_t w, op;

    (void)regs;
    __asm__ volatile("csrrd %0, 0x6" : "=r"(era));
    __asm__ volatile("csrrd %0, 0x5" : "=r"(estat));

    if (((estat >> 16) & 0x3f) == 0 && (estat & 0x800)) {
        __asm__ volatile("csrwr %0, 0x44" :: "r"((uint64_t)1)); /* TICLR */
        return;
    }
    if (((estat >> 16) & 0x3f) == 0x17) {
        uint64_t gstat = 0;
        hvc_count++;
        __asm__ volatile("csrwr %0, 0x6" :: "r"((uint64_t)(uintptr_t)lvz_host_after));
        __asm__ volatile("csrrd %0, 0x50" : "=r"(gstat));
        gstat &= ~(uint64_t)2;
        __asm__ volatile("csrwr %0, 0x50" :: "r"(gstat));
        return;
    }
    if (((estat >> 16) & 0x3f) != 0x0d) {
        puts_("probe: unexpected trap estat=");
        puthex(estat);
        puts_("\n");
        for (;;) {
        }
    }
    w = *(volatile uint32_t *)(uintptr_t)era;
    op = w >> 24;
    if (w == lbt_word) {
        lbt_fault++;
        __asm__ volatile("csrwr %0, 0x6" :: "r"(era + 4));
        return;
    }
    if (op == 0x04 || op == 0x24 || op == 0xa4) {
        uint32_t csr = (w >> 10) & 0x3fff;

        fault_csr = (int)csr;
        fault_count++;
        __asm__ volatile("csrwr %0, 0x6" :: "r"(era + 4));
        return;
    }
    puts_("probe: unexpected #INE word=");
    puthex(w);
    puts_("\n");
    for (;;) {
    }
}

__asm__(
".text\n"
".globl lvz_guest_probe\n"
"lvz_guest_probe:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d $ra, $sp, 0\n"
"  lu12i.w $t0, 0xa\n"
"  ori $t0, $t0, 0xaaa\n"
"  .word 0x00000980\n"
"  .word 0x00000c0d\n"
"  la $t2, pre_scr\n"
"  st.d $t1, $t2, 0\n"
"  la $t0, lvz_guest_entry\n"
"  csrwr $t0, 0x6\n"
"  li.d $t0, 8\n"
"  .word 0x0500002c\n"
"  csrrd $t0, 0x50\n"
"  ori $t1, $zero, 2\n"
"  or $t0, $t0, $t1\n"
"  csrwr $t0, 0x50\n"
"  ertn\n"
".globl lvz_host_after\n"
"lvz_host_after:\n"
"  .word 0x00000c0c\n"
"  la $t1, host_scr\n"
"  st.d $t0, $t1, 0\n"
"  ld.d $ra, $sp, 0\n"
"  addi.d $sp, $sp, 16\n"
"  jirl $zero, $ra, 0\n"
".globl lvz_guest_entry\n"
"lvz_guest_entry:\n"
"  lu12i.w $t0, 0x1\n"
"  ori $t0, $t0, 0x234\n"
"  .word 0x00000980\n"
"  .word 0x00000c0d\n"
"  la $t2, guest_scr\n"
"  st.d $t1, $t2, 0\n"
"  addi.d $r12, $r12, 1\n"
"  .word 0x002b8000\n"
"  b lvz_guest_entry\n"
".globl lvz_entry\n"
"lvz_entry:\n"
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
"  bl lvz_handle\n"
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

#define TRY_READ(NAME, NUM)                                             \
    do {                                                                \
        uint64_t v = 0;                                                 \
        fault_csr = -1;                                                 \
        __asm__ volatile("csrrd %0, " #NUM : "=r"(v));                  \
        puts_("probe: " NAME " rd ");                                   \
        if (fault_csr == (NUM)) {                                       \
            puts_("FAULT\n");                                           \
        } else {                                                        \
            puts_("ok=");                                               \
            puthex(v);                                                  \
            puts_("\n");                                                \
        }                                                               \
    } while (0)

#define TRY_WRITE(NAME, NUM)                                            \
    do {                                                                \
        fault_csr = -1;                                                 \
        __asm__ volatile("csrwr %0, " #NUM :: "r"((uint64_t)0));        \
        puts_("probe: " NAME " wr0 ");                                  \
        puts_(fault_csr == (NUM) ? "FAULT\n" : "ok\n");                 \
    } while (0)

int efi_main(void *image, void *systab)
{
    (void)image;
    (void)systab;

    puts_("lvz-probe: start\n");
    __asm__ volatile("csrwr %0, 0xc" :: "r"((uint64_t)lvz_entry));
    {
        uint64_t cfg2 = 0, two = 2;
        __asm__ volatile("cpucfg %0, %1" : "=r"(cfg2) : "r"(two));
        puts_("probe: cpucfg2=");
        puthex(cfg2);
        puts_(" LSX=");
        puthex((cfg2 >> 6) & 1);
        puts_(" LASX=");
        puthex((cfg2 >> 7) & 1);
        puts_(" LVZP=");
        puthex((cfg2 >> 10) & 1);
        puts_(" verdict=");
        puts_(((cfg2 >> 10) & 1) ? "LVZ=advertised" : "LVZ=ABSENT");
        puts_("\n");
        puts_("probe: LBT_X86=");
        puthex((cfg2 >> 18) & 1);
        puts_(" LBT_ARM=");
        puthex((cfg2 >> 19) & 1);
        puts_(" LBT_MIPS=");
        puthex((cfg2 >> 20) & 1);
        puts_("\n");
    }

    /* Probe whether the LBT_X86 opcode space is decoded (x86add.d). */
    lbt_fault = 0;
    __asm__ volatile(".word 0x003f0007");
    {
        uint64_t cfg2b = 0, two2 = 2;
        __asm__ volatile("cpucfg %0, %1" : "=r"(cfg2b) : "r"(two2));
        if ((cfg2b >> 10) & 1) {
        hvc_count = 0;
        guest_mode_seen = 0;
        lvz_guest_probe();
        puts_("probe: lvz-guest hvcl=");
        puthex((uint64_t)hvc_count);
        puts_(" pre=");
        puthex(pre_scr);
        puts_(" gscr=");
        puthex(guest_scr);
        puts_(" hscr=");
        puthex(host_scr);
        puts_("\n");
        }
    }

    puts_("probe: lbt-insn=");
    puts_(lbt_fault ? "#INE(unsupported)" : "EXECUTED");
    puts_("\n");

    TRY_READ("EUEN", 0x2);
    TRY_READ("ESTAT", 0x5);
    TRY_READ("BADI", 0x8);
    TRY_READ("TLBRENTRY", 0x88);
    TRY_READ("GTLBC", 0x15);
    TRY_READ("GSTAT", 0x50);

    TRY_WRITE("GTLBC", 0x15);
    TRY_WRITE("GSTAT", 0x50);

    TRY_READ("GSTAT", 0x50);

    TRY_READ("CSR0x3f", 0x3f);
    TRY_READ("CSR0x7f", 0x7f);
    TRY_READ("CSR0x51", 0x51);
    TRY_READ("CSR0x98", 0x98);
    puts_("probe: csr-trap=");
    puts_(fault_count ? "traps" : "PERMISSIVE(unknown CSRs read as 0)");
    puts_("\n");
    puts_("lvz-probe: faults=");
    puthex((uint64_t)fault_count);
    puts_("\nlvz-probe: done\n");
    return 0;
}
