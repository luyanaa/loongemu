/* Weak defaults for the LATA core's host/runtime boundary.
 * A real VMM, or the LATX bare runtime, may override any of these. */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"
#include "target/arm/tcg/translate.h"
#include "cpregs.h"

#define LATA_WEAK __attribute__((weak))

LATA_WEAK int option_fam_jmp_cache;
LATA_WEAK int qemu_loglevel;
LATA_WEAK CPUState *current_cpu;

LATA_WEAK uint32_t lata_guest_ldl(uint64_t addr) { (void)addr; return 0; }

LATA_WEAK int arm_mmu_idx_to_el(int idx) { return idx & 3; }
LATA_WEAK int arm_rmode_to_sf(int rmode)
{
    switch (rmode) {
    case 0: return 0;   /* float_round_nearest_even -> FPSCR 00 */
    case 2: return 1;   /* float_round_up           -> FPSCR 01 */
    case 1: return 2;   /* float_round_down         -> FPSCR 10 */
    case 3: return 3;   /* float_round_to_zero      -> FPSCR 11 */
    case 4: return 4;   /* float_round_ties_away    -> FPROUNDING_TIEAWAY */
    default: return 0;
    }
}
LATA_WEAK CPUARMTBFlags arm_tbflags_from_tb(TranslationBlock *tb)
{
    CPUARMTBFlags f = {{0}};
    if (tb) { f.flags[0] = tb->flags; }
    return f;
}
LATA_WEAK int tb_cflags(const TranslationBlock *tb) { return tb ? (int)tb->cflags : 0; }
LATA_WEAK const ARMCPRegInfo *get_arm_cp_reginfo(void *regs, uint32_t key)
{ (void)regs; (void)key; return 0; }

LATA_WEAK void qemu_log(const char *fmt, ...) { (void)fmt; }
LATA_WEAK void latx_bare_assert_fail(const char *expr, const char *file, int line)
{ (void)expr; (void)file; (void)line; for (;;) { } }
LATA_WEAK void tcg_debug_assert_(int cond) { (void)cond; }
LATA_WEAK void g_assert_not_reached(void) { for (;;) { } }
LATA_WEAK void *tcg_splitwx_to_rx(void *p) { return p; }

LATA_WEAK TCGContext lata_tcg_ctx;
LATA_WEAK TCGContext *tcg_ctx = &lata_tcg_ctx;

/*
 * System-register access.
 *
 * MRS/MSR reach these through the disassembler, which resolves the register to
 * an ARMCPRegInfo and passes it here.  The value lives in CPUARMState at
 * `fieldoffset`, so a read is a load and a write is a store -- the register
 * file is the state, and keeping a second copy in this file would let the two
 * disagree.
 *
 * ARM_CP_CONST registers ignore writes and return their reset value; ARM_CP_NOP
 * does nothing in either direction.  Anything with a readfn/writefn is a
 * register whose behaviour is not a plain field (a counter, a remapped index),
 * and returning a stored field for those would be a fabrication -- so they are
 * reported as zero rather than silently pretending to work.
 */
LATA_WEAK __attribute__((used)) const ARMCPRegInfo *
lata_cp_reg_lookup(uint32_t key)
{
    return get_arm_cp_reginfo(NULL, key);
}

LATA_WEAK uint64_t helper_get_cp_reg64(CPUARMState *env,
                                       const ARMCPRegInfo *ri)
{
    if (env == NULL || ri == NULL) {
        return 0;
    }
    if (ri->type & ARM_CP_NOP) {
        return 0;
    }
    if (ri->type & ARM_CP_CONST) {
        return ri->resetvalue;
    }
    if (ri->fieldoffset <= 0) {
        return 0;              /* not a plain field; see above */
    }
    return *(const uint64_t *)((const uint8_t *)env + ri->fieldoffset);
}

LATA_WEAK void helper_set_cp_reg64(CPUARMState *env,
                                   const ARMCPRegInfo *ri, uint64_t value)
{
    if (env == NULL || ri == NULL) {
        return;
    }
    if (ri->type & (ARM_CP_NOP | ARM_CP_CONST)) {
        return;                /* read-only in hardware */
    }
    if (ri->fieldoffset <= 0) {
        return;
    }
    *(uint64_t *)((uint8_t *)env + ri->fieldoffset) = value;
}

LATA_WEAK void lata_ir2_debug(unsigned int opcode, unsigned int word)
{
    (void)opcode;
    (void)word;
}

/* Opt-in switch for optional AArch64 ISA extensions (see translate.h). */
LATA_WEAK bool lata_isa_optional_features;

#define LATA_HELPER_STUB(name) LATA_WEAK void name(void) { }
LATA_HELPER_STUB(helper_access_check_cp_reg)
LATA_WEAK void helper_exception_with_syndrome(CPUARMState *env,
                                              uint32_t excp,
                                              uint32_t syndrome)
{
    if (env != NULL) {
        env->sve_ctrl.last_exception_index = excp;
        env->sve_ctrl.last_exception_syndrome = syndrome;
    }
}
LATA_HELPER_STUB(helper_lookup_cp_reg)
LATA_HELPER_STUB(helper_simd_tblx)
LATA_HELPER_STUB(helper_neon_addl_saturate_s32)
LATA_HELPER_STUB(helper_neon_addl_saturate_s64)
LATA_HELPER_STUB(helper_vfp_tosld)
LATA_HELPER_STUB(helper_vfp_tosls)
LATA_HELPER_STUB(helper_vfp_tosqd)
LATA_HELPER_STUB(helper_vfp_tosqs)
LATA_HELPER_STUB(helper_vfp_tould)
LATA_HELPER_STUB(helper_vfp_touls)
LATA_HELPER_STUB(helper_vfp_touqd)
LATA_HELPER_STUB(helper_vfp_touqs)

/* TU / code-generation internals: inert unless the embedder installs a real
 * translation-cache implementation. */
LATA_WEAK void cpu_loop_exit(CPUState *cpu) { (void)cpu; for (;;) { } }
LATA_WEAK int encode_search(TranslationBlock *tb, void *p) { (void)tb; (void)p; return 0; }
LATA_WEAK tb_page_addr_t get_page_addr_code(CPUArchState *env, target_ulong pc)
{ (void)env; return pc; }
LATA_WEAK bool have_mmap_lock(void) { return true; }
LATA_WEAK void mmap_unlock(void) { }
LATA_WEAK void qemu_spin_init(QemuSpin *s) { (void)s; }
LATA_WEAK void qemu_thread_jit_write(void) { }
LATA_WEAK void tb_flush(CPUState *cpu) { (void)cpu; }
LATA_WEAK TranslationBlock *tb_htable_lookup(CPUState *cpu, uint64_t pc,
                                             uint64_t cs_base, uint32_t flags,
                                             uint32_t cflags)
{ (void)cpu; (void)pc; (void)cs_base; (void)flags; (void)cflags; return 0; }
LATA_WEAK void tb_link_page(TranslationBlock *tb) { (void)tb; }
LATA_WEAK void tb_lock_page0(tb_page_addr_t addr) { (void)addr; }
LATA_WEAK void tb_lock_page1(tb_page_addr_t addr) { (void)addr; }
LATA_WEAK void tb_reset_jump(TranslationBlock *tb, int n) { (void)tb; (void)n; }
LATA_WEAK void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{ (void)tb; (void)n; (void)addr; }
LATA_WEAK void tb_set_page_addr0(TranslationBlock *tb, tb_page_addr_t addr)
{ (void)tb; (void)addr; }
LATA_WEAK void tb_set_page_addr1(TranslationBlock *tb, tb_page_addr_t addr)
{ (void)tb; (void)addr; }
LATA_WEAK void *tcg_malloc(size_t size) { extern void *malloc(size_t); return malloc(size); }
LATA_WEAK void *tcg_tb_alloc_full(TCGContext *s) { (void)s; return 0; }
LATA_WEAK void tcg_tb_insert(TranslationBlock *tb) { (void)tb; }
LATA_WEAK int tr_translate_tb(TranslationBlock *tb) { (void)tb; return 0; }
