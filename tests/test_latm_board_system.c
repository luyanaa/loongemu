#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-system-board.h"

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

int main(void)
{
    static uint8_t ram[0x2000000] __attribute__((aligned(4096)));
    static uint8_t dtb[8192], st[8192], strings[4096];
    LatmSystem sys;
    uint64_t v;
    size_t len;

    check(latm_system_init_3a4000(&sys, ram, 0x100000000ull, sizeof(ram),
                                  0x100000000ull, 0x400000ull,
                                  0x100400000ull, 0x400000ull), 1,
          "system init");
    check(latm_system_mmio_write(&sys, 0x1008, 8, 0x1122334455667788ull), 1,
          "RAM wide write");
    check(latm_system_mmio_read(&sys, 0x1008, 8, &v), 1, "RAM wide read");
    check(v, 0x1122334455667788ull, "RAM value");
    check(latm_system_mmio_write(&sys, LATM_MAILBOX_BASE + 0x10, 8,
                                 0xdeadbeefcafef00dull), 1, "mailbox write");
    check(latm_system_mmio_read(&sys, LATM_MAILBOX_BASE + 0x10, 8, &v), 1,
          "mailbox read");
    check(v, 0xdeadbeefcafef00dull, "mailbox value");
    check(latm_system_mmio_write(&sys, 0x1fe001e3, 1, 0x80), 1, "UART DLAB");
    check(latm_system_mmio_write(&sys, 0x1fe001e0, 1, 1), 1, "UART DLL");
    check(latm_system_mmio_write(&sys, 0x1fe001e3, 1, 3), 1, "UART LCR");
    check(latm_system_mmio_write(&sys, 0x1fe001e1, 1, 3), 1, "UART IER");
    check(latm_system_mmio_write(&sys, 0x1fe001e0, 1, 'A'), 1, "UART THR");
    check(sys.uart.tx_count, 1, "UART TX count");
    check(latm_system_mmio_read(&sys, 0x1fe001e5, 1, &v), 1, "UART LSR");
    check(v & 0x60, 0x60, "UART THRE/TEMT");

    latm_system_set_boot_args(&sys, 0x03000000, 0x02000000, 0x02800000,
                              "console=ttyS0 earlycon");
    len = latm_system_build_dtb(&sys, dtb, sizeof(dtb), st, sizeof(st),
                                strings, sizeof(strings));
    check(len > 0, 1, "system DTB");
    check((uint32_t)dtb[0] << 24 | (uint32_t)dtb[1] << 16 |
          (uint32_t)dtb[2] << 8 | dtb[3], 0xd00dfeedu, "system DTB magic");

    {
        CPUMIPSState env;
        memset(&env, 0, sizeof(env));
        latm_system_apply_boot_args(&sys, &env);
        check(env.active_tc.gpr[6], 0x03000000ull, "system fw_arg2");
    }

    printf("latm-board-system: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
