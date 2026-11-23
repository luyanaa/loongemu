/*
 * Helper-descriptor lifetime test.
 *
 * Every SVE fallback helper site embeds its LataSVEHelperDesc address as an
 * immediate in generated code, so a descriptor must live exactly as long as
 * the code that references it: freed too early is host memory corruption,
 * kept forever is an unbounded leak on a long-running guest. The lifetime is
 * therefore managed in groups keyed by TranslationBlock:
 *
 *  - announcing a block (begin_tb) only switches the active group; other
 *    groups must survive because generated blocks coexist (TU emits a chain
 *    before running any of it);
 *  - release_tb drops one block's group;
 *  - release_all drops everything, which is only legal at a code-cache flush
 *    or context teardown;
 *  - re-announcing the same block reuses its group's partial chunk instead of
 *    growing a new one.
 *
 * These tests use hand-built DisasContext fixtures (base == NULL gives the
 * fixture owner), the same path the allocator provides to callers that never
 * announce a block.
 */
#include "sve-lasx-lower.h"
#include "target/arm/lata/include/lata.h"
#include "target/arm/tcg/translate.h"

#include <stdio.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

/* Standalone context, same pattern as test_lata_context.c. */
static LATA_TLS LataContext tls_ctx;

LATA_TLS ENV *lsenv = NULL;
FILE *stderr = NULL;

void latx_bare_assert_fail(const char *expr, const char *file, int line)
{
    printf("ASSERT FAIL %s:%d: %s\n", file, line, expr);
    abort();
}

LataContext *lata_context_current(void)
{
    return &tls_ctx;
}

/* sve-helper.c references these only from execution paths this test avoids. */
void helper_exception_with_syndrome(CPUARMState *env, uint32_t excp,
                                    uint32_t syndrome)
{
    (void)env; (void)excp; (void)syndrome;
}

static PvirInstruction make_pvir(void)
{
    PvirInstruction insn;

    memset(&insn, 0, sizeof(insn));
    insn.opcode = PVIR_OP_LOAD;
    insn.operation.vector.operand_count = 1;
    return insn;
}

static void make_ctx(DisasContext *s, DisasContextBase *base,
                     TranslationBlock *tb, uint64_t pc_curr)
{
    memset(s, 0, sizeof(*s));
    memset(base, 0, sizeof(*base));
    base->tb = tb;
    base->pc_first = pc_curr;
    base->pc_next = pc_curr + 4;
    s->base = base;
    s->pc_curr = pc_curr;
    s->vl = 64;
    s->tbid = 1;
}

int main(void)
{
    uint32_t descs;
    uint32_t chunks;
    PvirInstruction insn = make_pvir();
    static uint8_t tb_a_mem[512];
    static uint8_t tb_b_mem[512];
    TranslationBlock *tb_a = (TranslationBlock *)tb_a_mem;
    TranslationBlock *tb_b = (TranslationBlock *)tb_b_mem;
    DisasContext s;
    DisasContextBase base;
    const LataSVEHelperDesc *d1;
    const LataSVEHelperDesc *d2;
    unsigned i;

    memset(&tls_ctx, 0, sizeof(tls_ctx));

    /* One block, more helper sites than a chunk holds: chunks accumulate. */
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 0 && chunks == 0);
    make_ctx(&s, &base, tb_a, 0x1000);
    for (i = 0; i < 20; ++i) {
        d1 = lata_sve_alloc_helper_desc(&s, &insn, NULL);
        CHECK(d1 != NULL);
    }
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 20);
    CHECK(chunks == 3); /* 8 descriptors per chunk */

    /* Announcing a different block must not drop anything: both blocks' code
     * can be live (TU generation emits several blocks before running them). */
    make_ctx(&s, &base, tb_b, 0x2000);
    d1 = lata_sve_alloc_helper_desc(&s, &insn, NULL);
    CHECK(d1 != NULL);
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 21);
    CHECK(chunks == 4);

    /* Same block re-announced: the group switch happens, nothing is freed, and
     * a partial chunk of the same group is reused rather than reallocated. */
    make_ctx(&s, &base, tb_b, 0x2000);
    d1 = lata_sve_alloc_helper_desc(&s, &insn, NULL);
    CHECK(d1 != NULL);
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 22);
    CHECK(chunks == 4);

    /* release_tb drops exactly one block's group. */
    lata_sve_helper_release_tb((const void *)tb_a);
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 2);
    CHECK(chunks == 1);

    /* Descriptor contents must survive: the generated code dereferences them
     * without re-reading anything else. */
    CHECK(d1->pvir.opcode == PVIR_OP_LOAD);
    CHECK(d1->pvir.operation.vector.operand_count == 1);
    CHECK(d1->vl_bytes == 64);
    CHECK(d1->pc_curr == 0x2000);
    CHECK(d1->pc_next == 0x2004);

    /* release_all drops everything and resets the group state, so a fresh
     * start after a flush allocates from zero. */
    lata_sve_helper_release_all();
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 0 && chunks == 0);
    make_ctx(&s, &base, tb_b, 0x2000);
    d1 = lata_sve_alloc_helper_desc(&s, &insn, NULL);
    CHECK(d1 != NULL);
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 1 && chunks == 1);

    /* Fixtures without a DisasContextBase still work and group by PC. */
    memset(&s, 0, sizeof(s));
    s.pc_curr = 0x3000;
    s.vl = 64;
    d1 = lata_sve_alloc_helper_desc(&s, &insn, NULL);
    d2 = lata_sve_alloc_helper_desc(&s, &insn, NULL);
    CHECK(d1 != NULL && d2 != NULL && d1 != d2);
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 3);
    s.pc_curr = 0x4000;
    (void)lata_sve_alloc_helper_desc(&s, &insn, NULL);
    lata_sve_helper_stats(&descs, &chunks);
    CHECK(descs == 4);
    lata_sve_helper_release_all();

    /* NULL instruction must be refused, not crash. */
    CHECK(lata_sve_alloc_helper_desc(&s, NULL, NULL) == NULL);

    printf("SVE helper lifecycle: %u checks passed", checks);
    if (failures != 0) {
        printf(", %u FAILED\n", failures);
        return 1;
    }
    printf("\n");
    return 0;
}
