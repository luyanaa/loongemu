/*
 * In-tree minimal libc for the bare-metal LATX core.
 *
 * Scope is exactly what the retained sources reference (verified against the
 * archive's symbol table): memory/string primitives, a printf subset, and the
 * logging/assert entry points.  No locale, no floating point formatting, no
 * buffered streams.  Every routine is written here so the archive never needs
 * a host libc.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "latx_bare.h"
#include "qemu/log.h"

/* ---------- memory ---------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;

    while (n--) {
        if (*p == (unsigned char)c) {
            return (void *)p;
        }
        p++;
    }
    return 0;
}

/* ---------- strings ---------- */

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != '\0') {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;

    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);

    while ((*d++ = *src++) != '\0') {
    }
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;;) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (*s == '\0') {
            return 0;
        }
        s++;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;

    for (;;) {
        if (*s == (char)c) {
            last = s;
        }
        if (*s == '\0') {
            return (char *)last;
        }
        s++;
    }
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);

    if (p != 0) {
        memcpy(p, s, n);
    }
    return p;
}

/* ---------- formatted output ---------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} Out;

static void out_ch(Out *o, char c)
{
    if (o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_str(Out *o, const char *s)
{
    while (*s) {
        out_ch(o, *s++);
    }
}

static void out_pad(Out *o, char c, int n)
{
    while (n-- > 0) {
        out_ch(o, c);
    }
}

static void out_unsigned(Out *o, unsigned long long v, unsigned base, int upper,
                         int width, char pad)
{
    char tmp[24];
    int n = 0;

    do {
        unsigned d = (unsigned)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + (d - 10));
        v /= base;
    } while (v != 0);
    out_pad(o, pad, width - n);
    while (n--) {
        out_ch(o, tmp[n]);
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    Out o = { buf, cap, 0 };

    while (*fmt) {
        int width = 0;
        char pad = ' ';
        int lng = 0;
        char spec;

        if (*fmt != '%') {
            out_ch(&o, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out_ch(&o, '%');
            fmt++;
            continue;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        if (*fmt == 'z') {
            lng = 2;
            fmt++;
        }
        spec = *fmt ? *fmt++ : '\0';
        switch (spec) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            out_str(&o, s ? s : "(null)");
            break;
        }
        case 'c':
            out_ch(&o, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            long long v;
            unsigned long long uv;
            int neg;

            v = (lng >= 2) ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            neg = v < 0;
            uv = neg ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;
            if (neg) {
                out_ch(&o, '-');
            }
            out_unsigned(&o, uv, 10, 0, width - (neg ? 1 : 0), pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
                                               : (unsigned long long)va_arg(ap, unsigned);
            unsigned base = (spec == 'u') ? 10u : 16u;
            out_unsigned(&o, v, base, spec == 'X', width, pad);
            break;
        }
        case 'p':
            out_str(&o, "0x");
            out_unsigned(&o, (unsigned long long)(uintptr_t)va_arg(ap, void *),
                         16, 0, width, pad);
            break;
        case '\0':
            out_ch(&o, '%');
            break;
        default:
            out_ch(&o, '%');
            out_ch(&o, spec);
            break;
        }
    }
    if (o.cap != 0) {
        size_t end = (o.len < o.cap) ? o.len : (o.cap - 1);
        o.buf[end] = '\0';
    }
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

static int uart_vprintf(const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    latx_bare_uart_puts(buf);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = uart_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    (void)f;
    va_start(ap, fmt);
    n = uart_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fputs(const char *s, FILE *f)
{
    (void)f;
    latx_bare_uart_puts(s);
    return 0;
}

int puts(const char *s)
{
    latx_bare_uart_puts(s);
    latx_bare_uart_putc('\n');
    return 0;
}

int putchar(int c)
{
    latx_bare_uart_putc(c);
    return c;
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

/* ---------- logging / termination ---------- */

void qemu_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)uart_vprintf(fmt, ap);
    va_end(ap);
}

__attribute__((weak)) void latx_bare_assert_fail(const char *expr,
                                                const char *file, int line)
{
    printf("assert: %s at %s:%d\n", expr ? expr : "?", file ? file : "?", line);
    for (;;) {
    }
}

__attribute__((weak)) void abort(void)
{
    latx_bare_uart_puts("abort\n");
    for (;;) {
    }
}

__attribute__((weak)) void exit(int code)
{
    printf("exit(%d)\n", code);
    for (;;) {
    }
}
