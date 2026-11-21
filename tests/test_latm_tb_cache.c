#include <stdio.h>
#include <string.h>
#include "loongemu/tb_cache.h"

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
    TbCacheEntry entries[8];
    uint32_t buckets[8];
    TbCache cache;

    check(tbc_init(&cache, entries, buckets, 8), 1, "init");
    TbCacheEntry *a = tbc_insert_mips(&cache, 0xffffffff80002000ull,
                                      0x2, 0x11);
    check(a != NULL, 1, "insert MIPS");
    check(tbc_lookup_mips(&cache, 0xffffffff80002000ull, 0x2, 0x11) == a,
          1, "lookup same ASID/mode");
    check(tbc_lookup_mips(&cache, 0xffffffff80002000ull, 0x3, 0x11) == NULL,
          1, "different ASID misses");
    check(tbc_lookup_mips(&cache, 0xffffffff80002000ull, 0x2, 0x01) == NULL,
          1, "different hflags misses");
    a->guest_size = 8;
    tbc_invalidate_range(&cache, 0x2, 0xffffffff80002000ull, 4);
    check(tbc_lookup_mips(&cache, 0xffffffff80002000ull, 0x2, 0x11) == NULL,
          1, "MIPS page invalidation");
    check(tbc_count(&cache), 0, "invalidation count");

    printf("latm-tb-cache: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
