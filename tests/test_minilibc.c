/* Behaviour parity for the in-tree minimal libc: every routine must match the
 * host libc on the cases the translator actually exercises. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void  *latx_memcpy(void *, const void *, size_t);
void  *latx_memmove(void *, const void *, size_t);
void  *latx_memset(void *, int, size_t);
int    latx_memcmp(const void *, const void *, size_t);
void  *latx_memchr(const void *, int, size_t);
size_t latx_strlen(const char *);
int    latx_strcmp(const char *, const char *);
int    latx_strncmp(const char *, const char *, size_t);
char  *latx_strcpy(char *, const char *);
char  *latx_strncpy(char *, const char *, size_t);
char  *latx_strcat(char *, const char *);
char  *latx_strchr(const char *, int);
char  *latx_strrchr(const char *, int);
char  *latx_strdup(const char *);
int    latx_snprintf(char *, size_t, const char *, ...);
int    latx_sprintf(char *, const char *, ...);
void  *latx_malloc(size_t);
void  *latx_calloc(size_t, size_t);
void  *latx_realloc(void *, size_t);
void   latx_free(void *);

static int checks;
static int fails;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            fails++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);       \
        }                                                               \
    } while (0)

static void test_memory(void)
{
    unsigned char a[32], b[32], c[32];
    size_t i;

    for (i = 0; i < sizeof(a); i++) {
        a[i] = (unsigned char)(0x40 + i);
        b[i] = 0;
    }
    CHECK(latx_memcpy(b, a, sizeof(a)) == b);
    CHECK(memcmp(a, b, sizeof(a)) == 0);

    CHECK(latx_memcpy(b, a, 8) == b);
    CHECK(memcmp(a, b, 8) == 0);

    CHECK(latx_memset(c, 0xA5, 17) == c);
    for (i = 0; i < 17; i++) {
        CHECK(c[i] == 0xA5);
    }

    memcpy(c, "0123456789", 11);
    CHECK(latx_memmove(c + 2, c, 8) == c + 2);
    CHECK(memcmp(c, "0101234567", 11) == 0);

    CHECK(latx_memcmp("ab", "ab", 2) == 0);
    CHECK(latx_memcmp("ab", "ac", 2) < 0);
    CHECK(latx_memcmp("ac", "ab", 2) > 0);
    CHECK(latx_memchr("hello", 'l', 5) != 0);
    CHECK(latx_memchr("hello", 'z', 5) == 0);
}

static void test_strings(void)
{
    char buf[32];
    char small[4];

    CHECK(latx_strlen("") == 0);
    CHECK(latx_strlen("abcd") == 4);
    CHECK(latx_strcmp("a", "a") == 0);
    CHECK(latx_strcmp("a", "b") < 0);
    CHECK(latx_strcmp("b", "a") > 0);
    CHECK(latx_strncmp("abcX", "abcY", 3) == 0);
    CHECK(latx_strncmp("abcX", "abcY", 4) < 0);

    CHECK(strcmp(latx_strcpy(buf, "hello"), "hello") == 0);
    CHECK(strcmp(latx_strcat(buf, "!"), "hello!") == 0);

    memset(small, 0x5a, sizeof(small));
    latx_strncpy(small, "abcdef", 3);
    CHECK(small[0] == 'a' && small[1] == 'b' && small[2] == 'c');
    memset(small, 0x5a, sizeof(small));
    latx_strncpy(small, "a", sizeof(small));
    CHECK(small[0] == 'a' && small[1] == '\0' && small[2] == '\0');

    CHECK(latx_strchr("hello", 'l') != 0);
    CHECK(strcmp(latx_strchr("hello", 'l'), "llo") == 0);
    CHECK(strcmp(latx_strrchr("hello", 'l'), "lo") == 0);
    CHECK(latx_strchr("hello", 'z') == 0);

    {
        char *d = latx_strdup("copy");
        CHECK(d != 0 && strcmp(d, "copy") == 0);
        latx_free(d);
    }
}

static void test_format(void)
{
    char ours[128], ref[128];
    int n_ours, n_ref;

#define CMP(fmt, ...)                                                   \
    do {                                                                \
        memset(ours, '?', sizeof(ours));                                \
        memset(ref, '?', sizeof(ref));                                  \
        n_ours = latx_snprintf(ours, sizeof(ours), fmt, __VA_ARGS__);   \
        n_ref = snprintf(ref, sizeof(ref), fmt, __VA_ARGS__);           \
        CHECK(n_ours == n_ref);                                         \
        CHECK(strcmp(ours, ref) == 0);                                  \
    } while (0)

    CMP("%d", -1234);
    CMP("%u", 4000000000u);
    CMP("%x", 0xdeadbeefu);
    CMP("%016llx", (unsigned long long)0x0123456789abcdefull);
    CMP("%s", "text");
    CMP("%c%c", 'o', 'k');
    CMP("a%db%sc", 7, "XY");
    CMP("%08x", 0x1234u);
    CMP("%5d", 42);
    CMP("%p", (void *)0x1234);
    /* ir2_dump renders mnemonics with a left justified field */
    CMP("%-8s|", "abc");
    CMP("%-6d|", 42);
    CMP("%-6d|", -42);
    CMP("%-7x|", 0x12u);
#undef CMP

    CHECK(latx_snprintf(ours, 4, "abcdef") == 6);
    CHECK(strcmp(ours, "abc") == 0);
    {
        char s2[16];
        CHECK(latx_sprintf(s2, "%d-%s", 5, "z") == 3);
        CHECK(strcmp(s2, "5-z") == 0);
    }
}

static void test_alloc(void)
{
    unsigned char *p = latx_malloc(16);
    unsigned char *q;
    size_t i;

    CHECK(p != 0);
    for (i = 0; i < 16; i++) {
        p[i] = (unsigned char)i;
    }
    q = latx_realloc(p, 32);
    CHECK(q != 0);
    for (i = 0; i < 16; i++) {
        CHECK(q[i] == (unsigned char)i);
    }
    {
        unsigned char *z = latx_calloc(8, 4);
        CHECK(z != 0);
        for (i = 0; i < 32; i++) {
            CHECK(z[i] == 0);
        }
        latx_free(z);
    }
    latx_free(q);
}

int main(void)
{
    test_memory();
    test_strings();
    test_format();
    test_alloc();
    printf("minilibc parity: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
