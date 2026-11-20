#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include "cpu.h"
#include "internal.h"
#include "latm-system.h"

static int contains(const uint8_t *buf, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(buf + i, needle, n) == 0) return 1;
    }
    return 0;
}

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

static unsigned last_size;
static uint64_t last_off;
static uint64_t dev_read(void *opaque, uint64_t off, unsigned size)
{
    (void)opaque;
    last_off = off;
    last_size = size;
    return 0x1122334455667788ull;
}
static void dev_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    (void)opaque;
    last_off = off;
    last_size = size;
    (void)val;
}

int main(void)
{
    LatmMmioBus bus;
    LatmMemoryLayout mem;
    LatmBootInfo boot;
    static uint8_t dtb[8192];
    static uint8_t st[8192], strings[4096];
    uint64_t value;
    size_t dtb_len;

    latm_mmio_init(&bus);
    check(latm_mmio_add(&bus, 0x1000, 0x1000, LATM_MMIO_DEVICE, 0,
                        dev_read, dev_write, 0, "dev"), 1, "add mmio");
    check(latm_mmio_write(&bus, 0x1008, 8, 0xaabb), 1, "wide write");
    check(last_off, 8, "write offset");
    check(last_size, 8, "write exact width");
    check(latm_mmio_read(&bus, 0x1008, 4, &value), 1, "wide read");
    check(last_size, 4, "read exact width");
    check(latm_mmio_read(&bus, 0x1ffc, 8, &value), 0, "cross-region reject");
    check(latm_mmio_read(&bus, 0x2000, 1, &value), 0, "unmapped reject");

    check(latm_memory_setup_3a4000(&mem, 0x100000000ull, 0x20000000ull,
                                   0x100000000ull, 0x1000000ull,
                                   0x110000000ull, 0x1000000ull), 1,
          "3A4000 memory layout");
    check(mem.low_ram_size, 0x10000000ull, "low RAM 256 MiB");
    check(mem.high_ram_base, 0x90000000ull, "high RAM base");
    check(mem.high_ram_size, 0x10000000ull, "high RAM size");
    check(latm_memory_overlaps(&mem, 0x100000000ull, 1), 1,
          "reserved overlap detected");
    check(latm_memory_overlaps(&mem, 0x30000000ull, 1), 0,
          "hole not RAM");

    latm_boot_info_init(&boot);
    latm_boot_info_set_initrd(&boot, 0x02000000, 0x02800000);
    latm_boot_info_set_cmdline(&boot, "console=ttyS0 earlycon");
    latm_boot_info_set_dtb(&boot, 0x03000000, 0);
    dtb_len = latm_dtb_build_3a4000(dtb, sizeof(dtb), st, sizeof(st),
                                    strings, sizeof(strings), &mem, &boot);
    check(dtb_len > 0, 1, "DTB built");
    check((uint32_t)dtb[0] << 24 | (uint32_t)dtb[1] << 16 |
          (uint32_t)dtb[2] << 8 | dtb[3], 0xd00dfeedu, "DTB magic");
    check((uint32_t)dtb[4] << 24 | (uint32_t)dtb[5] << 16 |
          (uint32_t)dtb[6] << 8 | dtb[7], dtb_len, "DTB totalsize");
    check(contains(dtb, dtb_len, "loongson64v-4core-ls7a"), 1,
          "3A4000 compatible");
    check(contains(dtb, dtb_len, "linux,initrd-start"), 1,
          "initrd property");
    check(contains(dtb, dtb_len, "reserved-memory"), 1,
          "reserved-memory node");

    {
        CPUMIPSState env;
        memset(&env, 0, sizeof(env));
        latm_boot_info_apply_args(&boot, &env);
        check(env.active_tc.gpr[6], 0x03000000ull, "fw_arg2 DTB");
    }

    printf("latm-system: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
