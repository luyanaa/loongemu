/*
 * P0 bare-metal probe: 4 KiB paging, a PLV3 page fault, the PLV0 walk that
 * services it, TLBFILL, and the resume.
 *
 * This is the fast iteration target for the UEFI VMM entry: the same
 * sequence the VMM performs after ExitBootServices, but booted straight into
 * QEMU with -kernel so a cycle costs seconds instead of a firmware boot.
 *
 * The mechanics:
 *
 *   - Paging is enabled with CRMD.PG=1, DA=0.  Afterwards an address
 *     resolves either through a DMW window (if VA[63:60] matches VSEG and
 *     the current PLV's bit is set) or through the TLB.
 *   - DMW0 gets VSEG=0 and only the PLV0 bit, so this code -- which lives
 *     low in RAM, segment 0, running at PLV0 -- keeps executing across the
 *     switch while a PLV3 access is not covered and must use the TLB.
 *   - The probe address sits in segment 1, which no DMW covers, so its
 *     absence from the TLB is guaranteed to fault rather than being an
 *     accident of where the page happened to be.
 *   - The handler walks the table with the same lapg_walk() the host unit
 *     tests exercise, and fills the entry the walk produced.
 */

#include <stdint.h>

#include "loongemu/loongarch_paging.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)
#define UART0_LSR ((volatile unsigned char *)0x1fe001e5)

static void pc(char c)
{
    unsigned spin = 0;

    while (!(*UART0_LSR & 0x20)) {
        if (++spin > 100000u) {
            break;
        }
    }
    *UART0_THR = (unsigned char)c;
}

static void ps(const char *s) { while (*s) { pc(*s++); } }

static void ph(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int i;

    ps("0x");
    for (i = 15; i >= 0; i--) {
        pc(d[(v >> (i * 4)) & 0xf]);
    }
}

#define LOG(m) do { ps("p0: "); ps(m); ps("\n"); } while (0)

/* ESTAT.Ecode values used here. */
#define EXC_PIL 0x1
#define EXC_PIS 0x2
#define EXC_PIF 0x3
#define EXC_IPE 0x0d

/*
 * The probe address.  QEMU masks a VA to 48 bits before the TLB lookup
 * (TARGET_VIRT_MASK), so BADV reports VA[47:0] -- a segment-1 address comes
 * back as its low half.  The table therefore has to be indexed by exactly
 * that value, and PROBE_VA is kept inside the 48-bit range to make the
 * address the walker sees identical to the one written here.
 *
 * Nothing maps this page on purpose: the load must fault.
 */
#define PROBE_VA UINT64_C(0x0000000010000000)
/* What the walker-produced entry maps that VA onto. */
#define PROBE_PA_OFFSET UINT64_C(0x0000000000100000)

/* ---- pool and tables -------------------------------------------------- */

static uint64_t g_pool_mem[16 * (LAPG_PAGE_SIZE / 8)]
    __attribute__((aligned(LAPG_PAGE_SIZE)));
static uint64_t g_probe_page[LAPG_PAGE_SIZE / 8]
    __attribute__((aligned(LAPG_PAGE_SIZE)));

static LapgPagePool g_pool;
static uint64_t g_root;

static volatile int g_fault_count;
static volatile int g_recovered;
static volatile uint64_t g_last_ecode;
static volatile uint64_t g_last_badv;
static volatile uint64_t g_probe_value;

/*
 * On this direct-mapped bring-up the image's own addresses are its physical
 * addresses, so the memory callbacks can hand back plain pointers.  The
 * UEFI entry uses a DMW-based translation for the same interface.
 */
static int mem_read(void *opaque, uint64_t phys, uint64_t *value)
{
    (void)opaque;
    *value = *(volatile uint64_t *)(uintptr_t)phys;
    return 1;
}

static int mem_write(void *opaque, uint64_t phys, uint64_t value)
{
    (void)opaque;
    *(volatile uint64_t *)(uintptr_t)phys = value;
    return 1;
}

static LapgMemory g_memory = { .opaque = 0, .read_u64 = mem_read,
                               .write_u64 = mem_write };

/* ---- CSR -------------------------------------------------------------- */

static inline uint64_t csrrd(unsigned int n)
{
    uint64_t v = 0;

    switch (n) {
#define R(x) case x: __asm__ volatile("csrrd %0, " #x : "=r"(v)); break;
    R(0x0) R(0x1) R(0x4) R(0x5) R(0x6) R(0x7) R(0xc) R(0x1c) R(0x1d) R(0x1e)
    R(0x10) R(0x11) R(0x12) R(0x13) R(0x18) R(0x180) R(0x181) R(0x89) R(0x8a)
#undef R
    default: break;
    }
    return v;
}

static inline void csrwr(unsigned int n, uint64_t v)
{
    switch (n) {
#define W(x) \
    case x: __asm__ volatile("move $r12,%0\n csrwr $r12," #x :: "r"(v) : "r12", "memory"); break;
    W(0x0) W(0x1) W(0x4) W(0x6) W(0xc) W(0x88) W(0x1c) W(0x1d) W(0x1e)
    W(0x10) W(0x11) W(0x12) W(0x13) W(0x18) W(0x180) W(0x181) W(0x182) W(0x183)
    W(0x8a) W(0x8c) W(0x8d) W(0x8e)
#undef W
    default: break;
    }
}

void kmain(void);
void p0_trap(void);
void p0_probe(void);
void p0_fault_load(void);
void p0_trap_back(void);
void p0_done(void);
extern char p0_resume_plv0[];

/* ---- trap handler ----------------------------------------------------- */

/*
 * Entered at PLV0 with the interrupted register state saved.  A page-invalid
 * exception is serviced by walking the table and filling the TLB; an IPE
 * from the probe means the probe finished, so the handler rewrites PRMD to
 * return to PLV0 and points ERA at the C continuation.
 */
void p0_handle(uint64_t *regs)
{
    uint64_t estat = csrrd(0x5);
    uint64_t ecode = (estat >> 16) & 0x3f;
    uint64_t badv = csrrd(0x7);
    uint64_t tlbrera = csrrd(0x8a);

    (void)regs;
    g_fault_count++;

    /*
     * A TLB refill does not fill ESTAT.ECODE at all (cpu.c writes only
     * TLBRERA/TLBRPRMD on that path), so ESTAT alone cannot tell a refill
     * from an interrupt whose ecode is 0.  TLBRERA.ISTLBR (bit 0) is the
     * discriminator, and on that path the faulting address is TLBRBADV.
     */
    if (tlbrera & 1) {          /* TLBRERA.ISTLBR is bit 0 */
        badv = csrrd(0x89);
    }

    g_last_ecode = ecode;
    g_last_badv = badv;

    if ((tlbrera & 1) ||
        ecode == EXC_PIL || ecode == EXC_PIS || ecode == EXC_PIF) {
        LapgTranslation tr;
        LapgResult r;
        /* A refill can be a load, store or fetch.  TLBRERA.ISTLBR does not
         * carry the access type, so ask CRMD/ERA instead: a fetch fault has
         * the PC equal to the faulting address. */
        /*
         * On the refill path ESTAT carries no ecode at all, so "ecode == 0"
         * must not be read as "a load": that aliases with EXCCODE_INT and,
         * worse, silently selects the wrong access type.  The faulting
         * instruction has to be decoded from memory instead.  The probe is a
         * load, and a fetch fault is identifiable because the PC equals the
         * failing address.
         */
        uint64_t era = csrrd(0x6);
        uint64_t index, vppn;
        uint32_t access;

        /*
         * On the refill path ERA is not the faulting instruction: TLBRERA
         * holds it, in its PC field (bits [63:2], byte-aligned).  Reading
         * ERA here would decode whatever instruction happened to be there.
         * The fetch/data distinction then comes from whether the reported
         * PC equals the bad address.
         */
        {
            uint64_t pc = (uint64_t)(tlbrera >> 2) << 2;
            uint32_t insn;
            uint32_t major;

            if (pc == badv) {
                access = LAPG_PERM_EXEC | LAPG_PERM_USER;
            } else {
                insn = *(volatile uint32_t *)(uintptr_t)pc;
                major = (insn >> 26) & 0x3f;
                /*
                 * ld.d and st.d both use major 0x0a; bit 25 of the
                 * instruction is 0 for the load forms and 1 for the store
                 * forms.  Other ld/st majors (0x25/0x27 wide forms, 0x0b-0x0e
                 * float/vector) are not used by this probe.
                 */
                if (major >= 0x0b && major <= 0x0e) {
                    access = LAPG_PERM_READ | LAPG_PERM_USER;
                } else {
                    access = ((insn >> 25) & 1)
                           ? (LAPG_PERM_WRITE | LAPG_PERM_USER)
                           : (LAPG_PERM_READ | LAPG_PERM_USER);
                }
            }
        }
        (void)era;

        r = lapg_walk(&g_memory, g_root, badv, access, &tr);
        if (r != LAPG_OK) {
            {
                uint32_t insn = *(volatile uint32_t *)(uintptr_t)csrrd(0x6);
                ps("p0: era="); ph(csrrd(0x6));
                ps(" insn="); ph((uint64_t)insn);
                ps(" major="); ph((uint64_t)((insn >> 26) & 0x3f));
                ps(" tlbrera="); ph(csrrd(0x8a));
                ps("\n");
            }
            ps("p0: walk perm=");
            ph((uint64_t)tr.perm);
            ps(" access=");
            ph((uint64_t)access);
            ps(" pte=");
            ph(tr.pte);
            ps("\n");
            ps("p0: walk failed r=");
            ph((uint64_t)r);
            ps(" va=");
            ph(badv);
            ps("\n");
            for (;;) {
            }
        }

        /* TLBFILL derives the column itself; TLBIDX only carries PS, which
         * the fill source (TLBREHI.PS) already has.  Nothing to program. */
        (void)index;

        /*
         * A refill exception runs with TLBRERA.ISTLBR set, and in that state
         * fill_tlb_entry() takes its operands from the *TLBR* group
         * (TLBREHI/TLBRELO0/TLBRELO1) rather than TLBEHI/TLBELO0/TLBELO1.
         * Writing the normal CSRs here leaves the fill sources at zero, so
         * the entry arrives with V=0 and the refill repeats forever.
         */
        vppn = badv & UINT64_C(0x0000ffffffffe000);
        if (csrrd(0x8a) & 1) {
            /*
             * TLBREHI carries PS in its low six bits, and tlbfill compares
             * that against CSR.STLBPS to decide between STLB and MTLB.  With
             * PS left at zero it never matches and the entry is dropped into
             * MTLB at a random line, which is why the refill repeated.
             */
            csrwr(0x8e, vppn | (uint64_t)LAPG_PAGE_BITS);
            csrwr(0x8c, tr.pte);
            csrwr(0x8d, tr.pte);
        } else {
            csrwr(0x11, vppn);
            csrwr(0x12, tr.pte);
            csrwr(0x13, tr.pte);
        }
        /*
         * TLBFILL instead of TLBWR: TLBFILL picks the way itself and derives
         * the column from the entry, so it cannot disagree with the search
         * the way an explicit index can.  The probe needs the entry present,
         * not a specific way.
         */
        __asm__ volatile("tlbfill" ::: "memory");

        g_recovered++;
        return;
    }

    /*
     * PLV3 may not touch the CSRs.  The probe finishes with a csrrd there,
     * which raises IPE (0x0d) or, when the instruction is treated as a
     * guest-sensitive resource, GSPR (0x0e).  Either way it is the signal
     * that the probe is done, so return to PLV0 at the C continuation.
     */
    if (ecode == EXC_IPE || ecode == 0x0e) {
        /* The PLV3 probe tried a privilege instruction: it is done.  Return
         * to PLV0 at the C continuation. */
        uint64_t prmd = csrrd(0x1);

        prmd &= ~UINT64_C(0x3);            /* PRMD.PLV := 0 */
        csrwr(0x1, prmd);
        csrwr(0x6, (uint64_t)(uintptr_t)p0_resume_plv0);
        return;
    }

    ps("p0: unexpected ecode=");
    ph(ecode);
    ps(" badv=");
    ph(badv);
    ps("\n");
    for (;;) {
    }
}

/*
 * Trap frame: spill the GPRs the handler can touch, call C, restore, ERTN.
 * $sp is intentionally left as the interrupted value so ERTN returns into
 * the probe with its own stack.
 */
__asm__(
".text\n"
".globl p0_trap\n"
"p0_trap:\n"
"  addi.d $sp, $sp, -272\n"
"  st.d $ra, $sp, 0\n"
"  st.d $r12, $sp, 8\n"
"  st.d $r13, $sp, 16\n"
"  st.d $r14, $sp, 24\n"
"  st.d $r15, $sp, 32\n"
"  st.d $r16, $sp, 40\n"
"  st.d $r17, $sp, 48\n"
"  st.d $r18, $sp, 56\n"
"  st.d $r19, $sp, 64\n"
"  st.d $r20, $sp, 72\n"
"  st.d $r21, $sp, 80\n"
"  st.d $r22, $sp, 88\n"
"  st.d $r23, $sp, 96\n"
"  st.d $r24, $sp, 104\n"
"  st.d $r25, $sp, 112\n"
"  st.d $r26, $sp, 120\n"
"  st.d $r27, $sp, 128\n"
"  st.d $r28, $sp, 136\n"
"  st.d $r29, $sp, 144\n"
"  st.d $r30, $sp, 152\n"
"  st.d $r31, $sp, 160\n"
"  st.d $r4,  $sp, 168\n"
"  st.d $r5,  $sp, 176\n"
"  st.d $r6,  $sp, 184\n"
"  st.d $r7,  $sp, 192\n"
"  st.d $r8,  $sp, 200\n"
"  st.d $r9,  $sp, 208\n"
"  st.d $r10, $sp, 216\n"
"  st.d $r11, $sp, 224\n"
"  move $a0, $sp\n"
"  bl p0_handle\n"
"  ld.d $ra, $sp, 0\n"
"  ld.d $r12, $sp, 8\n"
"  ld.d $r13, $sp, 16\n"
"  ld.d $r14, $sp, 24\n"
"  ld.d $r15, $sp, 32\n"
"  ld.d $r16, $sp, 40\n"
"  ld.d $r17, $sp, 48\n"
"  ld.d $r18, $sp, 56\n"
"  ld.d $r19, $sp, 64\n"
"  ld.d $r20, $sp, 72\n"
"  ld.d $r21, $sp, 80\n"
"  ld.d $r22, $sp, 88\n"
"  ld.d $r23, $sp, 96\n"
"  ld.d $r24, $sp, 104\n"
"  ld.d $r25, $sp, 112\n"
"  ld.d $r26, $sp, 120\n"
"  ld.d $r27, $sp, 128\n"
"  ld.d $r28, $sp, 136\n"
"  ld.d $r29, $sp, 144\n"
"  ld.d $r30, $sp, 152\n"
"  ld.d $r31, $sp, 160\n"
"  ld.d $r4,  $sp, 168\n"
"  ld.d $r5,  $sp, 176\n"
"  ld.d $r6,  $sp, 184\n"
"  ld.d $r7,  $sp, 192\n"
"  ld.d $r8,  $sp, 200\n"
"  ld.d $r9,  $sp, 208\n"
"  ld.d $r10, $sp, 216\n"
"  ld.d $r11, $sp, 224\n"
"  addi.d $sp, $sp, 272\n"
"  ertn\n"
);

/*
 * The PLV3 probe.  Drops to PLV3 through PRMD/ERTN, loads from the unmapped
 * page, stores the value, then executes a CSR instruction -- which is an
 * IPE at PLV3 -- so the handler gets a second trap to restore PLV0.
 */
__asm__(
".text\n"
".globl p0_probe\n"
"p0_probe:\n"
/* Use a dedicated stack inside the identity-mapped low window so the
 * PLV3 probe does not fault on the caller's stack before reaching the
 * load it is meant to test. */
"  la $sp, p0_stack\n"
"  lu12i.w $t0, 16\n"
"  add.d $sp, $sp, $t0\n"
"  addi.d $sp, $sp, -32\n"
"  st.d $ra, $sp, 0\n"
"  st.d $r4, $sp, 8\n"
/* PRMD.PLV := 3 so the ERTN below lands in user mode */
"  csrrd $t0, 0x1\n"
"  srli.d $t0, $t0, 2\n"
"  slli.d $t0, $t0, 2\n"
"  ori $t0, $t0, 0x3\n"
"  csrwr $t0, 0x1\n"
/* ERA := faulting load */
"  la $t0, p0_fault_load\n"
"  csrwr $t0, 0x6\n"
"  ertn\n"
".globl p0_fault_load\n"
"p0_fault_load:\n"
"  lu12i.w $t1, 0x10000\n"
"  ld.d $t2, $t1, 0\n"
"  st.d $t2, $sp, 8\n"
/* this faults with IPE at PLV3 and unwinds back to PLV0 */
".globl p0_trap_back\n"
"p0_trap_back:\n"
"  csrrd $t3, 0x20\n"
"  ld.d $ra, $sp, 0\n"
"  ld.d $r4, $sp, 8\n"
"  addi.d $sp, $sp, 32\n"
"  jirl $zero, $ra, 0\n"
);

/*
 * PLV0 continuation.  The IPE trap rewrote PRMD.PLV to 0 and pointed ERA
 * here, so ERTN resumes at PLV0 with the probe's stack already unwound.
 */
__asm__(
".text\n"
".globl p0_resume_plv0\n"
"p0_resume_plv0:\n"
/* $sp is the probe's own frame; the loaded value is at +8 (where
 * p0_fault_load stored $t2). */
"  ld.d $t0, $sp, 8\n"
"  la $t1, g_probe_value\n"
"  st.d $t0, $t1, 0\n"
"  jirl $zero, $ra, 0\n"
);

static char kstack[65536] __attribute__((aligned(16), used));
/* Separate stack for the PLV3 probe, inside the mapped low window. */
static char p0_stack[16384] __attribute__((aligned(16), used));

__asm__(
".text\n"
".globl _start\n"
"_start:\n"
"  la $sp, kstack\n"
"  lu12i.w $t0, 16\n"
"  add.d $sp, $sp, $t0\n"
"  bl kmain\n"
"1: b 1b\n"
);

void kmain(void)
{
    uint64_t cr0;
    uint64_t dmw;

    ps("p0: start\n");

    /* Probe page content: the value the faulting load must eventually see. */
    g_probe_page[0] = UINT64_C(0x1234abcd5678ef90);

    /* 1. Build the 4 KiB table. */
    lapg_pool_init(&g_pool, (uint64_t)(uintptr_t)g_pool_mem,
                   sizeof(g_pool_mem));
    if (!lapg_pool_alloc(&g_pool, LAPG_PAGE_SIZE, &g_root)) {
        LOG("root allocation failed");
        return;
    }
    {
        uint64_t i;
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)g_root;

        for (i = 0; i < LAPG_PAGE_SIZE / 8; i++) {
            p[i] = 0;
        }
    }
    ps("p0: root=");
    ph(g_root);
    ps(" free_pages=");
    ph(lapg_pool_pages_remaining(&g_pool));
    ps("\n");

    /*
     * 2. Map the pages PLV3 will execute from -- the probe stub's own page.
     *    Without this the ERTN into PLV3 would fault on the instruction
     *    fetch instead of on the data load, and the test would not prove
     *    anything about servicing a data fault.
     *
     *    PROBE_VA is mapped *in the table* but deliberately not in the TLB:
     *    nothing has filled it, so the first PLV3 load to it still refills.
     *    That is the case the handler is supposed to service.
     */
    {
        /*
         * Both the probe's code and the stack it runs on have to be
         * present before PLV3 executes, otherwise the first fault is a
         * fetch or a stack push rather than the data load under test.
         */
        /*
         * Map the whole low image window rather than individual symbols:
         * the probe runs at PLV3, where every fetch, stack push and global
         * access must go through the TLB, so an unmapped page anywhere in
         * the image turns into a spurious fault that hides the data-load
         * case the test is about.
         */
        uint64_t code_va = (uint64_t)(uintptr_t)p0_probe & ~UINT64_C(0xfffff);
        uint64_t stack_va = (uint64_t)(uintptr_t)p0_stack & ~UINT64_C(0xfffff);
        uint64_t i;

        /* Cover the whole probe stack, including the page the initial
         * $sp lands in (base + 16 KiB - 32). */
        for (i = 0; i < 256; i++) {
            uint64_t va = stack_va + i * LAPG_PAGE_SIZE;
            LapgResult r = lapg_map(&g_memory, &g_pool, g_root, va, va,
                                    LAPG_PERM_READ | LAPG_PERM_WRITE |
                                    LAPG_PERM_USER);
            if (r != LAPG_OK) {
                ps("p0: map of probe stack failed r=");
                ph((uint64_t)r);
                ps("\n");
                return;
            }
        }

        for (i = 0; i < 256; i++) {
            uint64_t va = code_va + i * LAPG_PAGE_SIZE;
            LapgResult r = lapg_map(&g_memory, &g_pool, g_root, va, va,
                                    LAPG_PERM_READ | LAPG_PERM_WRITE |
                                    LAPG_PERM_EXEC | LAPG_PERM_USER);
            if (r != LAPG_OK) {
                ps("p0: map of probe code failed r=");
                ph((uint64_t)r);
                ps("\n");
                return;
            }
        }
    }

    /*
     * Map PROBE_VA in the table (not in the TLB): the walk below will find
     * it, so the handler has something real to install.  The physical page
     * holds the magic the load must read back.
     */
    {
        LapgResult r = lapg_map(&g_memory, &g_pool, g_root, PROBE_VA,
                                (uint64_t)(uintptr_t)g_probe_page,
                                LAPG_PERM_READ | LAPG_PERM_USER);
        if (r != LAPG_OK) {
            ps("p0: map of probe page failed r=");
            ph((uint64_t)r);
            ps("\n");
            return;
        }
    }

    /* 3. Program the page-walk controller and the 4 KiB granule. */
    csrwr(0x1c, lapg_pwcl_value());
    csrwr(0x1d, lapg_pwch_value());
    csrwr(0x1e, lapg_stlbps_value());
    ps("p0: pwcl=");
    ph(csrrd(0x1c));
    ps(" stlbps=");
    ph(csrrd(0x1e));
    ps("\n");

    /*
     * 4. Install both exception vectors, and flatten ECFG.VS.
     *
     *    A TLB refill (PIL/PIS/PIF) is delivered through CSR.TLBRENTRY,
     *    while every other exception goes to EENTRY + ECODE * vec_size.
     *    With a non-zero ECFG.VS the handler would be entered at an offset
     *    from its base and land in the middle of the stub, so VS is forced
     *    to zero here and EENTRY holds the entry point itself.
     */
    {
        uint64_t ecfg = csrrd(0x4);

        csrwr(0x4, ecfg & ~(UINT64_C(0x7) << 16));
    }
    csrwr(0xc, (uint64_t)(uintptr_t)p0_trap);
    csrwr(0x88, (uint64_t)(uintptr_t)p0_trap);

    /*
     * 5. DMW0 := segment 0, PLV0 only.  This is what lets this code keep
     *    running once paging is on: its own address is in segment 0 at
     *    PLV0, so the DMW covers the fetch and the stack, while a PLV3
     *    access is not covered by it and must consult the TLB.
     */
    dmw = ((uint64_t)0 << 60) | (UINT64_C(1) << 4) | 1u;
    csrwr(0x180, dmw);
    ps("p0: dmw0=");
    ph(csrrd(0x180));
    ps("\n");

    /* 6. Turn paging on: DA=0, PG=1, PLV0, IE off. */
    cr0 = csrrd(0x0);
    cr0 &= ~UINT64_C(0x1b);        /* clear DA, PG, PLV, IE */
    cr0 |= UINT64_C(1) << 4;       /* PG on, DA off, PLV0 */
    cr0 &= ~UINT64_C(1) << 2;      /* DA off explicitly */
    cr0 &= ~UINT64_C(1) << 3;      /* IE off */
    csrwr(0x0, cr0);
    ps("p0: cr0=");
    ph(csrrd(0x0));
    ps("\n");
    LOG("paging enabled");

    /* 7. Run the probe: PLV3 load from the unmapped page. */
    p0_probe();

    /* 8. Report. */
    ps("p0: faults=");
    ph((uint64_t)g_fault_count);
    ps(" recovered=");
    ph((uint64_t)g_recovered);
    ps(" last_ecode=");
    ph(g_last_ecode);
    ps(" last_badv=");
    ph(g_last_badv);
    ps(" value=");
    ph(g_probe_value);
    ps("\n");
    ps(g_recovered > 0 ? "p0: P0-OK\n" : "p0: P0-FAIL\n");
}
