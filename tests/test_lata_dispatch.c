/*
 * Host test for the LATA dispatch loop. The translate/enter ops are faked:
 * translate "emits" a 4-byte sentinel and records it ran; enter walks a
 * scripted PC sequence. The assertions are about cache behaviour, not the
 * fake host code: repeated PCs must not re-translate.
 */
#include "loongemu/lata_dispatch.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fails++; \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
    } while (0)

typedef struct FakeCpu {
    /* Scripted guest-PC stream the TBs exit to. */
    const uint64_t *pc_stream;
    int stream_len;
    int stream_pos;
    uint64_t pc;
    /* Counts of how often each distinct pc got translated/entered. */
    uint64_t asid;
    uint8_t el;
    /* After this many enters, change the key's current memory regime. */
    int asid_flip_at;
    uint64_t asid_new;
    int mapping_epoch_flip_at;
    uint64_t mapping_epoch;
    uint64_t mapping_epoch_new;
    int translate_calls;
    int enter_calls;
    size_t last_capacity;
    size_t emit_bytes;
    int lie_about_size;
    int sync_calls;
    uint64_t translated_pcs[64];
    int translated_n;
} FakeCpu;

static int fake_translate(void *o, uint64_t pc, void *code_addr,
                          size_t capacity, TbEmitResult *result)
{
    FakeCpu *c = o;
    size_t want = c->emit_bytes != 0 ? c->emit_bytes : 16;

    c->translate_calls++;
    c->last_capacity = capacity;
    if (c->translated_n < (int)(sizeof(c->translated_pcs) /
                              sizeof(c->translated_pcs[0]))) {
        c->translated_pcs[c->translated_n++] = pc;
    }
    if (want > capacity) {
        /* A real translator must not write past the reservation. The loop is
         * also required to catch a translator that claims more than it had. */
        if (c->lie_about_size) {
            result->code_size = (uint32_t)want;
            return 0;
        }
        return -1;
    }
    memset(code_addr, 0x90, want);
    result->code_size = (uint32_t)want;
    result->guest_size = 4;
    result->recovery_count = 1;
    return 0;
}

static void fake_sync(void *o, void *code_addr, size_t len)
{
    FakeCpu *c = o;

    (void)code_addr;
    (void)len;
    c->sync_calls++;
}

static void fake_enter(void *o, uint64_t pc, void *code_addr)
{
    FakeCpu *c = o;
    (void)code_addr;
    c->enter_calls++;
    c->pc = pc;
    /* Advance along the scripted control-flow stream. */
    if (c->stream_pos < c->stream_len) {
        c->pc = c->pc_stream[c->stream_pos++];
    } else {
        c->pc = UINT64_C(0); /* falls off; halt() catches it */
    }
    if (c->asid_flip_at != 0 && c->enter_calls == c->asid_flip_at) {
        c->asid = c->asid_new;
    }
    if (c->mapping_epoch_flip_at != 0 &&
        c->enter_calls == c->mapping_epoch_flip_at) {
        c->mapping_epoch = c->mapping_epoch_new;
    }
}

static uint64_t fake_next_pc(void *o) { return ((FakeCpu *)o)->pc; }
static uint64_t fake_asid(void *o) { return ((FakeCpu *)o)->asid; }
static uint64_t fake_mapping_epoch(void *o)
{
    return ((FakeCpu *)o)->mapping_epoch;
}
static uint8_t fake_el(void *o) { return ((FakeCpu *)o)->el; }
static bool fake_halt(void *o)
{
    FakeCpu *c = o;
    return c->pc == UINT64_C(0) || c->enter_calls > 1000;
}

static const LataDispatchOps OPS = {
    .translate = fake_translate,
    .sync_icache = fake_sync,
    .enter = fake_enter,
    .next_pc = fake_next_pc,
    .address_space = fake_asid,
    .mapping_epoch = fake_mapping_epoch,
    .exception_level = fake_el,
    .halt = fake_halt,
};

#define CAP 8
static TbCacheEntry g_entries[CAP];
static uint32_t g_buckets[CAP];
static uint8_t g_arena[4096];

static void zero_storage(void)
{
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_buckets, 0, sizeof(g_buckets));
    memset(g_arena, 0, sizeof(g_arena));
}

/* Program visits PCs [A,B,A,C,B,A] then exits to 0. Unique TBs = 3; total
 * executions = 6. A working cache translates each of A,B,C exactly once. */
static void test_revisits_translate_once(void)
{
    zero_storage();
    static const uint64_t A = 0x1000, B = 0x2000, C = 0x3000;
    const uint64_t stream[] = { B, A, C, B, A, 0 };
    FakeCpu c = {0};
    c.pc_stream = stream;
    c.stream_len = (int)(sizeof(stream) / sizeof(stream[0]));
    c.pc = A;           /* entry PC */
    c.asid = 0x11;
    c.el = 1;

    LataDispatch d;
    CHECK(lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, CAP,
                             g_arena, sizeof(g_arena)),
          "dispatch_init");
    uint64_t ran = lata_dispatch_run(&d);
    CHECK(ran == 6, "ran all 6 TB executions");
    CHECK(d.executed == 6, "executed counter");
    CHECK(d.translated == 3, "each unique pc translated once");
    CHECK(d.cache_hits == 3, "3 revisits served from cache");
    CHECK(c.translate_calls == 3, "translate called exactly per-unique-pc");
    CHECK(c.enter_calls == 6, "enter called per execution");
}

/* Same pc under a different address space must not hit the cached TB. One
 * shared dispatch: pc A runs under asid=1, then asid flips to 2 while pc
 * stays A — the asid=2 lookup must miss and re-translate, not serve the
 * asid=1 entry. Stream: pc=A enters→pc=A, flip to asid=2; second dispatch of
 * A is under asid=2 (miss→translate), then pc=0 halts. So 2 executes, 2
 * translates, 0 hits. */
static void test_asid_isolation(void)
{
    zero_storage();
    static const uint64_t A = 0x1000;
    const uint64_t stream[] = { A, 0 };
    FakeCpu c = {0};
    c.pc_stream = stream;
    c.stream_len = (int)(sizeof(stream) / sizeof(stream[0]));
    c.pc = A;
    c.asid = 0x1;
    c.el = 1;
    c.asid_flip_at = 1;     /* after first enter, address space -> 2 */
    c.asid_new = 0x2;

    LataDispatch d;
    CHECK(lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, CAP,
                             g_arena, sizeof(g_arena)),
          "asid dispatch_init");
    lata_dispatch_run(&d);
    CHECK(d.executed == 2, "asid runs A twice");
    CHECK(d.translated == 2, "asid change forces re-translation");
    CHECK(d.cache_hits == 0, "no cross-asid cache hit");
}

/* A remap at the same PC/TTBR must miss the old TB even before per-span
 * admission is reached. */
static void test_mapping_epoch_isolation(void)
{
    zero_storage();
    static const uint64_t A = 0x1000;
    const uint64_t stream[] = { A, 0 };
    FakeCpu c = {0};
    c.pc_stream = stream;
    c.stream_len = (int)(sizeof(stream) / sizeof(stream[0]));
    c.pc = A;
    c.asid = 0x11;
    c.el = 1;
    c.mapping_epoch = 7;
    c.mapping_epoch_flip_at = 1;
    c.mapping_epoch_new = 8;

    LataDispatch d;
    CHECK(lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, CAP,
                             g_arena, sizeof(g_arena)),
          "mapping-epoch dispatch_init");
    lata_dispatch_run(&d);
    CHECK(d.executed == 2, "same PC executes under both mapping epochs");
    CHECK(d.translated == 2, "mapping epoch change forces re-translation");
    CHECK(d.cache_hits == 0, "stale mapping-epoch TB is not reused");
}

/* init rejects non-power-of-two capacity and NULL ops. */
static void test_bad_geometry(void)
{
    LataDispatch d;
    FakeCpu c = {0};
    LataDispatchOps no_epoch_ops = OPS;
    CHECK(!lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, 3,
                              g_arena, sizeof(g_arena)),
          "rejects capacity 3");
    CHECK(!lata_dispatch_init(&d, NULL, &c, g_entries, g_buckets, CAP,
                              g_arena, sizeof(g_arena)),
          "rejects null ops");
    CHECK(!lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, CAP,
                              NULL, 0),
          "rejects null arena");
    no_epoch_ops.mapping_epoch = NULL;
    CHECK(!lata_dispatch_init(&d, &no_epoch_ops, &c, g_entries, g_buckets,
                              CAP, g_arena, sizeof(g_arena)),
          "rejects missing mapping-epoch callback");
}

/* The translator sees exactly the bytes left, and a TB that would not fit is
 * refused instead of being written past the reservation. */
static void test_capacity_is_respected(void)
{
    static const uint64_t stream[] = { 0x5000, 0x6000, 0 };
    FakeCpu c = {0};
    LataDispatch d;

    zero_storage();
    c.pc_stream = stream;
    c.stream_len = 2;
    c.pc = 0x4000;
    /* Arena of 200 bytes, 16-byte TBs: the third must see less than 64 left. */
    CHECK(lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, CAP,
                             g_arena, 200),
          "capacity init");
    lata_dispatch_run(&d);
    CHECK(c.sync_calls == c.translate_calls,
          "dispatcher syncs I-cache once per translation");
    CHECK(c.last_capacity <= 200, "capacity never exceeds the arena");

    /* A translator that lies about its size must be refused, not cached. */
    {
        static const uint64_t s2[] = { 0 };
        FakeCpu c2 = {0};
        LataDispatch d2;

        zero_storage();
        c2.pc_stream = s2;
        c2.stream_len = 0;
        c2.pc = 0x7000;
        c2.emit_bytes = 400;   /* larger than the arena passed below */
        c2.lie_about_size = 1;
        CHECK(lata_dispatch_init(&d2, &OPS, &c2, g_entries, g_buckets, CAP,
                                 g_arena, 200),
              "lying-translator init");
        lata_dispatch_run(&d2);
        CHECK(d2.refused == 1, "oversized translation is refused");
        CHECK(d2.translated == 0, "refused TB is not counted as translated");
    }
}

/* Arena exhaustion reclaims the cache and continues; a TB bigger than the whole
 * arena stops the loop instead of spinning. */
static void test_arena_reclaim(void)
{
    static const uint64_t stream[] = { 2, 3, 4, 5, 6, 0 };
    FakeCpu c = {0};
    LataDispatch d;

    zero_storage();
    c.pc_stream = stream;
    c.stream_len = 5;
    c.pc = 1;
    CHECK(lata_dispatch_init(&d, &OPS, &c, g_entries, g_buckets, CAP,
                             g_arena, 48),   /* room for three 16-byte TBs */
          "reclaim init");
    lata_dispatch_run(&d);
    CHECK(d.executed == 6, "all scripted TBs execute across a reclaim");
    CHECK(d.reclaims > 0, "arena was reclaimed instead of stopping");
    CHECK(d.arena_used <= 48, "arena stays inside its bounds");

    {
        static const uint64_t s2[] = { 0 };
        FakeCpu c2 = {0};
        LataDispatch d2;

        zero_storage();
        c2.pc_stream = s2;
        c2.stream_len = 0;
        c2.pc = 9;
        c2.emit_bytes = 4096;   /* cannot ever fit in 64 bytes */
        CHECK(lata_dispatch_init(&d2, &OPS, &c2, g_entries, g_buckets, CAP,
                                 g_arena, 64),
              "oversized-single-TB init");
        lata_dispatch_run(&d2);
        CHECK(d2.executed == 0, "a TB larger than the arena executes nothing");
        CHECK(d2.reclaims == 0, "an empty arena cannot be reclaimed");
    }
}

int main(void)
{
    test_revisits_translate_once();
    test_asid_isolation();
    test_mapping_epoch_isolation();
    test_bad_geometry();
    test_capacity_is_respected();
    test_arena_reclaim();
    if (fails == 0) {
        printf("lata-dispatch: all checks passed\n");
        return 0;
    }
    printf("lata-dispatch: %d failures\n", fails);
    return 1;
}
