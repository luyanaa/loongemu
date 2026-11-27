/*
 * Self-contained replacements for the small GLib surface the retained LATX
 * sources use.  Bare metal has no GLib; only six entry points are required:
 *   g_malloc / g_malloc0 / g_free, g_private_set / g_private_get,
 *   g_assert_not_reached.
 *
 * Allocation comes from a static arena: the translator allocates its IR1
 * decode templates once, and frees them only at thread teardown, so a
 * bump allocator with a no-op free keeps the library free of a host heap.
 * A platform that does own a heap can override latx_bare_arena_base().
 */
#include "glib.h"
#include "latx_bare.h"

/* Weak console hooks: a real platform overrides them; the defaults keep the
 * archive link-clean so the library never leaks an unresolved symbol. */
__attribute__((weak)) void latx_bare_uart_putc(int c)
{
    (void)c;
}

__attribute__((weak)) void latx_bare_uart_puts(const char *s)
{
    (void)s;
}

#ifndef LATX_BARE_ARENA_SIZE
/*
 * 4 MiB was too small: translating a block with a call needs a single ~8.9 MiB
 * allocation, so translation aborted with an assert that looked like
 * exhaustion but was one oversized request.  32 MiB covers it with headroom.
 */
#define LATX_BARE_ARENA_SIZE (32u * 1024u * 1024u)
#endif

static unsigned char g_arena[LATX_BARE_ARENA_SIZE] __attribute__((aligned(16)));

int qemu_loglevel;
static size_t g_arena_used;

size_t latx_bare_arena_used(void)
{
    return g_arena_used;
}

size_t latx_bare_arena_size(void)
{
    return LATX_BARE_ARENA_SIZE;
}

void *latx_bare_arena_base(void)
{
    return g_arena;
}

/* Each block carries its payload size so a later realloc can copy exactly the
 * bytes that were handed out.  free() is intentionally a no-op: the arena is
 * reclaimed wholesale when the translation context is torn down. */
typedef struct ArenaHdr {
    size_t size;
    size_t pad;
} ArenaHdr;

static gpointer arena_alloc(size_t n)
{
    size_t payload = (n + 15u) & ~(size_t)15u;
    ArenaHdr *h;

    if (payload < n || g_arena_used + sizeof(ArenaHdr) + payload > LATX_BARE_ARENA_SIZE) {
        /* Name the request and the remaining room: "assert" alone cannot tell a
         * single oversized allocation from a long run that finally exhausted
         * the arena, and those need opposite fixes. */
        latx_bare_uart_puts("latx: malloc exhausted");
        {
            static const char hex[] = "0123456789abcdef";
            const size_t vals[3] = { payload, g_arena_used,
                                     (size_t)LATX_BARE_ARENA_SIZE };
            static const char *const labels[3] = { " want=", " used=", " cap=" };

            for (int v = 0; v < 3; v++) {
                latx_bare_uart_puts(labels[v]);
                for (int i = (int)(sizeof(size_t) * 8) - 4; i >= 0; i -= 4) {
                    latx_bare_uart_putc(hex[(vals[v] >> i) & 15]);
                }
            }
        }
        latx_bare_uart_puts("\n");
        g_assert_not_reached();
    }
    h = (ArenaHdr *)(void *)(g_arena + g_arena_used);
    h->size = n;
    g_arena_used += sizeof(ArenaHdr) + payload;
    return (gpointer)(h + 1);
}

static size_t arena_block_size(gpointer mem)
{
    return mem != 0 ? ((ArenaHdr *)mem - 1)->size : 0;
}

void *malloc(size_t n)
{
    return arena_alloc(n);
}

void *calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    unsigned char *p;

    if (sz != 0 && total / sz != n) {
        g_assert_not_reached();
    }
    p = (unsigned char *)arena_alloc(total);
    for (size_t i = 0; i < total; i++) {
        p[i] = 0;
    }
    return p;
}

void *realloc(void *mem, size_t n)
{
    size_t old = arena_block_size(mem);
    unsigned char *p = (unsigned char *)arena_alloc(n);
    size_t copy = old < n ? old : n;

    for (size_t i = 0; i < copy; i++) {
        p[i] = ((unsigned char *)mem)[i];
    }
    return p;
}

void free(void *mem)
{
    (void)mem;
}

gpointer g_malloc(size_t n)
{
    return arena_alloc(n);
}

gpointer g_malloc0(size_t n)
{
    unsigned char *p = (unsigned char *)arena_alloc(n);
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
    return p;
}

gpointer g_realloc(gpointer mem, size_t n)
{
    /* The arena cannot grow blocks: callers in this tree only shrink or
     * allocate fresh blocks, so hand out a new block and leak the old one. */
    (void)mem;
    return arena_alloc(n);
}

void g_free(gpointer mem)
{
    (void)mem;
}

void g_private_set(GPrivate *key, gpointer value)
{
    key->p = value;
}

gpointer g_private_get(GPrivate *key)
{
    return key->p;
}

void g_assert_not_reached(void)
{
    static const char msg[] = "latx: g_assert_not_reached\n";

    latx_bare_uart_puts(msg);
    for (;;) {
    }
}
