#ifndef LATX_BARE_ASSERT_H
#define LATX_BARE_ASSERT_H
void latx_bare_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) do { if (!(x)) latx_bare_assert_fail(#x, __FILE__, __LINE__); } while (0)
#endif
#endif
