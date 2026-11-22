#include "lata-vcache.h"

#include <stdio.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

int main(void)
{
    uint32_t violations;

    /* The window is on by default and can be switched off. */
    CHECK(lata_vcache_enabled());
    lata_vcache_set_enabled(false);
    CHECK(!lata_vcache_enabled());
    lata_vcache_set_enabled(true);
    CHECK(lata_vcache_enabled());

    /* Fresh state: nothing borrowed, no traffic counted. */
    CHECK(lata_vcache_clean());
    CHECK(lata_vcache_dirty_mask() == 0);
    CHECK(lata_vcache_vector_reads() == 0);
    CHECK(lata_vcache_vector_writes() == 0);
    violations = lata_vcache_invariant_violations();

    /* One TB worth of traffic is counted and reset on the next begin. */
    lata_vcache_begin_tb();
    lata_vcache_note_env_vector_read();
    lata_vcache_note_env_vector_read();
    lata_vcache_note_env_vector_read();
    lata_vcache_note_env_vector_write();
    lata_vcache_note_env_vector_write();
    lata_vcache_note_env_scalar_read();
    lata_vcache_note_env_scalar_write();
    CHECK(lata_vcache_vector_reads() == 3);
    CHECK(lata_vcache_vector_writes() == 2);
    CHECK(lata_vcache_scalar_reads() == 1);
    CHECK(lata_vcache_scalar_writes() == 1);
    CHECK(lata_vcache_clean());
    lata_vcache_end_tb();
    CHECK(lata_vcache_clean());
    CHECK(lata_vcache_invariant_violations() == violations);

    /* The counters belong to the TB that produced them. */
    lata_vcache_begin_tb();
    CHECK(lata_vcache_vector_reads() == 0);
    CHECK(lata_vcache_vector_writes() == 0);
    CHECK(lata_vcache_scalar_reads() == 0);
    CHECK(lata_vcache_scalar_writes() == 0);
    lata_vcache_end_tb();

    /* A borrowed register that survives the TB is an I0 violation. */
    lata_vcache_begin_tb();
    lata_vcache_mark_dirty(5);
    CHECK(!lata_vcache_clean());
    CHECK(lata_vcache_dirty_mask() == (UINT64_C(1) << 5));
    lata_vcache_clear_dirty(5);
    CHECK(lata_vcache_clean());
    lata_vcache_mark_dirty(7);
    lata_vcache_end_tb();
    CHECK(lata_vcache_clean());
    CHECK(lata_vcache_invariant_violations() == violations + 1);
    violations = lata_vcache_invariant_violations();

    /* Entering a TB with a leak is reported once, not repeatedly. */
    lata_vcache_mark_dirty(9);
    lata_vcache_begin_tb();
    CHECK(lata_vcache_invariant_violations() == violations + 1);
    lata_vcache_end_tb();
    CHECK(lata_vcache_clean());

    /* Out-of-range indices must not touch the mask. */
    lata_vcache_begin_tb();
    lata_vcache_mark_dirty(64);
    lata_vcache_mark_dirty(1000);
    CHECK(lata_vcache_dirty_mask() == 0);
    lata_vcache_end_tb();

    if (failures != 0) {
        fprintf(stderr, "vcache: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    printf("vcache: %u checks passed\n", checks);
    return 0;
}
