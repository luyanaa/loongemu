#include <stdio.h>
#include <string.h>
#include "latm-uncore.h"

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
    LatmUncore u;
    uint64_t v;

    latm_uncore_init(&u);
    check(latm_uncore_mailbox_write(&u, LATM_MAILBOX_BASE + 0x20, 8,
                                    0x1122334455667788ull), 1, "mailbox write");
    check(latm_uncore_mailbox_read(&u, LATM_MAILBOX_BASE + 0x20, 8, &v), 1,
          "mailbox read");
    check(v, 0x1122334455667788ull, "mailbox value");
    latm_uncore_mailbox_send(&u, 2, 1, 0xcafebabe);
    latm_uncore_mailbox_irq(&u, 2);
    check(u.mailbox[2].buf[1], 0xcafebabe, "mailbox core2 buf1");
    check(u.mailbox[2].status & 1, 1, "mailbox core2 irq");

    latm_uncore_pic_raise(&u, 9, 1);
    check(u.pic_pending & (1ull << 9), 1ull << 9, "PIC line 9");
    latm_uncore_pic_write(&u, 0x20, 8, 1ull << 9);
    check(u.pic_mask & (1ull << 9), 1ull << 9, "PIC mask");
    latm_uncore_pic_eoi(&u, 9);
    check(u.pic_pending & (1ull << 9), 0, "PIC EOI");

    latm_uncore_extioi_route(&u, 7, 2, 4, 0);
    check(latm_uncore_msi_write(&u, LATM_PCH_MSI_BASE, 4, 7), 1,
          "MSI write");
    check(latm_uncore_cpu_ip(&u, 2), 1u << 12, "CPU2 IP4");
    latm_uncore_extioi_eoi(&u, 7);
    check(latm_uncore_cpu_ip(&u, 2), 0, "EXTIOI EOI");

    latm_uncore_extioi_route(&u, 31, 3, 6, 1);
    latm_uncore_extioi_raise(&u, 31);
    check(latm_uncore_cpu_ip(&u, 3), 1u << 14, "CPU3 IP6");
    check(latm_uncore_cpu_ip(&u, 0), 0, "CPU0 unaffected");

    /* Two vectors sharing one CPU/IP must not clear each other on EOI. */
    latm_uncore_extioi_route(&u, 40, 1, 5, 1);
    latm_uncore_extioi_route(&u, 41, 1, 5, 1);
    latm_uncore_extioi_raise(&u, 40);
    latm_uncore_extioi_raise(&u, 41);
    check(latm_uncore_cpu_ip(&u, 1), 1u << 13, "shared IP raised");
    latm_uncore_extioi_eoi(&u, 40);
    check(latm_uncore_cpu_ip(&u, 1), 1u << 13, "EOI one keeps peer");
    latm_uncore_extioi_eoi(&u, 41);
    check(latm_uncore_cpu_ip(&u, 1), 0, "EOI last clears IP");

    /* Linux EXTIOI IOCSR ABI: IPMAP/ENABLE/COREMAP/COREISR at
     * 0x1400 + {0xc0,0x200,0x800,0x400}. */
    latm_uncore_iocsr_write(&u, 0x14c0, 0x00000001, 4);
    latm_uncore_iocsr_write(&u, 0x1600, 1u << 10, 4);
    latm_uncore_iocsr_write(&u, 0x1c08, 0x00020000, 4);
    latm_uncore_extioi_raise(&u, 10);
    check(latm_uncore_cpu_ip(&u, 2), 1u << 11, "IOCSR coremap IP3");
    latm_uncore_iocsr_write(&u, 0x1800, 1u << 10, 4);
    check(latm_uncore_cpu_ip(&u, 2), 0, "IOCSR coreisr EOI");
    check(latm_uncore_iocsr_read(&u, 0x14c0, 4), 0x00000001,
          "IOCSR ipmap readback");
    /* 0x420 is beyond the eight per-CPU COREISR registers. */
    latm_uncore_iocsr_write(&u, 0x1820, 0xffffffffu, 4);
    check(latm_uncore_iocsr_read(&u, 0x1820, 4), 0,
          "IOCSR coreisr out-of-bank ignored");

    printf("latm-uncore: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
