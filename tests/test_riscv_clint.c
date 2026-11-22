#include <stdio.h>
#include <string.h>
#include "loongemu/riscv_clint.h"

static int checks, failures;
#define CHECK(c, n) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "CLINT: FAIL %s\n", n); } } while (0)

int main(void)
{
    LoongemuClint clint;
    uint64_t mip = 0;

    loongemu_clint_init(&clint, 2);
    /* Power-on comparators must be far in the future: a zero comparator makes
     * MTIP assert on the very first tick, before firmware programs the timer. */
    CHECK(clint.mtimecmp[0] == UINT64_MAX, "MTIMECMP hart 0 resets to max");
    CHECK(clint.mtimecmp[1] == UINT64_MAX, "MTIMECMP hart 1 resets to max");
    mip = 0;
    loongemu_clint_tick(&clint, 1, &mip, 0);
    CHECK((mip & (1ULL << 7)) == 0, "no boot-time MTIP before MTIMECMP is set");
    CHECK((mip & (1ULL << 3)) == 0, "no boot-time MSIP");
    loongemu_clint_write(&clint, 0x0000, 1, 4);
    CHECK(loongemu_clint_read(&clint, 0x0000, 4) == 1, "MSIP hart 0");
    loongemu_clint_write(&clint, 0x0004, 1, 4);
    CHECK(loongemu_clint_read(&clint, 0x0004, 4) == 1, "MSIP hart 1");
    loongemu_clint_write(&clint, 0x4000, 100, 8);
    CHECK(loongemu_clint_read(&clint, 0x4000, 8) == 100, "MTIMECMP hart 0");
    loongemu_clint_write(&clint, 0x4008, 200, 8);
    CHECK(loongemu_clint_read(&clint, 0x4008, 8) == 200, "MTIMECMP hart 1");
    loongemu_clint_write(&clint, 0xbff8, 150, 8);
    CHECK(loongemu_clint_read(&clint, 0xbff8, 8) == 150, "MTIME");

    loongemu_clint_tick(&clint, 50, &mip, 0);
    CHECK((mip & (1ULL << 3)) != 0, "MSIP reflected to MIP");
    CHECK((mip & (1ULL << 7)) == 0, "MTIP clear below cmp");
    loongemu_clint_tick(&clint, 100, &mip, 0);
    CHECK((mip & (1ULL << 7)) != 0, "MTIP set at cmp");
    loongemu_clint_tick(&clint, 99, &mip, 1);
    CHECK((mip & (1ULL << 7)) == 0, "MTIP clear for hart 1");
    CHECK((mip & (1ULL << 3)) != 0, "MSIP for hart 0 unaffected by hart 1");

    mip = 0;
    LoongemuMmioBus bus;
    loongemu_mmio_bus_init(&bus);
    CHECK(loongemu_clint_attach(&bus, &clint, 0x2000000) == 1,
          "CLINT attaches to the bus");
    CHECK(loongemu_mmio_bus_write(&bus, 0x2000000, 4, 1) == 0,
          "MMIO writes MSIP");
    {
        uint64_t value = 0;
        CHECK(loongemu_mmio_bus_read(&bus, 0x2000000, 4, &value) == 0 &&
              value == 1, "MMIO reads MSIP");
    }

    printf("CLINT: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
