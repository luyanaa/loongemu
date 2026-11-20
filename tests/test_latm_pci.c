#include <stdio.h>
#include <string.h>
#include "latm-pci.h"

static int checks, fails;
static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s got=%llx want=%llx\n", what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

static uint32_t mock_cfg[64][32][8];
static uint32_t mock_read(uint64_t addr)
{
    /* 7A split-CFG address layout: use low 24 bits as a synthetic index. */
    unsigned bus = (unsigned)((addr >> 16) & 0xff);
    unsigned dev = (unsigned)((addr >> 11) & 0x1f);
    unsigned fn = (unsigned)((addr >> 8) & 7);
    unsigned reg = (unsigned)(addr & 0xfc);
    if (bus < 64 && reg < 0x100) return mock_cfg[bus][dev][fn] + reg;
    return 0xffffffffu;
}
static void mock_write(uint64_t addr, uint32_t value)
{
    (void)addr; (void)value;
}

int main(void)
{
    LatmPci pci;
    LatmMmioBus bus;
    uint64_t v;

    memset(mock_cfg, 0xff, sizeof(mock_cfg));
    mock_cfg[0][6][0] = 0x7a060014u; /* 7A1000 DC */
    latm_pci_init(&pci, NULL);
    latm_pci_attach_ls7a_mock(&pci, LOONGSON_7A_CFG_TYPE0_BASE,
                              LOONGSON_7A_CFG_TYPE1_BASE,
                              mock_read, mock_write);
    check(latm_pci_cfg_read(&pci, 0, 6, 0, 0, 4), 0xffffffffu,
          "7A DC blacklisted");
    check(latm_pci_cfg_read(&pci, 0, 2, 0, 0, 4) & 0xffff, 0x1234u,
          "virtual VGA visible");

    latm_mmio_init(&bus);
    check(latm_pci_map_bochs_mmio(&pci, &bus), 1, "map Bochs BAR");
    check(latm_mmio_read(&bus, pci.host.display.mmio_gpa, 4, &v), 1,
          "BAR MMIO read");
    check(v != 0, 1, "BAR EDID nonempty");
    check(latm_pci_quiesce(&pci), 0, "mock quiesce no host device");
    check(latm_pci_quiesce(&(LatmPci){0}), 1, "no-op quiesce");

    printf("latm-pci: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
