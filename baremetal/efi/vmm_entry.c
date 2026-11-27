/*
 * P0: UEFI thin VMM entry -- real-hardware-safe ExitBootServices handoff,
 * ACPI SPCR UART + UEFI GOP console, relocation-aware image mapping, split
 * DMW0 (cached RAM) / DMW1 (uncached MMIO), and 4 KiB hardware page tables.
 */

#include <stdint.h>

#include "loongemu/loongarch_paging.h"
#include "loongemu/acpi_tables.h"
#include "loongemu/vclock.h"
#include "loongemu/pci_host.h"
#include "loongemu/vmm_handoff.h"
#include "efi_min.h"

#ifdef VMM_STANDALONE_PROBE
#define VMM_GUEST_RAM_PAGES    256    /* 1 MiB standalone paging probe */
#define VMM_CODE_CACHE_PAGES   64     /* 256 KiB */
#define VMM_POOL_PAGES         0x1000 /* 16 MiB */
#else
#define VMM_GUEST_RAM_PAGES    32768  /* 128 MiB: matches the System VMM guest map */
#define VMM_FB_FALLBACK_PAGES  1024   /* 4 MiB isolated Bochs framebuffer pool */
#endif
#define VMM_KSTACK_PAGES       16     /* 64 KiB dedicated VMM stack */

#define VMM_PAGE_ALIGN_UP(v) \
    (((v) + LAPG_PAGE_SIZE - 1u) & ~(uint64_t)(LAPG_PAGE_SIZE - 1u))

#if !defined(VMM_STANDALONE_PROBE) && !defined(LOONGEMU_PAYLOAD_FROM_ESP)
extern const uint8_t _ovmf_code_start[], _ovmf_code_end[];
extern const uint8_t _ovmf_vars_start[], _ovmf_vars_end[];
#endif

#ifdef LOONGEMU_PAYLOAD_FROM_ESP
/*
 * Runtime payload loading: the guest firmware is read from the boot volume
 * before ExitBootServices (baremetal/efi/efi_payload.c) instead of being
 * linked in.  Paths are fixed and documented rather than configurable, so a
 * boot volume can be prepared without knowing build-time flags.
 */
#define PAYLOAD_CODE_PATH "\\EFI\\loongemu\\ovmf_code.fd"
#define PAYLOAD_VARS_PATH "\\EFI\\loongemu\\ovmf_vars.fd"
int efi_payload_read(EFI_BOOT_SERVICES *bs, EFI_HANDLE image_handle,
                     const char *path, EFI_PHYSICAL_ADDRESS *out_base,
                     uint64_t *out_size);
#endif

#ifdef VMM_STANDALONE_PROBE
/* Where the probe lands in the guest address space. */
#define VMM_PROBE_VA         0x40000000ULL

/* Exception codes (LoongArch manual, ESTAT.Ecode). */
#define EXC_PIL  0x1
#define EXC_PIS  0x2
#define EXC_PIF  0x3
#define EXC_PME  0x4
#define EXC_PPI  0x7
#define EXC_PNX  0x8
#define EXC_PNR  0x9
#define EXC_IPE  0x0d
#define EXC_INT  0x0
#endif

/*
 * Split direct-map windows (LoongArch Reference Manual):
 *   DMW0: VSEG=0x9, MAT=1 (Coherent Cached), PLV0=1 -> RAM / page tables
 *   DMW1: VSEG=0x8, MAT=0 (Strongly-Ordered Uncached), PLV0=1 -> MMIO / UART / GOP
 */
#define VMM_DMW0_BASE UINT64_C(0x9000000000000000)
#define VMM_DMW1_BASE UINT64_C(0x8000000000000000)
#define VMM_DMW0_CFG  (VMM_DMW0_BASE | (UINT64_C(1) << 4) | 1u)
#define VMM_DMW1_CFG  (VMM_DMW1_BASE | (UINT64_C(0) << 4) | 1u)

/* ---- state that must outlive the firmware ---------------------------- */

static EFI_PHYSICAL_ADDRESS g_guest_ram;
static uint64_t g_guest_ram_bytes;
#ifdef VMM_STANDALONE_PROBE
static EFI_PHYSICAL_ADDRESS g_code_cache;
static EFI_PHYSICAL_ADDRESS g_pool;
#else
static EFI_PHYSICAL_ADDRESS g_fb_fallback;
static EFI_PHYSICAL_ADDRESS g_ovmf_code;
static EFI_PHYSICAL_ADDRESS g_ovmf_vars;
#endif
static EFI_PHYSICAL_ADDRESS g_kstack_base;
static uint64_t g_image_base;
static uint64_t g_image_size;
static uint64_t g_host_timer_hz = UINT64_C(100000000);
static const void *g_host_acpi_rsdp;
static uint64_t g_uart_phys = 0x1fe001e0ULL;
static uint8_t  g_uart_reg_shift;
static int      g_paging_on;

/* UEFI GOP linear framebuffer console + guest handover descriptor (survives ExitBootServices) */
static uint64_t g_gop_fb_phys;
static uint32_t g_gop_width;
static uint32_t g_gop_height;
static uint32_t g_gop_stride;
static uint32_t g_gop_cursor_x;
static uint32_t g_gop_cursor_y;
static LoongemuGopHandover g_gop_handover;
static LoongemuVmmHandoff g_vmm_handoff;

const LoongemuVmmHandoff *loongemu_vmm_get_handoff(void)
{
    return &g_vmm_handoff;
}

#ifdef VMM_STANDALONE_PROBE
static inline void *phys_to_cached(uint64_t phys)
{
    if (!g_paging_on) {
        return (void *)(uintptr_t)phys;
    }
    return (void *)(uintptr_t)(VMM_DMW0_BASE | (phys & UINT64_C(0x0000ffffffffffff)));
}
#endif

static inline void *phys_to_mmio(uint64_t phys)
{
    if (!g_paging_on) {
        return (void *)(uintptr_t)phys;
    }
    return (void *)(uintptr_t)(VMM_DMW1_BASE | (phys & UINT64_C(0x0000ffffffffffff)));
}

static void gop_putc(char c)
{
    if (g_gop_fb_phys == 0 || g_gop_width < 8u || g_gop_height < 8u || g_gop_stride == 0) {
        return;
    }
    if (c == '\n') {
        g_gop_cursor_x = 0;
        g_gop_cursor_y += 10u;
        if (g_gop_cursor_y + 8u >= g_gop_height) {
            g_gop_cursor_y = 0;
        }
        return;
    }
    if (c == '\r') {
        g_gop_cursor_x = 0;
        return;
    }
    /* Minimal visible 6x8 block glyph indicator so early bring-up is visible on GOP. */
    volatile uint32_t *fb = (volatile uint32_t *)phys_to_mmio(g_gop_fb_phys);
    uint32_t color = (c == ' ') ? 0x00000000u : 0x00e0e0e0u;
    for (uint32_t dy = 1; dy < 7u; dy++) {
        uint32_t py = g_gop_cursor_y + dy;
        for (uint32_t dx = 1; dx < 6u; dx++) {
            uint32_t px = g_gop_cursor_x + dx;
            if (px < g_gop_width && py < g_gop_height) {
                fb[py * g_gop_stride + px] = color;
            }
        }
    }
    g_gop_cursor_x += 8u;
    if (g_gop_cursor_x + 8u >= g_gop_width) {
        g_gop_cursor_x = 0;
        g_gop_cursor_y += 10u;
        if (g_gop_cursor_y + 8u >= g_gop_height) {
            g_gop_cursor_y = 0;
        }
    }
}

static void putc_(char c)
{
    if (g_uart_phys != 0) {
        volatile uint8_t *thr = (volatile uint8_t *)phys_to_mmio(g_uart_phys);
        volatile uint8_t *lsr = (volatile uint8_t *)phys_to_mmio(
            g_uart_phys + ((uint64_t)5u << g_uart_reg_shift));
        for (uint32_t spin = 0; spin < 100000u; spin++) {
            if (((*lsr) & 0x20u) != 0u) {
                break;
            }
        }
        *thr = (uint8_t)c;
    }
    gop_putc(c);
}

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

#define LOG(msg) do { puts_("vmm: "); puts_(msg); puts_("\n"); } while (0)

static int range_valid(uint64_t base, uint64_t size)
{
    return size != 0 && base <= UINT64_MAX - size;
}

static int ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
    return range_valid(a, as) && range_valid(b, bs) &&
           a < b + bs && b < a + as;
}

static int page_range_valid(uint64_t base, uint64_t size)
{
    return base != 0 && range_valid(base, size) &&
           (base & (LAPG_PAGE_SIZE - 1u)) == 0 &&
           (size & (LAPG_PAGE_SIZE - 1u)) == 0;
}

static uint64_t page_round_up(uint64_t size)
{
    if (size == 0 || size > UINT64_MAX - (LAPG_PAGE_SIZE - 1u)) {
        return 0;
    }
    return VMM_PAGE_ALIGN_UP(size);
}

static void copy_and_fill(uint64_t dst, const uint8_t *src,
                          uint64_t src_size, uint64_t dst_size,
                          uint8_t fill)
{
    volatile uint8_t *out = (volatile uint8_t *)(uintptr_t)dst;
    uint64_t i;

    for (i = 0; i < src_size; i++) {
        out[i] = src[i];
    }
    for (; i < dst_size; i++) {
        out[i] = fill;
    }
}

static int vmm_allocations_disjoint(uint64_t map_buf, uint64_t map_size)
{
    uint64_t bases[8];
    uint64_t sizes[8];
    uint32_t count = 0;

    if (!page_range_valid(g_image_base, g_image_size) ||
        !page_range_valid(g_guest_ram, g_guest_ram_bytes) ||
        !page_range_valid(g_kstack_base,
                          (uint64_t)VMM_KSTACK_PAGES * LAPG_PAGE_SIZE)) {
        return 0;
    }
    bases[count] = g_image_base;
    sizes[count++] = g_image_size;
    bases[count] = g_guest_ram;
    sizes[count++] = g_guest_ram_bytes;
#ifdef VMM_STANDALONE_PROBE
    if (!page_range_valid(g_code_cache,
                          (uint64_t)VMM_CODE_CACHE_PAGES * LAPG_PAGE_SIZE) ||
        !page_range_valid(g_pool, (uint64_t)VMM_POOL_PAGES * LAPG_PAGE_SIZE)) {
        return 0;
    }
    bases[count] = g_code_cache;
    sizes[count++] = (uint64_t)VMM_CODE_CACHE_PAGES * LAPG_PAGE_SIZE;
    bases[count] = g_pool;
    sizes[count++] = (uint64_t)VMM_POOL_PAGES * LAPG_PAGE_SIZE;
#else
    if (!page_range_valid(g_fb_fallback,
                          (uint64_t)VMM_FB_FALLBACK_PAGES * LAPG_PAGE_SIZE) ||
        !page_range_valid(g_ovmf_code, g_vmm_handoff.ovmf_code_size) ||
        !page_range_valid(g_ovmf_vars, g_vmm_handoff.ovmf_vars_size)) {
        return 0;
    }
    bases[count] = g_fb_fallback;
    sizes[count++] = (uint64_t)VMM_FB_FALLBACK_PAGES * LAPG_PAGE_SIZE;
    bases[count] = g_ovmf_code;
    sizes[count++] = g_vmm_handoff.ovmf_code_size;
    bases[count] = g_ovmf_vars;
    sizes[count++] = g_vmm_handoff.ovmf_vars_size;
#endif
    if (g_gop_handover.valid) {
        uint64_t gop_map_size = page_round_up(g_gop_handover.fb_size);
        if (!page_range_valid(g_gop_handover.fb_hpa, gop_map_size)) {
            return 0;
        }
        bases[count] = g_gop_handover.fb_hpa;
        sizes[count++] = gop_map_size;
    }
    bases[count] = g_kstack_base;
    sizes[count++] = (uint64_t)VMM_KSTACK_PAGES * LAPG_PAGE_SIZE;
    if (map_buf != 0 || map_size != 0) {
        if (!range_valid(map_buf, map_size)) {
            return 0;
        }
        bases[count] = map_buf;
        sizes[count++] = map_size;
    }

    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1u; j < count; j++) {
            if (ranges_overlap(bases[i], sizes[i], bases[j], sizes[j])) {
                return 0;
            }
        }
    }
    return 1;
}
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

#ifdef VMM_STANDALONE_PROBE
static LapgPagePool g_lapg_pool;
static uint64_t g_root_table;

static volatile int g_faults;
static volatile int g_recovered;
static volatile uint64_t g_fault_va;
static volatile uint64_t g_fault_ecode;

static int vmm_read_u64(void *opaque, uint64_t phys, uint64_t *value)
{
    (void)opaque;
    if ((phys & 7u) != 0) {
        return 0;
    }
    *value = *(volatile uint64_t *)phys_to_cached(phys);
    return 1;
}

static int vmm_write_u64(void *opaque, uint64_t phys, uint64_t value)
{
    (void)opaque;
    if ((phys & 7u) != 0) {
        return 0;
    }
    *(volatile uint64_t *)phys_to_cached(phys) = value;
    return 1;
}

static LapgMemory g_memory = {
    .opaque = 0,
    .read_u64 = vmm_read_u64,
    .write_u64 = vmm_write_u64,
};
#endif /* VMM_STANDALONE_PROBE */

/* ---- CSR access ------------------------------------------------------ */

static inline uint64_t csr_read(unsigned int num)
{
    uint64_t v;

    switch (num) {
#define CR(n) case n: __asm__ volatile("csrrd %0, " #n : "=r"(v)); break;
    CR(0x0) CR(0x4) CR(0x5) CR(0x6) CR(0x7) CR(0xc) CR(0x41) CR(0x88)
    CR(0x10) CR(0x11) CR(0x12) CR(0x13) CR(0x18) CR(0x1b)
    CR(0x1c) CR(0x1d) CR(0x1e) CR(0x8a) CR(0x89) CR(0x180) CR(0x181)
#undef CR
    default:
        v = 0;
        break;
    }
    return v;
}

static inline void csr_write(unsigned int num, uint64_t value)
{
    switch (num) {
#define CW(n) \
    case n: __asm__ volatile("move $r12,%0\n csrwr $r12," #n :: "r"(value) : "r12", "memory"); break;
    CW(0x0) CW(0x4) CW(0xc) CW(0x41) CW(0x44) CW(0x88)
    CW(0x1c) CW(0x1d) CW(0x1e)
    CW(0x10) CW(0x11) CW(0x12) CW(0x13) CW(0x18) CW(0x1b)
    CW(0x8c) CW(0x8d) CW(0x8e) CW(0x180) CW(0x181)
#undef CW
    default:
        break;
    }
}

#ifdef VMM_STANDALONE_PROBE
/* ---- exception entry ------------------------------------------------- */

extern void vmm_trap_entry(void *frame);

void vmm_handle_trap(uint64_t *regs)
{
    uint64_t tlbrera = csr_read(0x8a);
    int is_refill = (int)(tlbrera & 1u);
    uint64_t estat = csr_read(0x5);
    uint64_t ecode = (estat >> 16) & 0x3fu;
    uint64_t era = is_refill ? ((tlbrera >> 2) << 2) : csr_read(0x6);
    uint64_t badv = is_refill ? csr_read(0x89) : csr_read(0x7);

    (void)regs;
    g_faults++;
    g_fault_ecode = ecode;
    g_fault_va = badv;

    if (!is_refill && ecode == EXC_INT) {
        /* Clear any stray firmware timer interrupt cleanly. */
        csr_write(0x41, 0);
        csr_write(0x44, 1);
        return;
    }

    if (is_refill || ecode == EXC_PIL || ecode == EXC_PIS || ecode == EXC_PIF) {
        LapgTranslation tr;
        LapgResult r = lapg_walk(&g_memory, g_root_table, badv, LAPG_PERM_READ, &tr);
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
        uint64_t vppn = badv & UINT64_C(0x0000ffffffffe000);
        int is_odd = (int)((badv >> LAPG_PAGE_BITS) & 1u);
        uint64_t pte0 = is_odd ? 0ULL : tr.pte;
        uint64_t pte1 = is_odd ? tr.pte : 0ULL;
        if (is_refill) {
            csr_write(0x8e, vppn | (uint64_t)LAPG_PAGE_BITS);
            csr_write(0x8c, pte0);
            csr_write(0x8d, pte1);
        } else {
            csr_write(0x10, (uint64_t)LAPG_PAGE_BITS << 24);
            csr_write(0x11, vppn);
            csr_write(0x12, pte0);
            csr_write(0x13, pte1);
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
);

/* ---- page table bring-up -------------------------------------------- */

static int vmm_map_identity(uint64_t start, uint64_t bytes, uint32_t perm)
{
    uint64_t va = start & LAPG_PAGE_MASK;
    uint64_t end = (start + bytes + LAPG_PAGE_SIZE - 1) & LAPG_PAGE_MASK;

    for (; va < end; va += LAPG_PAGE_SIZE) {
        LapgResult r = lapg_map(&g_memory, &g_lapg_pool, g_root_table,
                                va, va, perm);
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
    volatile uint64_t *p = (volatile uint64_t *)phys_to_cached(g_root_table);
    for (uint64_t i = 0; i < LAPG_PAGE_SIZE / 8; i++) {
        p[i] = 0;
    }
    return 1;
}

static uint64_t vmm_probe_load(void)
{
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)VMM_PROBE_VA;
    return *p;
}
#endif /* VMM_STANDALONE_PROBE */

/* Post-ExitBootServices entry running on our own allocated kstack. */
static void __attribute__((used, noreturn)) vmm_post_ebs_main(void)
{
    /* 1. Immediately quiesce all firmware interrupts and timers. */
    csr_write(0x0, csr_read(0x0) & ~UINT64_C(0x4)); /* CRMD.IE = 0 */
    csr_write(0x41, 0);                             /* CSR.TCFG = 0 */
    csr_write(0x44, 1);                             /* CSR.TICLR = 1 */
    csr_write(0x4, 0);                              /* CSR.ECFG = 0 */

    g_host_timer_hz = loongemu_host_timer_hz();
    LOG("boot services exited (clean kstack)");
    puts_("vmm: host_timer_hz=");
    puthex(g_host_timer_hz);
    puts_(" image_base=");
    puthex(g_image_base);
    puts_(" image_size=");
    puthex(g_image_size);
    g_vmm_handoff.host_timer_hz = g_host_timer_hz;
    if (g_gop_handover.valid) {
        puts_(" gop_hpa=");
        puthex(g_gop_handover.fb_hpa);
        puts_(" gop_sz=");
        puthex(g_gop_handover.fb_size);
        puts_(" gop_wh=");
        puthex(((uint64_t)g_gop_handover.width << 32) | g_gop_handover.height);
    }
    if (g_vmm_handoff.ls7a_mcfg_valid) {
        puts_(" ls7a_mcfg=");
        puthex(g_vmm_handoff.ls7a_cfg_type0_base);
    }
    puts_("\n");

#ifndef VMM_STANDALONE_PROBE
    puts_("vmm: entering System VMM (tb_sys_vmm_main)\n");
    tb_sys_vmm_main(&g_vmm_handoff);
#else
    if (!vmm_build_tables()) {
        LOG("page table build failed");
        for (;;) {}
    }

    /* Map only the relocated VMM image, our kstack, pool, code_cache, and guest_ram. */
    {
        int ok = vmm_map_identity(g_image_base, g_image_size,
                                  LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
        ok = ok && vmm_map_identity(g_kstack_base, (uint64_t)VMM_KSTACK_PAGES * LAPG_PAGE_SIZE,
                                    LAPG_PERM_READ | LAPG_PERM_WRITE);
        ok = ok && vmm_map_identity(g_pool, (uint64_t)VMM_POOL_PAGES * LAPG_PAGE_SIZE,
                                    LAPG_PERM_READ | LAPG_PERM_WRITE);
        ok = ok && vmm_map_identity(g_code_cache, (uint64_t)VMM_CODE_CACHE_PAGES * LAPG_PAGE_SIZE,
                                    LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
        ok = ok && vmm_map_identity(g_guest_ram, g_guest_ram_bytes,
                                    LAPG_PERM_READ | LAPG_PERM_WRITE | LAPG_PERM_EXEC);
        if (!ok) {
            LOG("identity map failed");
            for (;;) {}
        }
    }

    /* Install 4 KiB paging CSRs and VMM trap vectors. */
    csr_write(0x1c, lapg_pwcl_value());
    csr_write(0x1d, lapg_pwch_value());
    csr_write(0x1e, lapg_stlbps_value());
    csr_write(0xc, (uint64_t)(uintptr_t)vmm_trap_entry);
    csr_write(0x88, (uint64_t)(uintptr_t)vmm_trap_entry);

    /*
     * Configure split DMW0 (VSEG=0x9, MAT=1 CC, PLV0=1) and DMW1 (VSEG=0x8,
     * MAT=0 SUC, PLV0=1), flush TLB, and enable paging (DA=0, PG=1, PLV0).
     */
    csr_write(0x180, VMM_DMW0_CFG);
    csr_write(0x181, VMM_DMW1_CFG);
    __asm__ volatile("invtlb 0x0, $zero, $zero\n dbar 0\n ibar 0" ::: "memory");
    g_paging_on = 1;
    {
        uint64_t crmd = csr_read(0x0);
        crmd &= ~(UINT64_C(1) << 3);   /* DA = 0 */
        crmd |= (UINT64_C(1) << 4);    /* PG = 1 */
        crmd &= ~UINT64_C(0x7);        /* PLV0, IE=0 */
        csr_write(0x0, crmd);
    }

    if (lapg_map(&g_memory, &g_lapg_pool, g_root_table,
                 VMM_PROBE_VA, g_guest_ram,
                 LAPG_PERM_READ | LAPG_PERM_USER) != LAPG_OK) {
        LOG("probe page map failed");
        for (;;) {}
    }
    *(volatile uint64_t *)phys_to_cached(g_guest_ram) = UINT64_C(0x1234abcd5678ef90);
    uint64_t observed = vmm_probe_load();

    puts_("vmm: faults=");
    puthex((uint64_t)g_faults);
    puts_(" recovered=");
    puthex((uint64_t)g_recovered);
    puts_(" observed=");
    puthex(observed);
    puts_("\n");
    puts_((g_recovered && observed == UINT64_C(0x1234abcd5678ef90))
          ? "vmm: P0-OK\n" : "vmm: P0-FAIL\n");
    for (;;) {
        __asm__ volatile("dbar 0x700" ::: "memory");
    }
#endif /* VMM_STANDALONE_PROBE */
}

static void __attribute__((noreturn)) switch_stack_and_enter(uint64_t new_sp)
{
    __asm__ volatile(
        "move $sp, %0\n"
        "bl vmm_post_ebs_main\n"
        "1: b 1b\n"
        :: "r"(new_sp) : "memory");
    __builtin_unreachable();
}

int efi_main(void *image, void *systab)
{
    EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)systab;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;
    UINTN map_size = 0, map_cap = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    void *map_buf = 0;
    UINTN map_key = 0;

    puts_("vmm: start\n");

    if (((uintptr_t)&bs->ExitBootServices - (uintptr_t)bs) != 232) {
        LOG("BootServices layout mismatch");
        return 1;
    }

    /* Refuse an unknown image extent; guessing could overwrite host memory. */
    {
        static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *li = 0;
        if (bs->HandleProtocol == 0 ||
            EFI_ERROR(bs->HandleProtocol(image, &li_guid, (void **)&li)) ||
            li == 0 || li->ImageBase == 0 || li->ImageSize == 0) {
            LOG("loaded image bounds unavailable");
            return 1;
        }
        g_image_base = (uint64_t)(uintptr_t)li->ImageBase;
        g_image_size = page_round_up(li->ImageSize);
        if (!page_range_valid(g_image_base, g_image_size)) {
            LOG("invalid loaded image bounds");
            return 1;
        }
    }

    /* Probe UEFI GOP framebuffer for real-hardware console fallback and Guest Bochs Display handover. */
    {
        static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
        if (bs->LocateProtocol != 0 &&
            !EFI_ERROR(bs->LocateProtocol(&gop_guid, 0, (void **)&gop)) &&
            gop != 0 && gop->Mode != 0 && gop->Mode->Info != 0 &&
            gop->Mode->FrameBufferBase != 0) {
            uint64_t fb_hpa = gop->Mode->FrameBufferBase;
            uint64_t fb_size = (uint64_t)gop->Mode->FrameBufferSize;
            uint64_t visible_size = 0;
            uint32_t width = gop->Mode->Info->HorizontalResolution;
            uint32_t height = gop->Mode->Info->VerticalResolution;
            uint32_t stride = gop->Mode->Info->PixelsPerScanLine;
            uint32_t pixel_format = gop->Mode->Info->PixelFormat;
            int geometry_ok = width != 0 && height != 0 && stride >= width &&
                              (uint64_t)stride <= UINT64_MAX / height / 4u;

            if (geometry_ok) {
                visible_size = (uint64_t)stride * height * 4u;
                if (fb_size == 0) {
                    fb_size = visible_size;
                }
            }
            if (geometry_ok && pixel_format <= 1u &&
                fb_size >= visible_size && fb_hpa <= UINT64_MAX - fb_size &&
                page_range_valid(fb_hpa, page_round_up(fb_size))) {
                g_gop_fb_phys = fb_hpa;
                g_gop_width = width;
                g_gop_height = height;
                g_gop_stride = stride;
                g_gop_handover.fb_hpa = fb_hpa;
                g_gop_handover.fb_size = fb_size;
                g_gop_handover.width = width;
                g_gop_handover.height = height;
                g_gop_handover.stride_px = stride;
                g_gop_handover.bpp = 32u;
                g_gop_handover.pixel_format = pixel_format;
                g_gop_handover.valid = 1u;
                g_gop_handover.owner = (uint8_t)LOONGEMU_FB_OWNER_HOST_GOP;
                g_gop_handover.host_mat = (uint8_t)LOONGEMU_MAT_WUC;
            }
        }
    }
    if (g_gop_handover.valid &&
        ranges_overlap(g_gop_handover.fb_hpa,
                       page_round_up(g_gop_handover.fb_size),
                       g_image_base, g_image_size)) {
        g_gop_handover = (LoongemuGopHandover){0};
        g_gop_fb_phys = 0;
        g_gop_width = 0;
        g_gop_height = 0;
        g_gop_stride = 0;
        LOG("GOP overlaps the protected VMM image; GOP disabled");
    }

    /* Capture host UEFI ACPI 2.0 RSDP and probe SPCR UART + MCFG 7A Split-CFG base. */
    g_vmm_handoff.magic = LOONGEMU_VMM_HANDOFF_MAGIC;
    g_vmm_handoff.version = LOONGEMU_VMM_HANDOFF_VERSION;
    g_vmm_handoff.vmm_image_base = g_image_base;
    g_vmm_handoff.vmm_image_size = g_image_size;
    g_vmm_handoff.gop = g_gop_handover;
    g_vmm_handoff.hide_host_dc_gpu = g_gop_handover.valid ? 1u : 0u;
    g_vmm_handoff.ls7a_cfg_type0_base = LOONGSON_7A_CFG_TYPE0_BASE;
    g_vmm_handoff.ls7a_cfg_type1_base = LOONGSON_7A_CFG_TYPE1_BASE;
    g_host_acpi_rsdp = uefi_find_acpi_rsdp(st);
    if (g_host_acpi_rsdp != 0) {
        uint8_t rev = 0;
        uint64_t xsdt_pa = 0;
        char oem[7];
        if (loongemu_acpi_validate_rsdp(g_host_acpi_rsdp, &rev, &xsdt_pa, oem)) {
            LoongemuAcpiSpcrInfo spcr;
            LoongemuAcpiMcfgInfo mcfg;
            g_vmm_handoff.host_acpi_rsdp = (uint64_t)(uintptr_t)g_host_acpi_rsdp;
            if (loongemu_acpi_find_spcr(g_host_acpi_rsdp, 0, &spcr) && spcr.base_addr != 0) {
                g_uart_phys = spcr.base_addr;
                g_uart_reg_shift = (spcr.access_size == 3u) ? 2u : 0u;
            }
            if (loongemu_acpi_find_mcfg(g_host_acpi_rsdp, 0, &mcfg) && mcfg.base_addr != 0) {
                g_vmm_handoff.ls7a_cfg_type0_base = mcfg.base_addr;
                g_vmm_handoff.ls7a_cfg_type1_base = mcfg.base_addr + UINT64_C(0x10000000);
                g_vmm_handoff.ls7a_pci_segment = mcfg.pci_segment;
                g_vmm_handoff.ls7a_start_bus = mcfg.start_bus;
                g_vmm_handoff.ls7a_end_bus = mcfg.end_bus;
                g_vmm_handoff.ls7a_mcfg_valid = 1u;
            }
            puts_("vmm: host ACPI RSDP=");
            puthex((uint64_t)(uintptr_t)g_host_acpi_rsdp);
            puts_(" uart=");
            puthex(g_uart_phys);
            puts_("\n");
        }
    }
    g_vmm_handoff.host_uart_phys = g_uart_phys;
    g_vmm_handoff.host_uart_reg_shift = g_uart_reg_shift;

    /* Allocate every handoff buffer before ExitBootServices. */
    addr = 0;
#ifdef VMM_STANDALONE_PROBE
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               VMM_GUEST_RAM_PAGES, &addr);
    if (EFI_ERROR(status)) { LOG("guest RAM alloc failed"); return 1; }
    g_guest_ram = addr;
    g_guest_ram_bytes = (uint64_t)VMM_GUEST_RAM_PAGES * LAPG_PAGE_SIZE;

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderCode,
                               VMM_CODE_CACHE_PAGES, &addr);
    if (EFI_ERROR(status)) { LOG("code cache alloc failed"); return 1; }
    g_code_cache = addr;

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               VMM_POOL_PAGES, &addr);
    if (EFI_ERROR(status)) { LOG("pool alloc failed"); return 1; }
    g_pool = addr;
#else
#ifdef LOONGEMU_PAYLOAD_FROM_ESP
    /*
     * Runtime payload: efi_payload_read() allocates page-granular loader
     * memory and reads the file into it, so no extra copy step is needed and
     * the sizes come from the files themselves rather than from link-time
     * symbols.  Both buffers must still survive ExitBootServices, which
     * EfiLoaderData does.
     */
    {
        uint64_t ovmf_code_size = 0;
        uint64_t ovmf_vars_size = 0;

        if (!efi_payload_read(bs, image, PAYLOAD_CODE_PATH,
                              &g_ovmf_code, &ovmf_code_size) ||
            !efi_payload_read(bs, image, PAYLOAD_VARS_PATH,
                              &g_ovmf_vars, &ovmf_vars_size)) {
            LOG("guest firmware not found on the boot volume");
            return 1;
        }
        g_vmm_handoff.ovmf_code_hpa = g_ovmf_code;
        g_vmm_handoff.ovmf_code_size = ovmf_code_size;
        g_vmm_handoff.ovmf_vars_hpa = g_ovmf_vars;
        g_vmm_handoff.ovmf_vars_size = ovmf_vars_size;
    }
#else
    uint64_t ovmf_code_file_size =
        (uint64_t)(_ovmf_code_end - _ovmf_code_start);
    uint64_t ovmf_vars_file_size =
        (uint64_t)(_ovmf_vars_end - _ovmf_vars_start);
    uint64_t ovmf_code_size = page_round_up(ovmf_code_file_size);
    uint64_t ovmf_vars_size = page_round_up(ovmf_vars_file_size);

    if (ovmf_code_size == 0 || ovmf_vars_size == 0) {
        LOG("invalid OVMF payload sizes");
        return 1;
    }
#endif

    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               VMM_GUEST_RAM_PAGES, &addr);
    if (EFI_ERROR(status)) { LOG("guest RAM alloc failed"); return 1; }
    g_guest_ram = addr;
    g_guest_ram_bytes = (uint64_t)VMM_GUEST_RAM_PAGES * LAPG_PAGE_SIZE;

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               VMM_FB_FALLBACK_PAGES, &addr);
    if (EFI_ERROR(status)) { LOG("fb fallback alloc failed"); return 1; }
    g_fb_fallback = addr;

#ifndef LOONGEMU_PAYLOAD_FROM_ESP
    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               (UINTN)(ovmf_code_size / LAPG_PAGE_SIZE), &addr);
    if (EFI_ERROR(status)) { LOG("OVMF code alloc failed"); return 1; }
    g_ovmf_code = addr;

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               (UINTN)(ovmf_vars_size / LAPG_PAGE_SIZE), &addr);
    if (EFI_ERROR(status)) { LOG("OVMF vars alloc failed"); return 1; }
    g_ovmf_vars = addr;
#endif

    g_vmm_handoff.guest_ram_hpa = g_guest_ram;
    g_vmm_handoff.guest_ram_size = g_guest_ram_bytes;
    g_vmm_handoff.fb_fallback_hpa = g_fb_fallback;
    g_vmm_handoff.fb_fallback_size =
        (uint64_t)VMM_FB_FALLBACK_PAGES * LAPG_PAGE_SIZE;
#ifndef LOONGEMU_PAYLOAD_FROM_ESP
    g_vmm_handoff.ovmf_code_hpa = g_ovmf_code;
    g_vmm_handoff.ovmf_code_size = ovmf_code_size;
    g_vmm_handoff.ovmf_vars_hpa = g_ovmf_vars;
    g_vmm_handoff.ovmf_vars_size = ovmf_vars_size;
#endif

#endif

    addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               VMM_KSTACK_PAGES, &addr);
    if (EFI_ERROR(status)) { LOG("kstack alloc failed"); return 1; }
    g_kstack_base = addr;
    if (!vmm_allocations_disjoint(0, 0)) {
        LOG("VMM allocations overlap or have invalid geometry");
        return 1;
    }

    /* Reserve enough map descriptors, then validate the final owned pool. */
    map_size = 0;
    status = bs->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
        LOG("initial memory map query failed");
        return 1;
    }
    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    }
    if (desc_size > UINT64_MAX / 16u ||
        map_size > UINT64_MAX - desc_size * 16u) {
        LOG("memory map size overflow");
        return 1;
    }
    map_cap = map_size + desc_size * 16u;
    status = bs->AllocatePool(EfiLoaderData, map_cap, &map_buf);
    if (EFI_ERROR(status)) { LOG("memory map pool alloc failed"); return 1; }
    if (map_buf == 0 ||
        !vmm_allocations_disjoint((uint64_t)(uintptr_t)map_buf, map_cap)) {
        LOG("memory map buffer overlaps VMM allocations");
        return 1;
    }
    /* All owned ranges are validated before writing to any allocated HPA. */
    copy_and_fill(g_guest_ram, 0, 0, g_guest_ram_bytes, 0);
#ifndef VMM_STANDALONE_PROBE
    copy_and_fill(g_fb_fallback, 0, 0,
                  (uint64_t)VMM_FB_FALLBACK_PAGES * LAPG_PAGE_SIZE, 0);
#ifndef LOONGEMU_PAYLOAD_FROM_ESP
    copy_and_fill(g_ovmf_code, _ovmf_code_start, ovmf_code_file_size,
                  g_vmm_handoff.ovmf_code_size, 0xffu);
    copy_and_fill(g_ovmf_vars, _ovmf_vars_start, ovmf_vars_file_size,
                  g_vmm_handoff.ovmf_vars_size, 0xffu);
#endif
#endif

    /*
     * Tight ExitBootServices retry loop: ZERO UEFI allocations, ZERO console
     * calls, and ZERO protocol calls between GetMemoryMap and ExitBootServices.
     */
    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = map_cap;
        status = bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                                  &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status)) {
            if (status == EFI_BUFFER_TOO_SMALL) {
                if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
                    desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
                }
                if (desc_size > UINT64_MAX / 16u ||
                    map_size > UINT64_MAX - desc_size * 16u) {
                    LOG("memory map size overflow");
                    return 1;
                }
                map_cap = map_size + desc_size * 16u;
                if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, map_cap, &map_buf))) {
                    return 1;
                }
                if (map_buf == 0 ||
                    !vmm_allocations_disjoint((uint64_t)(uintptr_t)map_buf,
                                              map_cap)) {
                    LOG("resized memory map buffer overlaps VMM allocations");
                    return 1;
                }
                continue;
            }
            return 1;
        }
        status = bs->ExitBootServices(image, map_key);
        if (!EFI_ERROR(status)) {
            uint64_t new_sp = (g_kstack_base + (uint64_t)VMM_KSTACK_PAGES * LAPG_PAGE_SIZE) &
                              ~UINT64_C(15);
            switch_stack_and_enter(new_sp);
        }
    }

    LOG("ExitBootServices failed after retries");
    return 1;
}
