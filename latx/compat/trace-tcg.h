#ifndef LATX_BARE_TRACE_TCG_H
#define LATX_BARE_TRACE_TCG_H
#include "qemu/osdep.h"
static inline void trace_guest_mem_before_exec(void *env, uint64_t vaddr, int info) { (void)env; (void)vaddr; (void)info; }
#endif
