#ifndef LATX_BARE_STDIO_H
#define LATX_BARE_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct latx_bare_file { int fd; } FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *b, const char *fmt, ...);
int snprintf(char *b, size_t n, const char *fmt, ...);
int vsnprintf(char *b, size_t n, const char *fmt, va_list ap);
int fflush(FILE *f);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int putchar(int c);
#endif
