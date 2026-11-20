/* Weak host/runtime boundary for the freestanding LATM core. */
#include "qemu/osdep.h"
#include "latm.h"
#include "exec/helper-proto.h"

#define LATM_WEAK __attribute__((weak))

LATM_WEAK TCGContext latm_tcg_ctx;
LATM_WEAK TCGContext *tcg_ctx = &latm_tcg_ctx;
LATM_WEAK int option_fam_jmp_cache;

LATM_WEAK void qemu_log(const char *fmt, ...) { (void)fmt; }
LATM_WEAK void *tcg_splitwx_to_rx(void *p) { return p; }

LATM_WEAK void cpu_loop_exit(struct CPUState *cs)
{
    (void)cs;
    for (;;) { }
}

LATM_WEAK void tcg_debug_assert_(int cond) { (void)cond; }
LATM_WEAK void g_assert_not_reached(void) { for (;;) { } }
LATM_WEAK void latm_tb_flush_range(uint64_t start, uint64_t end)
{ (void)start; (void)end; }
LATM_WEAK void qemu_spin_init(QemuSpin *s) { (void)s; }
LATM_WEAK void tb_flush(struct CPUState *cpu) { (void)cpu; }
LATM_WEAK TranslationBlock *tb_htable_lookup(struct CPUState *cpu, uint64_t pc,
                                             uint64_t cs_base, uint32_t flags,
                                             uint32_t cflags)
{
    (void)cpu; (void)pc; (void)cs_base; (void)flags; (void)cflags;
    return NULL;
}
LATM_WEAK void tb_link_page(TranslationBlock *tb) { (void)tb; }
LATM_WEAK void tb_lock_page0(tb_page_addr_t addr) { (void)addr; }
LATM_WEAK void tb_lock_page1(tb_page_addr_t addr) { (void)addr; }
LATM_WEAK void tb_reset_jump(TranslationBlock *tb, int n) { (void)tb; (void)n; }
LATM_WEAK void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{ (void)tb; (void)n; (void)addr; }
LATM_WEAK void tb_set_page_addr0(TranslationBlock *tb, tb_page_addr_t addr)
{ (void)tb; (void)addr; }
LATM_WEAK void tb_set_page_addr1(TranslationBlock *tb, tb_page_addr_t addr)
{ (void)tb; (void)addr; }
LATM_WEAK void qemu_thread_jit_write(void) { }
LATM_WEAK bool have_mmap_lock(void) { return true; }
LATM_WEAK void mmap_unlock(void) { }
LATM_WEAK void *tcg_tb_alloc_full(TCGContext *s) { (void)s; return NULL; }
LATM_WEAK void tcg_tb_insert(TranslationBlock *tb) { (void)tb; }
LATM_WEAK int tr_translate_tb(TranslationBlock *tb) { (void)tb; return 0; }
LATM_WEAK int encode_search(TranslationBlock *tb, void *p)
{ (void)tb; (void)p; return 0; }
LATM_WEAK tb_page_addr_t get_page_addr_code(void *env, target_ulong pc)
{ (void)env; return pc; }
