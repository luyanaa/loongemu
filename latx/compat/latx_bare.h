#ifndef LATX_BARE_H
#define LATX_BARE_H
/* Freestanding runtime contract for the LATX bare-metal port.
 * Everything here is implemented in baremetal/shim/ (P1: no upstream edits). */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* --- console --- */
void latx_bare_uart_putc(int c);
void latx_bare_uart_puts(const char *s);
int  latx_bare_vprintf(const char *fmt, va_list ap);
int  latx_bare_printf(const char *fmt, ...);
#define uart_printf latx_bare_printf

/* --- guest memory (4KB shadow page table / flat guest RAM) --- */
int      latx_bare_guest_read(uint64_t gpa, void *dst, size_t len);
uint8_t  latx_bare_ldub_code(uint64_t vaddr);
uint32_t latx_bare_ldl_code(uint64_t vaddr);
uint64_t latx_bare_ldq_code(uint64_t vaddr);

/* --- code cache --- */
void *latx_bare_code_cache_alloc(size_t size);
void  latx_bare_icache_flush(void *start, size_t size);

#endif
