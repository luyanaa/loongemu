#ifndef LATA_ACCEL_TCG_INTERNAL_H
#define LATA_ACCEL_TCG_INTERNAL_H
#include "qemu/osdep.h"
#include "exec/exec-all.h"

/* Translation-block bookkeeping and invalidation hooks used by the TU
 * optimiser; runtime.c owns page membership and direct-jump lifecycle. */
TranslationBlock *tb_htable_lookup(CPUState *cpu, uint64_t pc, uint64_t cs_base,
                                   uint32_t flags, uint32_t cflags);
void tb_link_page(TranslationBlock *tb);
/* Invalidate every TB registered against the containing physical page. */
void tb_invalidate_page(tb_page_addr_t addr);
/* Mark invalid and unlink incoming/outgoing direct jumps before retirement. */
void tb_invalidate_tb(TranslationBlock *tb);
void tb_lock_page0(tb_page_addr_t addr);
void tb_lock_page1(tb_page_addr_t addr);
void tb_set_page_addr0(TranslationBlock *tb, tb_page_addr_t addr);
void tb_set_page_addr1(TranslationBlock *tb, tb_page_addr_t addr);
void tb_reset_jump(TranslationBlock *tb, int n);
void cpu_loop_exit(CPUState *cpu);
void *tcg_malloc(size_t size);
tb_page_addr_t get_page_addr_code(CPUArchState *env, target_ulong pc);
bool have_mmap_lock(void);
typedef unsigned int uint;
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *));
#endif
