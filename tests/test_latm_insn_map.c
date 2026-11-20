#include <stdio.h>
#include <string.h>
#include "latm.h"

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
    TRANSLATION_DATA td;
    uint64_t guest;
    uint8_t delay;
    const uintptr_t base = 0x1000;

    memset(&td, 0, sizeof(td));
    latm_tb_insn_map_begin(&td, (const void *)base, 64);
    latm_tb_insn_map_add(&td, 0x80000000u, 0, 16, 0);
    latm_tb_insn_map_add(&td, 0x80000004u, 4, 8, 1);

    guest = 0;
    delay = 0;
    check(latm_tb_insn_map_resolve(&td, base + 6, &guest, &delay), 1,
          "resolve inside delay");
    check(guest, 0x80000004u, "delay guest PC");
    check(delay, 1, "delay flag");
    check(latm_tb_insn_map_resolve(&td, base + 12, &guest, &delay), 1,
          "resolve inside instruction");
    check(guest, 0x80000000u, "instruction guest PC");
    check(delay, 0, "non-delay flag");
    check(latm_tb_insn_map_resolve(&td, base - 4, &guest, &delay), 0,
          "resolve below base");
    check(latm_tb_insn_map_resolve(&td, base + 32, &guest, &delay), 0,
          "resolve outside spans");
    check(latm_tb_insn_map_resolve(NULL, base, &guest, &delay), 0,
          "resolve null map");

    printf("latm-insn-map: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
