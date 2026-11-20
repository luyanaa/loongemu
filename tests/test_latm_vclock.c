#include <stdio.h>
#include "loongemu/vclock.h"

static int checks, fails;
static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s got=%llu want=%llu\n", what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

int main(void)
{
    LoongemuVClock vc;
    loongemu_vclock_init(&vc, LOONGEMU_VCLOCK_VIRTUAL_STEP, 100000000, 0, 0,
                         0, 0, 0);
    loongemu_vclock_advance_ns(&vc, 1000000000ull);
    check(loongemu_vclock_read_mips_count(&vc, 50000000ull), 50000000,
          "MIPS Count 50 MHz");
    loongemu_vclock_advance_ns(&vc, 20000000ull);
    check(loongemu_vclock_read_mips_count(&vc, 50000000ull), 51000000,
          "MIPS Count advances");
    printf("latm-vclock: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
