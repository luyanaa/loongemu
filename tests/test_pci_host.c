#include "loongemu/pci_host.h"
#include "loongemu/phys_mem.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

typedef struct MockBarLog {
    uint64_t gpa;
    uint64_t size;
    int enabled;
    unsigned calls;
    int fail_next;
} MockBarLog;

static int mock_bar_cb(void *opaque, uint32_t bar_idx,
                       uint64_t new_gpa, uint64_t size, int enabled)
{
    MockBarLog *log = (MockBarLog *)opaque;
    if (!log) {
        return 0;
    }
    if (bar_idx != 0u) {
        return 1;
    }
    log->calls++;
    if (log->fail_next) {
        log->fail_next = 0;
        return 0;
    }
    log->gpa = new_gpa;
    log->size = size;
    log->enabled = enabled;
    return 1;
}

/* Mock 7A1000/7A2000 physical split-CFG config space */
typedef struct Mock7aDev {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint32_t dwords[64]; /* 256 bytes of config space */
} Mock7aDev;

static Mock7aDev g_mock_devs[16];
static unsigned g_mock_dev_count;
static unsigned g_mock_reads;
static unsigned g_mock_writes;
static uint8_t g_last_write_bus;
static uint8_t g_last_write_dev;
static uint32_t g_last_write_val;
static uint16_t g_last_write_reg;
static unsigned g_last_write_size;
static unsigned g_write_sizes[5];
static int g_mock_alias_bus1;
static void mock_7a_reset(void)
{
    memset(g_mock_devs, 0, sizeof(g_mock_devs));
    g_mock_dev_count = 0;
    g_mock_reads = 0;
    g_mock_writes = 0;
    g_last_write_bus = 0xffu;
    g_last_write_dev = 0xffu;
    g_last_write_val = 0;
    g_mock_alias_bus1 = 0;
    g_last_write_reg = 0;
    g_last_write_size = 0;
    memset(g_write_sizes, 0, sizeof(g_write_sizes));
}

static Mock7aDev *mock_7a_add(uint8_t bus, uint8_t dev, uint8_t fn,
                              uint16_t vid, uint16_t did)
{
    Mock7aDev *d = &g_mock_devs[g_mock_dev_count++];
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    d->dwords[0] = (uint32_t)vid | ((uint32_t)did << 16);
    return d;
}

static Mock7aDev *mock_7a_lookup(uint64_t phys_addr, uint16_t *out_dw_idx)
{
    uint16_t reg = (uint16_t)(((phys_addr >> 16) & 0xf00u) | (phys_addr & 0xfcu));
    uint8_t bus = (phys_addr >= LOONGSON_7A_CFG_TYPE1_BASE)
                      ? (uint8_t)((phys_addr >> 16) & 0xffu)
                      : 0u;
    uint8_t dev = (uint8_t)((phys_addr >> 11) & 0x1fu);
    uint8_t fn  = (uint8_t)((phys_addr >> 8) & 0x07u);

    if (out_dw_idx) {
        *out_dw_idx = (uint16_t)(reg >> 2);
    }
    for (unsigned i = 0; i < g_mock_dev_count; i++) {
        Mock7aDev *d = &g_mock_devs[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) {
            return d;
        }
    }
    if (g_mock_alias_bus1 && bus == 1u) {
        for (unsigned i = 0; i < g_mock_dev_count; i++) {
            Mock7aDev *d = &g_mock_devs[i];
            if (d->bus == bus && d->dev == 0u && d->fn == fn) {
                return d;
            }
        }
    }
    return 0;
}

static uint32_t mock_7a_read32(uint64_t phys_addr)
{
    g_mock_reads++;
    uint16_t dw_idx = 0;
    Mock7aDev *d = mock_7a_lookup(phys_addr, &dw_idx);
    if (!d || dw_idx >= 64u) {
        return 0xffffffffu;
    }
    return d->dwords[dw_idx];
}

static void mock_7a_write_config(uint64_t phys_addr, uint32_t val,
                                 unsigned size)
{
    uint16_t dw_idx = 0;
    uint16_t reg = (uint16_t)(((phys_addr >> 16) & 0xf00u) |
                              (phys_addr & 0xffu));
    uint8_t lane = (uint8_t)(reg & 3u);
    Mock7aDev *d = mock_7a_lookup(phys_addr, &dw_idx);
    g_mock_writes++;
    g_last_write_bus = (phys_addr >= LOONGSON_7A_CFG_TYPE1_BASE)
                           ? (uint8_t)((phys_addr >> 16) & 0xffu)
                           : 0u;
    g_last_write_dev = (uint8_t)((phys_addr >> 11) & 0x1fu);
    g_last_write_reg = reg;
    g_last_write_size = size;
    g_last_write_val = val;
    if (size <= 4u) {
        g_write_sizes[size]++;
    }
    if (!d || dw_idx >= 64u || size == 0u || size > 4u ||
        (unsigned)lane + size > 4u) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        uint16_t byte_reg = (uint16_t)(reg + i);
        unsigned shift = ((unsigned)lane + i) * 8u;
        uint8_t byte_val = (uint8_t)(val >> (i * 8u));
        uint8_t old_byte = (uint8_t)(d->dwords[dw_idx] >> shift);
        int w1c = byte_reg == 0x06u || byte_reg == 0x07u ||
                  ((byte_reg == 0x1eu || byte_reg == 0x1fu) &&
                   ((d->dwords[0x0cu >> 2] >> 16) & 0x7fu) == 1u) ||
                  (d->bus == 1u && d->dev == 0u &&
                   (byte_reg == 0x7au || byte_reg == 0x7bu));
        uint8_t new_byte = w1c ? (uint8_t)(old_byte & (uint8_t)~byte_val)
                               : byte_val;
        d->dwords[dw_idx] =
            (d->dwords[dw_idx] & ~(0xffu << shift)) |
            ((uint32_t)new_byte << shift);
    }
}

static void test_phys_mem_mat_and_remove(void)
{
    LoongemuMemMap map;
    LoongemuMemLookup lk;

    loongemu_mem_map_init(&map);
    CHECK(loongemu_mem_map_add(&map, 0x10000000ULL, 0x10000ULL, 0x10000000ULL,
                               LOONGEMU_MEM_RESERVED, "vmm_reserved") == 1);
    CHECK(loongemu_mem_map_add_mat(&map, 0x80000000ULL, 0x300000ULL, 0x10000000ULL,
                                   LOONGEMU_MEM_RAM, LOONGEMU_MAT_WUC, "overlap") == 0);
    CHECK(loongemu_mem_map_add_mat(&map, 0x80000000ULL, 0x300000ULL, 0x20000000ULL,
                                   LOONGEMU_MEM_RAM, LOONGEMU_MAT_WUC, "fb") == 1);
    CHECK(loongemu_mem_resolve(&map, 0x80000000ULL, 0x1000ULL, &lk) == LOONGEMU_MEM_RAM);
    CHECK(lk.host_mat == LOONGEMU_MAT_WUC);
    CHECK(lk.hpa == 0x20000000ULL);
    CHECK(loongemu_mem_map_remove(&map, 0x80000000ULL) == 1);
    CHECK(loongemu_mem_map_remove(&map, 0x80000000ULL) == 0);
    CHECK(loongemu_mem_resolve(&map, 0x80000000ULL, 0x1000ULL, &lk) == LOONGEMU_MEM_UNMAPPED);
}

static void test_bochs_gop_geometry_and_bar0(void)
{
    LoongemuPciHost host;
    LoongemuGopHandover gop;
    MockBarLog log;
    uint64_t val = 0;
    uint32_t initial_bar;
    uint32_t bar_before;
    uint32_t failed_bar;
    uint64_t fb_before;
    uint64_t mapped_before;
    unsigned calls_before;

    memset(&gop, 0, sizeof(gop));
    memset(&log, 0, sizeof(log));
    gop.fb_hpa = UINT64_C(0x90000000);
    gop.fb_size = UINT64_C(3) * 1024u * 1024u; /* 3 MiB non-pow2 */
    gop.width = 1024u;
    gop.height = 768u;
    gop.stride_px = 1024u;
    gop.bpp = 32u;
    gop.pixel_format = 1u;
    gop.valid = 1u;
    gop.owner = LOONGEMU_FB_OWNER_HOST_GOP;
    gop.host_mat = LOONGEMU_MAT_WUC;

    loongemu_pci_host_init(&host, &gop, mock_bar_cb, &log);
    CHECK(log.calls == 1u);
    CHECK(log.size == (UINT64_C(4) * 1024u * 1024u)); /* 3 MiB rounds up to 4 MiB BAR0 */
    CHECK(host.display.gop.owner == LOONGEMU_FB_OWNER_HOST_GOP);
    CHECK(host.display.gop.host_mat == LOONGEMU_MAT_WUC);
    initial_bar = loongemu_pci_cfg_read(&host, 0, 2, 0, 0x10u, 4u);

    /* PCI BAR0 size probe should also report 4 MiB */
    loongemu_pci_cfg_write(&host, 0, 2, 0, 0x10u, 0xffffffffu, 4u);
    CHECK(loongemu_pci_cfg_read(&host, 0, 2, 0, 0x10u, 4u) == (0xffc00000u | 0x08u));
    /* A rejected safe remap must leave the guest BAR and mapping unchanged. */
    loongemu_pci_cfg_write(&host, 0, 2, 0, 0x10u, initial_bar, 4u);
    bar_before = loongemu_pci_cfg_read(&host, 0, 2, 0, 0x10u, 4u);
    fb_before = host.display.fb_gpa;
    mapped_before = log.gpa;
    failed_bar = (bar_before & 0x003fffffu) |
                 ((bar_before & 0xffc00000u) + 0x01000000u);
    calls_before = log.calls;
    log.fail_next = 1;
    loongemu_pci_cfg_write(&host, 0, 2, 0, 0x10u, failed_bar, 4u);
    CHECK(log.calls > calls_before);
    CHECK(log.fail_next == 0);
    CHECK(loongemu_pci_cfg_read(&host, 0, 2, 0, 0x10u, 4u) == bar_before);
    CHECK(host.display.fb_gpa == fb_before);
    CHECK(log.gpa == mapped_before);
    /* Virtual config space has no bytes beyond its 256-byte configuration space. */
    CHECK(loongemu_pci_cfg_read(&host, 0, 2, 0, 0x00ffu, 2u) == 0u);
    loongemu_pci_cfg_write(&host, 0, 2, 0, 0x00ffu, 0xffffu, 2u);
    CHECK(loongemu_pci_cfg_read(&host, 0, 2, 0, 0x00ffu, 2u) == 0u);

    /* OOB DISPI geometry writes via MMIO must be rejected */
    loongemu_bochs_mmio_write(&host.display, 0x500u + (1u << 1), 2048u, 2u); /* XRES=2048 */
    CHECK(loongemu_bochs_mmio_read(&host.display, 0x502u, 2u) == 1024u);
    loongemu_bochs_mmio_write(&host.display, 0x500u + (6u << 1), 4096u, 2u); /* VIRT_WIDTH=4096 */
    CHECK(loongemu_bochs_mmio_read(&host.display, 0x50cu, 2u) == 1024u);
    loongemu_bochs_mmio_write(&host.display, 0x500u + (9u << 1), 1u, 2u);    /* Y_OFFSET=1 pushes total lines to 769 */
    CHECK(loongemu_bochs_mmio_read(&host.display, 0x512u, 2u) == 0u);

    /* OOB DISPI geometry writes via I/O ports must also be rejected */
    CHECK(loongemu_pci_io_write(&host, LOONGEMU_BOCHS_DISPI_INDEX, 2, 1u) == 1);
    CHECK(loongemu_pci_io_write(&host, LOONGEMU_BOCHS_DISPI_DATA, 2, 1920u) == 1);
    CHECK(loongemu_pci_io_read(&host, LOONGEMU_BOCHS_DISPI_DATA, 2, &val) == 1);
    CHECK(val == 1024u);

    /* Valid downsize (800x600@32) succeeds */
    loongemu_bochs_mmio_write(&host.display, 0x500u + (1u << 1), 800u, 2u);
    loongemu_bochs_mmio_write(&host.display, 0x500u + (2u << 1), 600u, 2u);
    CHECK(loongemu_bochs_mmio_read(&host.display, 0x502u, 2u) == 800u);
    CHECK(loongemu_bochs_mmio_read(&host.display, 0x504u, 2u) == 600u);
}

static void test_ls7a_blacklist_flag_dev_fix_and_mrrs(void)
{
    LoongemuPciHost host;
    unsigned reads_before;
    unsigned writes_before;
    uint64_t port_value = 0;
    unsigned byte_writes_before;
    unsigned word_writes_before;

    mock_7a_reset();
    loongemu_pci_host_init(&host, 0, 0, 0);
    /* Passing hide_host_dc_gpu=0 must not disable the mandatory blacklist. */
    loongemu_pci_host_attach_ls7a(&host, LOONGSON_7A_CFG_TYPE0_BASE,
                                  LOONGSON_7A_CFG_TYPE1_BASE, 0,
                                  mock_7a_read32, mock_7a_write_config);

    mock_7a_add(0, 6, 0, 0x0014u, 0x7a06u);
    mock_7a_add(0, 7, 0, 0x0014u, 0x7a36u);
    mock_7a_add(0, 15, 0, 0x0014u, 0x7a15u);
    mock_7a_add(0, 16, 0, 0x0014u, 0x7a25u);

    /* 7A internal Root Port 00:09.0 -> SecBus=1, SubBus=1. */
    Mock7aDev *rp = mock_7a_add(0, 9, 0, 0x0014u, 0x7a09u);
    rp->dwords[0x08u >> 2] = 0x06040000u; /* PCI-to-PCI bridge class */
    rp->dwords[0x0cu >> 2] = 0x00010000u; /* Header Type = 0x01 */
    rp->dwords[0x18u >> 2] = 0x00010100u; /* Pri=0, Sec=1, Sub=1 */
    /* A 7A bridge-ID lookalike is not topology without PCI-to-PCI class code. */
    Mock7aDev *bad_rp = mock_7a_add(0, 10, 0, 0x0014u, 0x7a19u);
    bad_rp->dwords[0x0cu >> 2] = 0x00010000u;
    bad_rp->dwords[0x18u >> 2] = 0x00020200u; /* Pri=0, Sec=2, Sub=2 */

    /* Downstream NVMe endpoint at 01:00.0, PCIe Cap at 0x70. */
    Mock7aDev *nvme = mock_7a_add(1, 0, 0, 0x144du, 0xa808u);
    nvme->dwords[0x04u >> 2] = 0x00100007u; /* Status bit 4 = Capabilities List */
    nvme->dwords[0x34u >> 2] = 0x00000050u; /* First Cap at 0x50 */
    nvme->dwords[0x50u >> 2] = 0x00007001u; /* PM Cap (0x01), next=0x70 */
    nvme->dwords[0x70u >> 2] = 0x00020010u; /* PCIe Cap (0x10) */
    nvme->dwords[0x78u >> 2] = 0xa5a52010u; /* DevCtl MRRS=512 B; DevStatus W1C */
    g_mock_alias_bus1 = 1;

    /* Bus 2 has only the malformed 7A bridge-ID lookalike; dev 0 is absent. */
    Mock7aDev *unattached = mock_7a_add(2, 1, 0, 0x1234u, 0x5678u);
    unattached->dwords[0x7cu >> 2] = 0x44332211u;
    unattached->dwords[0x80u >> 2] = 0x88776655u;
    /* Same 7A device number from another vendor is not part of the blacklist. */
    mock_7a_add(2, 2, 0, 0x1234u, 0x7a06u);

    /* PCIe on bus 0 is not subject to the downstream MRRS policy. */
    Mock7aDev *upstream = mock_7a_add(0, 3, 0, 0x1234u, 0x1111u);
    upstream->dwords[0x04u >> 2] = 0x00100000u;
    upstream->dwords[0x34u >> 2] = 0x00000040u;
    upstream->dwords[0x40u >> 2] = 0x00000010u;
    upstream->dwords[0x48u >> 2] = 0x00002010u;

    /* Every physical 7A DC/GPU ID is hidden, independent of the legacy flag. */
    CHECK(loongemu_pci_cfg_read(&host, 0, 6, 0, 0x00u, 4u) == 0xffffffffu);
    CHECK(loongemu_pci_cfg_read(&host, 0, 7, 0, 0x00u, 1u) == 0xffffffffu);
    CHECK(loongemu_pci_cfg_read(&host, 0, 15, 0, 0x00u, 2u) == 0xffffffffu);
    CHECK(loongemu_pci_cfg_read(&host, 0, 16, 0, 0x00u, 4u) == 0xffffffffu);
    loongemu_pci_cfg_write(&host, 0, 6, 0, 0x04u, 0x7u, 2u);
    loongemu_pci_cfg_write(&host, 0, 7, 0, 0x04u, 0x7u, 2u);
    loongemu_pci_cfg_write(&host, 0, 15, 0, 0x04u, 0x7u, 2u);
    loongemu_pci_cfg_write(&host, 0, 16, 0, 0x04u, 0x7u, 2u);
    CHECK(g_mock_writes == 0u);
    CHECK(loongemu_pci_cfg_read(&host, 2, 2, 0, 0x00u, 4u) == 0x7a061234u);

    /* FLAG_DEV_FIX applies only to bus 1, which is a root port's secondary bus. */
    CHECK(loongemu_pci_cfg_read(&host, 1, 0, 0, 0x00u, 4u) == 0xa808144du);
    CHECK((nvme->dwords[0x78u >> 2] & (0x7u << 12)) == (1u << 12));
    CHECK((nvme->dwords[0x78u >> 2] >> 16) == 0xa5a5u);
    CHECK(g_mock_writes == 1u);
    CHECK(g_last_write_size == 2u);
    CHECK(g_last_write_reg == 0x78u);
    CHECK(g_last_write_val == 0x1010u);
    CHECK(loongemu_pci_cfg_read(&host, 1, 1, 0, 0x00u, 4u) == 0xffffffffu);
    CHECK(loongemu_pci_cfg_read(&host, 1, 31, 0, 0x00u, 4u) == 0xffffffffu);
    loongemu_pci_cfg_write(&host, 1, 1, 0, 0x04u, 0x7u, 2u);
    CHECK(g_mock_writes == 1u);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x00u, 4u) == 0x56781234u);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x00u, 1u) == 0x34u);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x00u, 2u) == 0x1234u);

    /* Straddling config accesses handle both dwords and retain untouched bytes. */
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x7fu, 2u) == 0x5544u);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x7fu, 4u) == 0x77665544u);
    loongemu_pci_cfg_write(&host, 2, 1, 0, 0x7fu, 0xbbaau, 2u);
    CHECK(unattached->dwords[0x7cu >> 2] == 0xaa332211u);
    CHECK(unattached->dwords[0x80u >> 2] == 0x887766bbu);

    /* Out-of-range physical config offsets must not reach the callbacks. */
    reads_before = g_mock_reads;
    writes_before = g_mock_writes;
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x1000u, 1u) == 0xffffffffu);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x0fffu, 2u) == 0xffffffffu);
    loongemu_pci_cfg_write(&host, 2, 1, 0, 0x1000u, 0u, 4u);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x20u, 3u) == 0xffffffffu);
    loongemu_pci_cfg_write(&host, 2, 1, 0, 0x20u, 0xffffffffu, 3u);
    CHECK(g_mock_reads == reads_before);
    CHECK(g_mock_writes == writes_before);
    loongemu_pci_io_write(&host, LOONGEMU_PCI_CFG_ADDR_PORT, 4u,
                          0x80000000u | (2u << 16) | (1u << 11) | 0xfcu);
    reads_before = g_mock_reads;
    writes_before = g_mock_writes;
    CHECK(loongemu_pci_io_read(&host, LOONGEMU_PCI_CFG_DATA_PORT + 3u,
                               2, &port_value) == 1);
    CHECK(port_value == 0xffffffffu);
    loongemu_pci_io_write(&host, LOONGEMU_PCI_CFG_DATA_PORT + 3u, 4,
                          0xffffffffu);
    CHECK(g_mock_reads == reads_before);
    CHECK(g_mock_writes == writes_before);
    writes_before = g_mock_writes;
    loongemu_pci_cfg_write(&host, 2, 1, 0, 0x00u, 0xffffffffu, 4u);
    CHECK(g_mock_writes == writes_before);
    CHECK(loongemu_pci_cfg_read(&host, 2, 1, 0, 0x00u, 4u) == 0x56781234u);
    reads_before = g_mock_reads;
    writes_before = g_mock_writes;
    CHECK(loongemu_pci_ecam_read(&host, LOONGEMU_PCI_ECAM_SIZE, 4u) ==
          0xffffffffu);
    CHECK(loongemu_pci_ecam_read(&host, LOONGEMU_PCI_ECAM_SIZE - 1u, 2u) ==
          0xffffffffu);
    loongemu_pci_ecam_write(&host, LOONGEMU_PCI_ECAM_SIZE, 0u, 4u);
    CHECK(g_mock_reads == reads_before);
    CHECK(g_mock_writes == writes_before);

    /* Unaligned bridge writes issue native cycles and leave unselected W1C bytes untouched. */
    rp->dwords[0x1cu >> 2] = 0xa5a50000u;
    writes_before = g_mock_writes;
    byte_writes_before = g_write_sizes[1];
    word_writes_before = g_write_sizes[2];
    loongemu_pci_cfg_write(&host, 0, 9, 0, 0x1bu, 0x00221100u, 4u);
    CHECK((rp->dwords[0x1cu >> 2] >> 16) == 0xa5a5u);
    CHECK((rp->dwords[0x1cu >> 2] & 0xffffu) == 0x2211u);
    CHECK(g_mock_writes == writes_before + 3u);
    CHECK(g_write_sizes[1] == byte_writes_before + 2u);
    CHECK(g_write_sizes[2] == word_writes_before + 1u);
    CHECK(g_last_write_reg == 0x1eu && g_last_write_size == 1u);

    /* Command writes must not clear adjacent W1C PCI Status bits. */
    loongemu_pci_cfg_write(&host, 1, 0, 0, 0x04u, 0x0007u, 2u);
    CHECK((nvme->dwords[0x04u >> 2] >> 16) == 0x0010u);
    CHECK((nvme->dwords[0x04u >> 2] & 0xffffu) == 0x0007u);

    /* Word, byte, and dword Device Control writes all clamp MRRS to 256 B. */
    loongemu_pci_cfg_write(&host, 1, 0, 0, 0x78u, 0x5930u, 2u);
    CHECK(g_last_write_reg == 0x78u && g_last_write_size == 2u);
    CHECK(g_last_write_val == 0x1930u);
    CHECK((loongemu_pci_cfg_read(&host, 1, 0, 0, 0x78u, 2u) &
           (0x7u << 12)) == (1u << 12));
    CHECK((loongemu_pci_cfg_read(&host, 1, 0, 0, 0x78u, 2u) &
           0x0fffu) == 0x0930u);
    CHECK((nvme->dwords[0x78u >> 2] >> 16) == 0xa5a5u);

    loongemu_pci_cfg_write(&host, 1, 0, 0, 0x79u, 0x39u, 1u);
    CHECK(g_last_write_reg == 0x79u && g_last_write_size == 1u);
    CHECK(g_last_write_val == 0x19u);
    CHECK((loongemu_pci_cfg_read(&host, 1, 0, 0, 0x78u, 2u) &
           (0x7u << 12)) == (1u << 12));
    CHECK((nvme->dwords[0x78u >> 2] >> 16) == 0xa5a5u);

    loongemu_pci_cfg_write(&host, 1, 0, 0, 0x78u, 0x00005930u, 4u);
    CHECK(g_last_write_reg == 0x78u && g_last_write_size == 4u);
    CHECK(g_last_write_val == 0x1930u);
    CHECK((nvme->dwords[0x78u >> 2] & 0xffffu) == 0x1930u);
    CHECK((nvme->dwords[0x78u >> 2] >> 16) == 0xa5a5u);

    loongemu_pci_cfg_write(&host, 1, 0, 0, 0x78u, 0x0930u, 2u);
    CHECK((loongemu_pci_cfg_read(&host, 1, 0, 0, 0x78u, 2u) &
           (0x7u << 12)) == 0u);

    /* Upstream PCIe and non-PCIe downstream writes are not spuriously clamped. */
    loongemu_pci_cfg_write(&host, 0, 3, 0, 0x48u, 0x00005000u, 4u);
    CHECK(upstream->dwords[0x48u >> 2] == 0x00005000u);
    loongemu_pci_cfg_write(&host, 2, 1, 0, 0x78u, 0x00005000u, 4u);
    CHECK(unattached->dwords[0x78u >> 2] == 0x00005000u);
}
static void test_ls7a_passthrough_requires_callbacks(void)
{
    LoongemuPciHost host;

    mock_7a_reset();
    loongemu_pci_host_init(&host, 0, 0, 0);
    loongemu_pci_host_attach_ls7a(&host, LOONGSON_7A_CFG_TYPE0_BASE,
                                  LOONGSON_7A_CFG_TYPE1_BASE, 0, 0, 0);
    CHECK(host.ls7a.enabled == 0);
    CHECK(loongemu_pci_cfg_read(&host, 1, 0, 0, 0x00u, 4u) == 0xffffffffu);
    loongemu_pci_cfg_write(&host, 1, 0, 0, 0x04u, 0x7u, 2u);
    CHECK(g_mock_reads == 0u);
    CHECK(g_mock_writes == 0u);
}
static void test_ls7a_readonly_backend_hides_high_mrrs(void)
{
    LoongemuPciHost host;
    mock_7a_reset();
    loongemu_pci_host_init(&host, 0, 0, 0);
    Mock7aDev *dev = mock_7a_add(1, 0, 0, 0x144du, 0xa808u);
    dev->dwords[0x04u >> 2] = 0x00100000u;
    dev->dwords[0x34u >> 2] = 0x00000040u;
    dev->dwords[0x40u >> 2] = 0x00000010u;
    dev->dwords[0x48u >> 2] = 0x00002010u;
    loongemu_pci_host_attach_ls7a(&host, LOONGSON_7A_CFG_TYPE0_BASE,
                                  LOONGSON_7A_CFG_TYPE1_BASE, 0,
                                  mock_7a_read32, 0);
    CHECK(host.ls7a.enabled == 1);
    CHECK(loongemu_pci_cfg_read(&host, 1, 0, 0, 0x00u, 4u) == 0xffffffffu);
    CHECK(((dev->dwords[0x48u >> 2] >> 12) & 0x7u) == 2u);
    CHECK(g_mock_writes == 0u);
}


int main(void)
{
    test_phys_mem_mat_and_remove();
    test_bochs_gop_geometry_and_bar0();
    test_ls7a_blacklist_flag_dev_fix_and_mrrs();
    test_ls7a_passthrough_requires_callbacks();
    test_ls7a_readonly_backend_hides_high_mrrs();

    printf("pci_host: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
