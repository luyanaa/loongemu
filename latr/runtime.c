#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"

#define LATR_WEAK __attribute__((weak))

LATR_WEAK void qemu_log(const char *fmt, ...) { (void)fmt; }
LATR_WEAK void lata_ir2_debug(unsigned int opcode, unsigned int word)
{
    (void)opcode;
    (void)word;
}
LATR_WEAK void latx_bare_assert_fail(const char *expr, const char *file, int line)
{
    (void)expr;
    (void)file;
    (void)line;
    for (;;) { }
}
LATR_WEAK void tcg_debug_assert_(int condition) { (void)condition; }
LATR_WEAK void g_assert_not_reached(void) { for (;;) { } }
LATR_WEAK void *tcg_splitwx_to_rx(void *ptr) { return ptr; }
LATR_WEAK int tb_cflags(const TranslationBlock *tb)
{
    return tb == NULL ? 0 : (int)tb->cflags;
}

LATR_WEAK TCGContext latr_tcg_ctx;
LATR_WEAK TCGContext *tcg_ctx = &latr_tcg_ctx;
