/* Bare-runtime glue for the LATA core. TB jump/page lifecycle hooks below are
 * concrete; unrelated platform hooks retain weak fallback behavior. */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "accel/tcg/internal.h"
#include "tcg/tcg.h"
#include "target/arm/tcg/translate.h"
#include "cpregs.h"
#include "ir2-patch.h"
#include "loongemu/tb_cache.h"

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

static void tb_invalidate_all_pages(void);
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
LATA_WEAK void tb_flush(CPUState *cpu)
{
    (void)cpu;
    tb_invalidate_all_pages();
}
LATA_WEAK TranslationBlock *tb_htable_lookup(CPUState *cpu, uint64_t pc,
                                             uint64_t cs_base, uint32_t flags,
                                             uint32_t cflags)
{ (void)cpu; (void)pc; (void)cs_base; (void)flags; (void)cflags; return 0; }

/*
 * The bare LATA runtime keeps a bounded page index for physical TB pages.
 * Page/jump graph mutations share one lock: link and invalidation are rare,
 * and serializing them keeps the two outgoing slots and each destination's
 * incoming list consistent.
 */
#define TB_PAGE_BUCKETS 4096u
#define TB_PAGE_BUCKET_MASK (TB_PAGE_BUCKETS - 1u)
#define TB_PAGE_ADDR_INVALID ((tb_page_addr_t)-1)

typedef struct TbPageBucket {
    tb_page_addr_t addr;
    uintptr_t head;
    uint8_t state; /* 0 empty, 1 occupied, 2 tombstone */
} TbPageBucket;

static volatile int tb_lifecycle_lock;
static TbPageBucket tb_page_buckets[TB_PAGE_BUCKETS];

static void tb_lifecycle_lock_acquire(void)
{
    while (__atomic_exchange_n(&tb_lifecycle_lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&tb_lifecycle_lock, __ATOMIC_RELAXED)) {
        }
    }
}

static void tb_lifecycle_lock_release(void)
{
    __atomic_store_n(&tb_lifecycle_lock, 0, __ATOMIC_RELEASE);
}

static tb_page_addr_t tb_page_base(tb_page_addr_t addr)
{
    return addr & (tb_page_addr_t)TBC_PAGE_MASK;
}

static TbPageBucket *tb_page_find(tb_page_addr_t addr, bool create)
{
    tb_page_addr_t page = tb_page_base(addr);
    uint32_t slot = (uint32_t)((page >> TBC_PAGE_BITS) ^
                               (page >> (TBC_PAGE_BITS + 32))) &
                    TB_PAGE_BUCKET_MASK;
    TbPageBucket *tombstone = NULL;

    for (uint32_t i = 0; i < TB_PAGE_BUCKETS; i++) {
        TbPageBucket *bucket = &tb_page_buckets[(slot + i) &
                                                TB_PAGE_BUCKET_MASK];
        if (bucket->state == 1) {
            if (bucket->addr == page) {
                return bucket;
            }
        } else if (bucket->state == 2) {
            if (tombstone == NULL) {
                tombstone = bucket;
            }
        } else {
            return create ? (tombstone != NULL ? tombstone : bucket) : NULL;
        }
    }
    return create ? tombstone : NULL;
}

static TranslationBlock *tb_jmp_node_tb(uintptr_t node)
{
    return (TranslationBlock *)(node & ~(uintptr_t)1);
}

static int tb_jmp_node_index(uintptr_t node)
{
    return (int)(node & 1u);
}
static bool tb_patch_branch(TranslationBlock *tb, int n, uintptr_t target)
{
    uint16_t offset;

    if (tb == NULL || n < 0 || n > 1 || tb->tc.ptr == NULL) {
        return false;
    }
    offset = tb->jmp_insn_offset[n];
    if (offset == TB_JMP_OFFSET_INVALID ||
        (size_t)offset > tb->tc.size ||
        tb->tc.size - (size_t)offset < sizeof(uint32_t)) {
        return false;
    }
    return loongarch_patch_b((uint32_t *)(void *)(tb->tc.ptr + offset),
                             target);
}

static bool tb_patch_reset(TranslationBlock *tb, int n)
{
    uintptr_t target;

    if (tb == NULL || n < 0 || n > 1 || tb->tc.ptr == NULL ||
        tb->jmp_reset_offset[n] == TB_JMP_OFFSET_INVALID ||
        (size_t)tb->jmp_reset_offset[n] >= tb->tc.size) {
        return false;
    }
    target = (uintptr_t)tb->tc.ptr + tb->jmp_reset_offset[n];
    return tb_patch_branch(tb, n, target);
}

static void tb_unlink_outgoing(TranslationBlock *tb, int n)
{
    TranslationBlock *dest = tb->next_tb[n];

    if (tb->jmp_dest[n] != 0 && dest != NULL &&
        (uintptr_t)dest->tc.ptr == tb->jmp_dest[n]) {
        uintptr_t wanted = (uintptr_t)tb | (uintptr_t)n;
        uintptr_t *link = &dest->jmp_list_head;

        while (*link != 0) {
            uintptr_t node = *link;
            TranslationBlock *origin = tb_jmp_node_tb(node);
            int index = tb_jmp_node_index(node);

            if (node == wanted) {
                *link = tb->jmp_list_next[n];
                break;
            }
            link = &origin->jmp_list_next[index];
        }
    }
    tb->jmp_list_next[n] = 0;
    tb->jmp_dest[n] = 0;
}

static bool tb_reset_jump_locked(TranslationBlock *tb, int n)
{
    if (!tb_patch_reset(tb, n)) {
        tb->cflags |= CF_INVALID;
        return false;
    }
    tb_unlink_outgoing(tb, n);
    return true;
}

static bool tb_unlink_incoming(TranslationBlock *tb)
{
    while (tb->jmp_list_head != 0) {
        uintptr_t node = tb->jmp_list_head;
        TranslationBlock *origin = tb_jmp_node_tb(node);
        int n = tb_jmp_node_index(node);

        if (!tb_patch_reset(origin, n)) {
            origin->cflags |= CF_INVALID;
            return false;
        }
        tb->jmp_list_head = origin->jmp_list_next[n];
        origin->jmp_list_next[n] = 0;
        origin->jmp_dest[n] = 0;
    }
    return true;
}

static bool tb_invalidate_locked(TranslationBlock *tb)
{
    bool safe_to_unlink = tb_unlink_incoming(tb);

    tb->cflags |= CF_INVALID;
    for (int n = 0; n < 2; n++) {
        if (tb->jmp_dest[n] != 0 && !tb_reset_jump_locked(tb, n)) {
            safe_to_unlink = false;
        }
    }
    return safe_to_unlink;
}


static void tb_page_unlink(TranslationBlock *tb);
void tb_invalidate_tb(TranslationBlock *tb)
{
    if (tb == NULL) {
        return;
    }
    tb_lifecycle_lock_acquire();
    if (tb_invalidate_locked(tb)) {
        tb_page_unlink(tb);
    }
    tb_lifecycle_lock_release();
}

static bool tb_page_linked(TranslationBlock *tb)
{
    for (int i = 0; i < 2; i++) {
        tb_page_addr_t addr = tb->page_addr[i];
        TbPageBucket *bucket;
        uintptr_t node;

        if (addr == TB_PAGE_ADDR_INVALID ||
            (i == 1 && tb->page_addr[0] != TB_PAGE_ADDR_INVALID &&
             tb_page_base(addr) == tb_page_base(tb->page_addr[0]))) {
            continue;
        }
        bucket = tb_page_find(addr, false);
        for (node = bucket != NULL ? bucket->head : 0; node != 0; ) {
            if (tb_jmp_node_tb(node) == tb && tb_jmp_node_index(node) == i) {
                return true;
            }
            TranslationBlock *next = tb_jmp_node_tb(node);
            node = next->page_next[tb_jmp_node_index(node)];
        }
    }
    return false;
}

static void tb_page_unlink(TranslationBlock *tb)
{
    for (int i = 0; i < 2; i++) {
        tb_page_addr_t addr = tb->page_addr[i];
        TbPageBucket *bucket;
        uintptr_t wanted;
        uintptr_t *link;

        if (addr == TB_PAGE_ADDR_INVALID ||
            (i == 1 && tb->page_addr[0] != TB_PAGE_ADDR_INVALID &&
             tb_page_base(addr) == tb_page_base(tb->page_addr[0]))) {
            tb->page_next[i] = 0;
            continue;
        }
        bucket = tb_page_find(addr, false);
        wanted = (uintptr_t)tb | (uintptr_t)i;
        link = bucket != NULL ? &bucket->head : NULL;
        while (link != NULL && *link != 0) {
            uintptr_t node = *link;
            TranslationBlock *current = tb_jmp_node_tb(node);
            int index = tb_jmp_node_index(node);

            if (node == wanted) {
                *link = tb->page_next[i];
                break;
            }
            link = &current->page_next[index];
        }
        tb->page_next[i] = 0;
        if (bucket != NULL && bucket->head == 0) {
            bucket->state = 2;
        }
    }
    tb->page_addr[0] = TB_PAGE_ADDR_INVALID;
    tb->page_addr[1] = TB_PAGE_ADDR_INVALID;
}

static bool tb_page_link(TranslationBlock *tb)
{
    if (tb_page_linked(tb)) {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        tb_page_addr_t addr = tb->page_addr[i];
        TbPageBucket *bucket;

        if (addr == TB_PAGE_ADDR_INVALID ||
            (i == 1 && tb->page_addr[0] != TB_PAGE_ADDR_INVALID &&
             tb_page_base(addr) == tb_page_base(tb->page_addr[0]))) {
            continue;
        }
        bucket = tb_page_find(addr, true);
        if (bucket == NULL) {
            tb_page_unlink(tb);
            return false;
        }
        if (bucket->state != 1) {
            bucket->addr = tb_page_base(addr);
            bucket->head = 0;
            bucket->state = 1;
        }
        tb->page_next[i] = bucket->head;
        bucket->head = (uintptr_t)tb | (uintptr_t)i;
    }
    return true;
}

void tb_link_page(TranslationBlock *tb)
{
    if (tb == NULL) {
        return;
    }
    tb_lifecycle_lock_acquire();
    if (!tb_page_link(tb)) {
        tb_invalidate_locked(tb);
    }
    tb_lifecycle_lock_release();
}

void tb_set_page_addr0(TranslationBlock *tb, tb_page_addr_t addr)
{
    if (tb == NULL) {
        return;
    }
    tb_lifecycle_lock_acquire();
    tb->page_addr[0] = addr == TB_PAGE_ADDR_INVALID ? addr : tb_page_base(addr);
    tb->page_next[0] = 0;
    tb_lifecycle_lock_release();
}

void tb_set_page_addr1(TranslationBlock *tb, tb_page_addr_t addr)
{
    if (tb == NULL) {
        return;
    }
    tb_lifecycle_lock_acquire();
    tb->page_addr[1] = addr == TB_PAGE_ADDR_INVALID ? addr : tb_page_base(addr);
    tb->page_next[1] = 0;
    tb_lifecycle_lock_release();
}

void tb_invalidate_page(tb_page_addr_t addr)
{
    TbPageBucket *bucket;

    if (addr == TB_PAGE_ADDR_INVALID) {
        return;
    }
    tb_lifecycle_lock_acquire();
    bucket = tb_page_find(addr, false);
    while (bucket != NULL && bucket->head != 0) {
        TranslationBlock *tb = tb_jmp_node_tb(bucket->head);

        if (!tb_invalidate_locked(tb)) {
            break;
        }
        tb_page_unlink(tb);
    }
    if (bucket != NULL && bucket->head == 0) {
        bucket->state = 2;
    }
    tb_lifecycle_lock_release();
}

/*
 * A full cache flush retires page-linked TBs but does not recycle their code
 * bytes. The translator's code-arena owner may reclaim only after execution
 * has left that arena.
 */
static void tb_invalidate_all_pages(void)
{
    tb_lifecycle_lock_acquire();
    for (uint32_t i = 0; i < TB_PAGE_BUCKETS; i++) {
        TbPageBucket *bucket = &tb_page_buckets[i];

        while (bucket->state == 1 && bucket->head != 0) {
            TranslationBlock *tb = tb_jmp_node_tb(bucket->head);

            if (!tb_invalidate_locked(tb)) {
                break;
            }
            tb_page_unlink(tb);
        }
        if (bucket->state == 1 && bucket->head == 0) {
            bucket->addr = 0;
            bucket->state = 2;
        }
    }
    tb_lifecycle_lock_release();
}

void tb_reset_jump(TranslationBlock *tb, int n)
{
    if (tb == NULL || n < 0 || n > 1) {
        return;
    }
    tb_lifecycle_lock_acquire();
    tb_reset_jump_locked(tb, n);
    tb_lifecycle_lock_release();
}

void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{
    TranslationBlock *dest;

    if (tb == NULL || n < 0 || n > 1) {
        return;
    }
    tb_lifecycle_lock_acquire();
    if (!tb_patch_reset(tb, n)) {
        tb->cflags |= CF_INVALID;
        tb_lifecycle_lock_release();
        return;
    }
    tb_unlink_outgoing(tb, n);
    if ((tb->cflags & CF_INVALID) != 0) {
        tb_lifecycle_lock_release();
        return;
    }
    dest = tb->next_tb[n];
    if (dest != NULL && (uintptr_t)dest->tc.ptr != addr) {
        dest = NULL;
    }
    if (dest != NULL && (dest->cflags & CF_INVALID) != 0) {
        tb_lifecycle_lock_release();
        return;
    }
    if (addr == 0 || !tb_patch_branch(tb, n, addr)) {
        tb_lifecycle_lock_release();
        return;
    }
    tb->jmp_dest[n] = addr;
    if (dest != NULL) {
        tb->jmp_list_next[n] = dest->jmp_list_head;
        dest->jmp_list_head = (uintptr_t)tb | (uintptr_t)n;
    }
    tb_lifecycle_lock_release();
}

/* Locking of translated pages is folded into the lifecycle lock above. */
LATA_WEAK void tb_lock_page0(tb_page_addr_t addr) { (void)addr; }
LATA_WEAK void tb_lock_page1(tb_page_addr_t addr) { (void)addr; }
LATA_WEAK void *tcg_malloc(size_t size) { extern void *malloc(size_t); return malloc(size); }
LATA_WEAK void *tcg_tb_alloc_full(TCGContext *s) { (void)s; return 0; }
LATA_WEAK void tcg_tb_insert(TranslationBlock *tb) { (void)tb; }
LATA_WEAK int tr_translate_tb(TranslationBlock *tb) { (void)tb; return 0; }
