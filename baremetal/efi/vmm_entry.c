/*
 * P0: UEFI thin VMM entry -- ExitBootServices handoff and the 4 KiB
 * hardware page table bring-up.
 *
 * The sequence is the one the design calls for:
 *
 *   1. Before ExitBootServices, buy every resource in one go with
 *      AllocatePages: guest RAM, the translate code cache (EfiLoaderCode so
 *      the firmware already treats it as executable), and the pool the page
 *      tables and VMM structures live in.  Nothing is allocated afterwards.
 *   2. Call GetMemoryMap + ExitBootServices, at which point BootServices is
 *      gone and the firmware's own page tables stop being authoritative.
 *   3. Build a 4 KiB (CSR.STLBPS.PS = 12) four-level table over the pool and
 *      install CSR.PWCL/PWCH.
 *   4. Install CSR.EENTRY and CSR.TLBRENTRY so a miss in PLV1/PLV3 lands in
 *      this code, fill the TLB from the guest tables, and resume.
 *
 * The exception path is exercised by a probe page that is deliberately left
 * unmapped: a PLV3 load from it must trap, the handler must map and fill the
 * entry, and execution must resume and read the value.
 *
 * Everything here runs on real hardware (or QEMU) at PLV0; the table
 * arithmetic itself is unit tested on the host via loongarch_paging.c.
 *
 * STATUS: the same sequence is verified end to end by baremetal/bare/
 * paging_smoke.c (make test-p0): a PLV3 load faults, the PLV0 handler walks
 * the table, TLBFILLs, and the load completes with the right value.  This
 * UEFI entry reaches its allocations, GetMemoryMap and the PWCL/PWCH/STLBPS
 * programming, but ExitBootServices does not return on this firmware in the
 * current build, so the probe below is not executed and the UEFI path is
 * NOT verified.  See the note at the ExitBootServices call.
 */

#include <stdint.h>

#include "loongemu/loongarch_paging.h"
#include "loongemu/acpi_tables.h"
#include "efi_min.h"

/* The UART on the QEMU virt board; memory mapped, so it survives EBS. */
#define UART0_THR ((volatile unsigned char *)0x1fe001e0)

#define VMM_GUEST_RAM_PAGES  256      /* 1 MiB  */
#define VMM_CODE_CACHE_PAGES 64       /* 256 KiB */
#define VMM_POOL_PAGES       0x1000   /* 16 MiB: tables for a 256 MiB window */

/*
 * The image is identity mapped before paging comes on.  The firmware places
 * the application inside a larger loaded image, so the window is generous
 * rather than fitted to this object.
 */
/*
 * Everything the VMM itself touches after the switch has to be mapped: its
 * code, its stack, the page table pool, guest RAM and the code cache.  The
 * firmware places the application, its stack and the pool in the same low
 * physical region, so one window from the start of RAM covers them; 64 MiB
 * is generous because unmapping any part of it turns into a fault inside
 * the handler, which is very hard to see.
 */
/*
 * Map from the bottom of physical memory: the firmware's own stack and the
 * images it loaded all live below the application, and after the switch any
 * of them can be touched -- including by the exception stub when it saves
 * the interrupted $sp, which points into whatever stack the fault came from.
 */
#define VMM_IMAGE_BASE       UINT64_C(0x00000000)
#define VMM_IMAGE_PAGES      0x10000     /* 256 MiB */

/* Where the probe lands in the guest address space. */
#define VMM_PROBE_VA         0x40000000ULL

/* Exception codes (LoongArch manual, ESTAT.Ecode). */
#define EXC_PIL  0x1    /* page invalid, load  */
#define EXC_PIS  0x2    /* page invalid, store */
#define EXC_PIF  0x3    /* page invalid, fetch */
#define EXC_PME  0x4    /* page modify */
#define EXC_PPI  0x7    /* page privilege */
#define EXC_PNX  0x8    /* page non-executable */
#define EXC_PNR  0x9    /* page non-readable */
#define EXC_IPE  0x0d   /* instruction privilege error */
#define EXC_INT  0x0

static void putc_(char c) { *UART0_THR = (unsigned char)c; }

static void puts_(const char *s)
{
    while (*s) {
        putc_(*s++);
    }
}

static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int sh;

    puts_("0x");
    for (sh = 60; sh >= 0; sh -= 4) {
        putc_(d[(v >> sh) & 0xf]);
    }
}

#define LOG(msg)        do { puts_("vmm: "); puts_(msg); puts_("\n"); } while (0)

/* ---- state that must outlive the firmware ---------------------------- */

static EFI_PHYSICAL_ADDRESS g_guest_ram;
static EFI_PHYSICAL_ADDRESS g_code_cache;
static EFI_PHYSICAL_ADDRESS g_pool;
static uint64_t g_map_key;
static const void *g_host_acpi_rsdp;
static LoongemuAcpiInfo g_guest_acpi_info;

static int efi_guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) {
            return 0;
        }
    }
    return 1;
}

static const void *uefi_find_acpi_rsdp(const EFI_SYSTEM_TABLE *st)
{
    static const EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    static const EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;
    const void *v1_fallback = 0;

    if (st == 0 || st->ConfigurationTable == 0) {
        return 0;
    }
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        const EFI_CONFIGURATION_TABLE *ent = &st->ConfigurationTable[i];
        if (efi_guid_eq(&ent->VendorGuid, &acpi20_guid)) {
            return ent->VendorTable;
        }
        if (efi_guid_eq(&ent->VendorGuid, &acpi10_guid)) {
            v1_fallback = ent->VendorTable;
        }
    }
    return v1_fallback;
}

static LapgPagePool g_lapg_pool;
static uint64_t g_root_table;

static volatile int g_faults;
static volatile int g_recovered;
static volatile uint64_t g_fault_va;
static volatile uint64_t g_fault_ecode;

/* ---- physical memory access ------------------------------------------ */

/*
 * Direct-map window.  CSR.DMW[n].VSEG selects the top four address bits;
 * with paging on, an access whose VA[63:60] matches VSEG and whose PLV bit
 * is set bypasses the TLB and uses VA[47:0] as the physical address
 * (cpu_helper.c: dmw_va2pa returns va & TARGET_VIRT_MASK on LA64).
 *
 * The VMM therefore keeps its own window rather than inheriting the
 * firmware's: after ExitBootServices the firmware's DMWs are not
 * guaranteed to survive, and every physical access this file makes -- page
 * table walks included -- goes through it.
 */
#define VMM_DMW_SEG  UINT64_C(0x9)
#define VMM_DMW_VA(phys) (UINT64_C(0x9000000000000000) | ((phys) & UINT64_C(0x0000ffffffffffff)))

/* MAT=1 (coherent cached), PLV0 only. */
#define VMM_DMW_CFG ((uint64_t)VMM_DMW_SEG << 60 | UINT64_C(1) << 4 | 1u)

/*
 * While the firmware's page tables are still in force this code runs with
 * CRMD.DA=1 (direct address translation), so a physical address is directly
 * usable.  The DMW window only becomes necessary after paging is switched
 * on, so the low form is used until the switch and the DMW form after it.
 */
static int g_paging_on;
static uint64_t g_dmw0;

static void *phys_to_ptr(uint64_t phys)
{
    if (!g_paging_on) {
        return (void *)(uintptr_t)phys;
    }
    return (void *)(uintptr_t)(g_dmw0 | (phys & UINT64_C(0x0000ffffffffffff)));
}

static int vmm_read_u64(void *opaque, uint64_t phys, uint64_t *value)
{
    (void)opaque;
    if ((phys & 7) != 0) {
        return 0;
    }
    *value = *(volatile uint64_t *)phys_to_ptr(phys);
    return 1;
}

static int vmm_write_u64(void *opaque, uint64_t phys, uint64_t value)
{
    (void)opaque;
    if ((phys & 7) != 0) {
        return 0;
    }
    *(volatile uint64_t *)phys_to_ptr(phys) = value;
    return 1;
}

static LapgMemory g_memory = {
    .opaque = 0,
    .read_u64 = vmm_read_u64,
    .write_u64 = vmm_write_u64,
};

/* ---- CSR access ------------------------------------------------------ */

static inline uint64_t csr_read(unsigned int num)
{
    uint64_t v;

    switch (num) {
#define CR(n) case n: __asm__ volatile("csrrd %0, " #n : "=r"(v)); break;
    CR(0x0) CR(0x4) CR(0x5) CR(0x6) CR(0x7) CR(0xc) CR(0x88)
    CR(0x10) CR(0x11) CR(0x12) CR(0x13) CR(0x18) CR(0x1b)
    CR(0x1c) CR(0x1d) CR(0x1e) CR(0x8a) CR(0x89)
#undef CR
    default:
        v = 0;
        break;
    }
    return v;
}

/* csrwr writes the previous value back into rd, so stage the value. */
static inline void csr_write(unsigned int num, uint64_t value)
{
    switch (num) {
#define CW(n) \
    case n: __asm__ volatile("move $r12,%0\n csrwr $r12," #n :: "r"(value) : "r12", "memory"); break;
    CW(0x0) CW(0x4) CW(0xc) CW(0x88) CW(0x1c) CW(0x1d) CW(0x1e)
    CW(0x10) CW(0x11) CW(0x12) CW(0x13) CW(0x18) CW(0x1b)
    CW(0x8c) CW(0x8d) CW(0x8e)
#undef CW
    default:
        break;
    }
}

/* ---- exception entry ------------------------------------------------- */

extern void vmm_trap_entry(void *frame);
extern char vmm_trap_frame[];

/*
 * Called from the assembly stub with the interrupted register state saved.
 * Decodes ESTAT.Ecode, and for a page-invalid exception walks the guest
 * table, fills the TLB from the resulting entry, and returns so the stub can
 * ERTN back to the faulting instruction.
 */
void vmm_handle_trap(uint64_t *regs)
{
    uint64_t estat = csr_read(0x5);
    uint64_t ecode = (estat >> 16) & 0x3f;
    uint64_t era = csr_read(0x6);
    uint64_t badv;

    g_faults++;
    g_fault_ecode = ecode;

    if (ecode == EXC_INT) {
        puts_("vmm: unexpected interrupt\n");
        for (;;) {
        }
    }

    badv = csr_read(0x7);    /* CSR.BADV: the faulting virtual address */
    g_fault_va = badv;

    if (ecode == EXC_PIL || ecode == EXC_PIS || ecode == EXC_PIF) {
        LapgTranslation tr;
        LapgResult r;
        uint32_t access = (ecode == EXC_PIS) ? LAPG_PERM_WRITE
                        : (ecode == EXC_PIF) ? LAPG_PERM_EXEC
                                             : LAPG_PERM_READ;
        uint64_t index;

        /*
         * The guest page tables are the x86 side's; for the P0 bring-up the
         * VMM owns them directly, so the walk uses the same 4 KiB levels.
         */
        (void)access;
        r = lapg_walk(&g_memory, g_root_table, badv,
                      LAPG_PERM_READ, &tr);
        if (r != LAPG_OK) {
            puts_("vmm: unhandled fault ecode=");
            puthex(ecode);
            puts_(" va=");
            puthex(badv);
            puts_(" walk=");
            puthex((uint64_t)r);
            puts_("\n");
            for (;;) {
            }
        }

        /*
         * Program the TLB entry the walk produced.
         *
         * A refill runs with TLBRERA.ISTLBR set, and in that state
         * fill_tlb_entry() sources its operands from the TLBR* group
         * (TLBREHI/TLBRELO0/TLBRELO1), not from TLBEHI/TLBELO0/TLBELO1.
         * TLBREHI also carries PS in its low six bits, and tlbfill compares
         * that against CSR.STLBPS to choose between STLB and MTLB -- leaving
         * PS at zero silently drops the entry into MTLB at a random line and
         * the refill repeats forever.  TLBFILL then picks the way itself, so
         * no TLBIDX write is needed.
         */
        index = badv & ~((1ULL << LAPG_PAGE_BITS) - 1);
        if (csr_read(0x8a) & 1) {
            csr_write(0x8e, index | (uint64_t)LAPG_PAGE_BITS);
            csr_write(0x8c, tr.pte);
            csr_write(0x8d, tr.pte);
        } else {
            csr_write(0x11, index);
            csr_write(0x12, tr.pte);
            csr_write(0x13, tr.pte);
        }
        __asm__ volatile("tlbfill" ::: "memory");

        g_recovered++;
        return;
    }

    puts_("vmm: unhandled trap ecode=");
    puthex(ecode);
    puts_(" era=");
    puthex(era);
    puts_(" badv=");
    puthex(badv);
    puts_("\n");
    for (;;) {
    }
}

/*
 * The trap frame: save every GPR the handler may touch, call the C handler,
 * then restore and ERTN.  Keeping the stub in one place means the EENTRY and
 * TLBRENTRY both point at it.
 */
__asm__(
".text\n"
".globl vmm_trap_entry\n"
"vmm_trap_entry:\n"
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
"  move $a0, $sp\n"
"  bl vmm_handle_trap\n"
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
"  addi.d $sp, $sp, 272\n"
"  ertn\n"
".globl vmm_trap_frame\n"
"vmm_trap_frame:\n"
"  nop\n"
);

/* ---- page table bring-up -------------------------------------------- */

/*
 * Identity map the VMM's own code and the pool before the MMU is switched
 * on, otherwise the first instruction after enabling paging would miss.
 */
static int vmm_map_identity(uint64_t start, uint64_t bytes, uint32_t perm)
{
    uint64_t off;

    for (off = 0; off < bytes; off += LAPG_PAGE_SIZE) {
        LapgResult r = lapg_map(&g_memory, &g_lapg_pool, g_root_table,
                                start + off, start + off, perm);

        if (r != LAPG_OK) {
            return 0;
        }
    }
    return 1;
}

static int vmm_build_tables(void)
{
    uint64_t pool_bytes = (uint64_t)VMM_POOL_PAGES * LAPG_PAGE_SIZE;

    lapg_pool_init(&g_lapg_pool, g_pool, pool_bytes);
    if (!lapg_pool_alloc(&g_lapg_pool, LAPG_PAGE_SIZE, &g_root_table)) {
        return 0;
    }

    /* Zero the root so an empty entry reads as "not present". */
    {
        uint64_t i;
        volatile uint64_t *p = (volatile uint64_t *)phys_to_ptr(g_root_table);

        for (i = 0; i < LAPG_PAGE_SIZE / 8; i++) {
            p[i] = 0;
        }
    }

    return 1;
}

/* ---- the probe ------------------------------------------------------- */

/*
 * Load from a page that has NOT been mapped yet.  The EENTRY handler maps it
 * and fills the TLB, and this instruction then completes -- that round trip
 * is the P0 acceptance criterion.
 */
static uint64_t vmm_probe_load(void)
{
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)VMM_PROBE_VA;

    return *p;
}

int efi_main(void *image, void *systab)
{
    EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)systab;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;
    UINTN map_size = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    void *map_buf;
    UINTN map_key = 0;
    int ok;

    puts_("vmm: start\n");

    /*
     * The table layout is checked against the firmware rather than assumed:
     * ExitBootServices sits at byte 232 of the table (EFI_TABLE_HEADER is 24
     * bytes, then 26 function pointers before it).  A mismatch means this
     * build's struct disagrees with the firmware's header and calling
     * anything through it would be guesswork, so stop instead.
     */
    {
        volatile uint64_t off = (uintptr_t)&bs->ExitBootServices -
                                (uintptr_t)bs;
        puts_("vmm: ExitBootServices offset=");
        puthex(off);
        puts_("\n");
        if (off != 232) {
            LOG("BootServices layout mismatch");
            return 1;
        }
    }

    /*
     * Phase 0: capture the host UEFI ACPI 2.0 RSDP from ConfigurationTable
     * before ExitBootServices so we can propagate platform metadata into the
     * synthesized x86-64 guest ACPI tables.
     */
    g_host_acpi_rsdp = uefi_find_acpi_rsdp(st);
    if (g_host_acpi_rsdp != 0) {
        uint8_t rev = 0;
        uint64_t xsdt_pa = 0;
        char oem[7];
        if (loongemu_acpi_validate_rsdp(g_host_acpi_rsdp, &rev, &xsdt_pa, oem)) {
            puts_("vmm: host UEFI ACPI RSDP=");
            puthex((uint64_t)(uintptr_t)g_host_acpi_rsdp);
            puts_(" rev=");
            puthex(rev);
            puts_(" xsdt=");
            puthex(xsdt_pa);
            puts_(" oem=");
            puts_(oem);
            puts_("\n");
        }
    }

    /*
     * Phase 1: buy everything while the Boot Services allocator still
     * exists.  EfiLoaderCode for the code cache so the firmware maps it
     * executable; EfiLoaderData for guest RAM and the pool.
     */
    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               VMM_GUEST_RAM_PAGES, &addr);
    if (EFI_ERROR(status)) {
        LOG("guest RAM allocation failed");
        return 1;
    }
    g_guest_ram = addr;

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderCode,
                               VMM_CODE_CACHE_PAGES, &addr);
    if (EFI_ERROR(status)) {
        LOG("code cache allocation failed");
        return 1;
    }
    g_code_cache = addr;

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, VMM_POOL_PAGES,
                               &addr);
    if (EFI_ERROR(status)) {
        LOG("page table pool allocation failed");
        return 1;
    }
    g_pool = addr;

    puts_("vmm: guest_ram=");
    puthex(g_guest_ram);
    puts_(" code_cache=");
    puthex(g_code_cache);
    puts_(" pool=");
    puthex(g_pool);
    puts_("\n");

    /*
     * Phase 2: GetMemoryMap, then ExitBootServices.  The map key may change
     * between the two calls if an event fires, so retry in the documented
     * way rather than assuming the first key is still valid.
     */
    map_size = 0;
    bs->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += desc_size * 4;
    status = bs->AllocatePool(EfiLoaderData, map_size, &map_buf);
    if (EFI_ERROR(status)) {
        LOG("memory map buffer allocation failed");
        return 1;
    }
    status = bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                              &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        LOG("GetMemoryMap failed");
        return 1;
    }
    status = bs->ExitBootServices(image, map_key);
    if (EFI_ERROR(status)) {
        LOG("ExitBootServices failed");
        return 1;
    }

    LOG("boot services exited");

    /* Phase 3: 4 KiB tables.  No UEFI allocator exists past this point. */
    if (!vmm_build_tables()) {
        LOG("page table build failed");
        return 1;
    }
    puts_("vmm: root="); puthex(g_root_table);
    puts_(" code_pc="); puthex((uint64_t)(uintptr_t)efi_main);
    puts_(" pool_va="); puthex((uint64_t)(uintptr_t)phys_to_ptr(g_pool));
    puts_("\n");
    puts_("vmm: pwcl=");
    puthex(lapg_pwcl_value());
    puts_(" pwch=");
    puthex(lapg_pwch_value());
    puts_(" stlbps=");
    puthex(lapg_stlbps_value());
    puts_("\n");

    /*
     * Identity map the pages this code is executing from, plus the pool, so
     * turning paging on does not fault on the next instruction.
     *
     * The range is taken from the absolute start of the image rather than
     * from efi_main: the entry point sits in the middle of a busier image
     * after the firmware relocates it, and the trap stub and the tables can
     * be on either side of it.
     */
    ok = vmm_map_identity(VMM_IMAGE_BASE, VMM_IMAGE_PAGES * LAPG_PAGE_SIZE,
                          LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
    ok = ok && vmm_map_identity(g_pool, (uint64_t)VMM_POOL_PAGES * LAPG_PAGE_SIZE,
                                LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
    ok = ok && vmm_map_identity(g_guest_ram,
                                (uint64_t)VMM_GUEST_RAM_PAGES * LAPG_PAGE_SIZE,
                                LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
    if (!ok) {
        LOG("identity map failed");
        puts_("vmm: pool pages left=");
        puthex(lapg_pool_pages_remaining(&g_lapg_pool));
        puts_("\n");
        return 1;
    }

    /* Phase 4: hand the tables to the hardware and take the exception
     * vectors. */
    csr_write(0x1c, lapg_pwcl_value());
    csr_write(0x1d, lapg_pwch_value());
    csr_write(0x1e, lapg_stlbps_value());
    /*
     * A refill is delivered through TLBRENTRY, and every other exception goes
     * to EENTRY + ECODE * vec_size.  With a non-zero ECFG.VS the handler
     * would be entered at an offset from its base and land inside the stub,
     * so VS is forced to zero and EENTRY holds the entry point itself.
     */
    {
        uint64_t ecfg = csr_read(0x4);

        csr_write(0x4, ecfg & ~(UINT64_C(0x7) << 16));
    }
    csr_write(0xc, (uint64_t)(uintptr_t)vmm_trap_entry);
    csr_write(0x88, (uint64_t)(uintptr_t)vmm_trap_entry);

    /*
     * Phase 5: switch paging on.  DMW0 gets segment 0 with only the PLV0
     * bit, so this code keeps executing across the switch while a PLV3
     * access must consult the TLB; CRMD then flips to DA=0, PG=1.
     */
    /* VSEG occupies VA[63:60]; segment 0 is VSEG == 0.  Writing 1<<60 here
     * would select segment 1 and leave the image and the UART, both of which
     * live in segment 0, unreachable the moment paging comes on. */
    g_dmw0 = UINT64_C(0);
    csr_write(0x180, g_dmw0 | (UINT64_C(1) << 4) | 1u);
    g_paging_on = 1;
    {
        uint64_t crmd = csr_read(0x0);

        crmd &= ~(UINT64_C(1) << 3);   /* DA off */
        crmd |= UINT64_C(1) << 4;      /* PG on  */
        crmd &= ~UINT64_C(3);          /* PLV0   */
        csr_write(0x0, crmd);
    }
    puts_("vmm: paging on, crmd=");
    puthex(csr_read(0x0));
    puts_("\n");

    puts_("vmm: pwcl readback=");
    puthex(csr_read(0x1c));
    puts_(" eentry=");
    puthex(csr_read(0xc));
    puts_("\n");

    /*
     * Map the probe VA onto guest RAM: the entry exists in the table but not
     * in the TLB, so the load below refills and the handler services it.
     */
    {
        LapgResult r = lapg_map(&g_memory, &g_lapg_pool, g_root_table,
                                VMM_PROBE_VA, g_guest_ram,
                                LAPG_PERM_READ | LAPG_PERM_USER);
        if (r != LAPG_OK) {
            LOG("probe page map failed");
            return 1;
        }
    }

    /* Place a value the probe will read once the fault is serviced. */
    *(volatile uint64_t *)phys_to_ptr(g_guest_ram) = UINT64_C(0x1234abcd5678ef90);

    puts_("vmm: before probe, era_check=");
    puthex((uint64_t)(uintptr_t)vmm_probe_load);
    puts_("\n");
    vmm_probe_load();
    puts_("vmm: after probe\n");

    puts_("vmm: faults=");
    puthex((uint64_t)g_faults);
    puts_(" recovered=");
    puthex((uint64_t)g_recovered);
    puts_(" last_va=");
    puthex(g_fault_va);
    puts_(" ecode=");
    puthex(g_fault_ecode);
    puts_("\n");
    puts_(g_recovered ? "vmm: P0-OK\n" : "vmm: P0-FAIL\n");
    return 0;
}
