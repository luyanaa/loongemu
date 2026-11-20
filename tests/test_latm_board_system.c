#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-system-board.h"

static const LatmPlatform *test_platform;
void latm_platform_set(const LatmPlatform *platform) { test_platform = platform; }
const LatmPlatform *latm_platform_get(void) { return test_platform; }

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
    check(latm_system_mmio_read(&sys, LOONGSON_7A_CFG_TYPE0_BASE, 4, &v), 1,
          "7A type0 cfg readable");
    check(v & 0xffffu, 0x8086u, "7A type0 vendor");
    check(latm_system_mmio_write(&sys, LATM_MAILBOX_BASE + 0x20, 8,
                                 0xdeadbeefcafef00dull), 1, "mailbox write");
    check(latm_system_mmio_read(&sys, LATM_MAILBOX_BASE + 0x20, 8, &v), 1,
          "mailbox read");
    check(v, 0xdeadbeefcafef00dull, "mailbox value");

    uint8_t elf[64 + 56 + 8] = { 0 };
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    elf[16] = 2; elf[18] = 8; elf[20] = 1;
    elf[24] = 0x00; elf[25] = 0x10; /* entry 0x1000 */
    elf[32] = 64; elf[54] = 56; elf[56] = 1;
    elf[64] = 1; /* PT_LOAD */
    elf[64 + 8] = 64 + 56; /* file offset */
    elf[64 + 16] = 0x00; elf[64 + 17] = 0x10; /* vaddr */
    elf[64 + 24] = 0x00; elf[64 + 25] = 0x10; /* paddr */
    elf[64 + 32] = 8; elf[64 + 40] = 16;
    elf[64 + 56] = 0xaa; elf[64 + 57] = 0xbb;
    uint64_t entry = 0;
    check(latm_system_load_elf(&sys, elf, sizeof(elf), &entry), 1,
          "vmlinux ELF load");
    check(entry, 0x1000, "ELF entry");
    uint64_t code;
    check(latm_system_mmio_read(&sys, 0x1000, 2, &code), 1, "ELF code");
    check(code, 0xbbaa, "ELF bytes");

    uint8_t helf[64 + 56 + 8];
    memcpy(helf, elf, sizeof(helf));
    uint64_t h = 0xffffffff80200000ull;
    for (unsigned i = 0; i < 8; i++) {
        helf[24 + i] = (uint8_t)(h >> (8 * i));
        helf[64 + 16 + i] = (uint8_t)(h >> (8 * i));
        helf[64 + 24 + i] = (uint8_t)(h >> (8 * i));
    }
    uint64_t hentry = 0;
    check(latm_system_load_elf(&sys, helf, sizeof(helf), &hentry), 1,
          "KSEG0 vmlinux load");
    check(hentry, h, "KSEG0 entry");
    check(latm_system_mmio_read(&sys, 0x200000, 2, &code), 1,
          "KSEG0 translated GPA");
    check(code, 0xbbaa, "KSEG0 bytes");
    {
        CPUMIPSState benv;
        memset(&benv, 0, sizeof(benv));
        check(latm_system_boot(&sys, elf, sizeof(elf), NULL, 0,
                               0x03000000, 0x04000000, "console=ttyS0",
                               &benv), 1, "system boot path");
        check(benv.active_tc.PC, 0x1000, "boot entry PC");
        check(benv.active_tc.gpr[6], 0x03000000, "boot fw_arg2");
    }

    const LatmPlatform *p = latm_system_platform(&sys);
    check(p != NULL, 1, "system platform");
    p->iocsr_write(NULL, 0x14a0, 0x11223344, 4);
    check(p->iocsr_read(NULL, 0x14a0, 4), 0x11223344, "IOCSR readback");
    latm_uncore_extioi_route(&sys.uncore, 9, 2, 4, 1);
    latm_uncore_extioi_raise(&sys.uncore, 9);
    check(latm_uncore_cpu_ip(&sys.uncore, 2), 1u << 12, "system IP4");
    p->iocsr_write(NULL, 0x1800, 1u << 9, 4);
    check(latm_uncore_cpu_ip(&sys.uncore, 2), 0, "system IOCSR EOI");
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
        check(env.active_tc.PC, entry, "system entry PC");
    }

    {
        static uint8_t ram2[0x100000] __attribute__((aligned(4096)));
        LatmSystem sys2;
        LoongemuVmmHandoff h;
        memset(&h, 0, sizeof(h));
        h.magic = LOONGEMU_VMM_HANDOFF_MAGIC;
        h.version = LOONGEMU_VMM_HANDOFF_VERSION;
        h.guest_ram_hpa = 0x100000000ull;
        h.guest_ram_size = sizeof(ram2);
        h.ls7a_cfg_type0_base = LOONGSON_7A_CFG_TYPE0_BASE;
        h.ls7a_cfg_type1_base = LOONGSON_7A_CFG_TYPE1_BASE;
        check(latm_system_init_from_handoff(&sys2, &h, ram2), 1,
              "UEFI handoff init");
        check(latm_system_mmio_read(&sys2, LOONGSON_7A_CFG_TYPE0_BASE, 4, &v),
              1, "handoff split cfg");
        check(v & 0xffffu, 0x8086u, "handoff split cfg vendor");
        latm_system_bind_platform(&sys);
    }

    printf("latm-board-system: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
