#include "mem.h"
#include "lata.h"
#include "lata-mem.h"
#include "target/arm/tcg/translate.h"

void helper_exception_with_syndrome(CPUARMState *env, uint32_t excp,
                                    uint32_t syndrome);

#include <string.h>

void *mm_malloc(int size)
{
    void *retval = malloc(size);
    lsassertm(retval != NULL, "dbt: cannot allocate memory (%d bytes)\n", size);
    return retval;
}

void *mm_calloc(int num, int size)
{
    void *retval = calloc(num, size);
    lsassertm(retval != NULL, "dbt: cannot allocate memory (%d*%d bytes)\n",
              num, size);
    return retval;
}

void *mm_realloc(void *ptr, int size)
{
    void *retval = realloc(ptr, size);
    lsassertm(retval != NULL, "dbt: cannot allocate memory (%d bytes)\n", size);
    return retval;
}

void mm_free(void *ptr) { free(ptr); }

void *mmi_palloc(int size, char *file_name, int line_num)
{
    void *retval = malloc(size);
    lsassertm(retval != NULL, "dbt: cannot allocate memory (%d bytes)\n", size);
    return retval;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_size,
                           uint64_t b_start, uint64_t b_size)
{
    uint64_t a_end = a_start + a_size;
    uint64_t b_end = b_start + b_size;
    if (a_size == 0 || b_size == 0) {
        return false;
    }
    if (a_end < a_start) {
        a_end = UINT64_MAX;
    }
    if (b_end < b_start) {
        b_end = UINT64_MAX;
    }
    return a_start < b_end && b_start < a_end;
}

static LataGuestMemStatus default_probe(CPUARMState *env, uint64_t vaddr,
                                        size_t size, bool is_write,
                                        bool clean_tbi)
{
    if (clean_tbi) {
        vaddr &= UINT64_C(0x00ffffffffffffff);
    }
    if (size == 0) {
        return LATA_MEM_OK;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8 && size != 16) {
        /* Multi-byte span probes are allowed as contiguous byte checks. */
    }
    if (vaddr < UINT64_C(0x1000) || vaddr > UINT64_MAX - size) {
        return LATA_MEM_FAULT_TRANSLATION;
    }
    if (env != NULL && env->sve_ctrl.fault_region_count > 0) {
        uint32_t need = is_write ? LATA_MEM_PERM_WRITE : LATA_MEM_PERM_READ;
        uint32_t count = env->sve_ctrl.fault_region_count;
        if (count > 8) {
            count = 8;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (!env->sve_ctrl.fault_regions[i].valid) {
                continue;
            }
            if (ranges_overlap(vaddr, size,
                               env->sve_ctrl.fault_regions[i].base,
                               env->sve_ctrl.fault_regions[i].size)) {
                if ((env->sve_ctrl.fault_regions[i].perm & need) != need) {
                    return env->sve_ctrl.fault_regions[i].perm == 0
                               ? LATA_MEM_FAULT_UNMAPPED
                               : LATA_MEM_FAULT_PERMISSION;
                }
                return LATA_MEM_OK;
            }
        }
    }
    if (env != NULL && env->sve_ctrl.fault_mem_limit != 0) {
        if (vaddr + size > env->sve_ctrl.fault_mem_limit) {
            return LATA_MEM_FAULT_TRANSLATION;
        }
        return LATA_MEM_OK;
    }
    if (ranges_overlap(vaddr, size, UINT64_C(0x10000000),
                       UINT64_C(0x10000000))) {
        return LATA_MEM_FAULT_UNMAPPED;
    }
    return LATA_MEM_OK;
}

static uint64_t sign_extend_u64(uint64_t val, size_t size)
{
    switch (size) {
    case 1:
        return (uint64_t)(int64_t)(int8_t)(val & 0xffu);
    case 2:
        return (uint64_t)(int64_t)(int16_t)(val & 0xffffu);
    case 4:
        return (uint64_t)(int64_t)(int32_t)(val & 0xffffffffu);
    default:
        return val;
    }
}

static LataGuestMemStatus default_load(CPUARMState *env, uint64_t vaddr,
                                       size_t size, bool is_signed,
                                       uint64_t *out_value)
{
    LataGuestMemStatus st = default_probe(env, vaddr, size, false, false);
    const volatile uint8_t *p;
    uint64_t val = 0;

    if (st != LATA_MEM_OK) {
        return st;
    }
    if (out_value == NULL) {
        return LATA_MEM_FAULT_TRANSLATION;
    }
    p = (const volatile uint8_t *)(uintptr_t)vaddr;
    for (size_t i = 0; i < size && i < 8; ++i) {
        val |= (uint64_t)p[i] << (i * 8u);
    }
    *out_value = is_signed ? sign_extend_u64(val, size) : val;
    return LATA_MEM_OK;
}

static LataGuestMemStatus default_store(CPUARMState *env, uint64_t vaddr,
                                        size_t size, uint64_t value)
{
    LataGuestMemStatus st = default_probe(env, vaddr, size, true, false);
    volatile uint8_t *p;

    if (st != LATA_MEM_OK) {
        return st;
    }
    p = (volatile uint8_t *)(uintptr_t)vaddr;
    for (size_t i = 0; i < size && i < 8; ++i) {
        p[i] = (uint8_t)(value >> (i * 8u));
    }
    return LATA_MEM_OK;
}

/*
 * Default provider: no inline direct access.
 *
 * `default_probe()` is a byte-level address check, not a mapping proof: it
 * cannot say whether host permission equals guest permission, and this build
 * has no mapping-generation counter to invalidate a span that was accepted
 * earlier. Publishing NULL here is the whole point of the contract - the SVE
 * lowering then has no fast path to emit and every access goes through
 * probe/load/store, which report an architectural status.
 */
static LataGuestMemStatus default_load_bytes(CPUARMState *env, uint64_t vaddr,
                                             void *dst, size_t size)
{
    LataGuestMemStatus st = default_probe(env, vaddr, size, false, false);

    if (st != LATA_MEM_OK) {
        return st;
    }
    if (dst == NULL || size == 0) {
        return LATA_MEM_FAULT_TRANSLATION;
    }
    memcpy(dst, (const void *)(uintptr_t)vaddr, size);
    return LATA_MEM_OK;
}

static LataGuestMemStatus default_store_bytes(CPUARMState *env, uint64_t vaddr,
                                              const void *src, size_t size)
{
    LataGuestMemStatus st = default_probe(env, vaddr, size, true, false);

    if (st != LATA_MEM_OK) {
        return st;
    }
    if (src == NULL || size == 0) {
        return LATA_MEM_FAULT_TRANSLATION;
    }
    memcpy((void *)(uintptr_t)vaddr, src, size);
    return LATA_MEM_OK;
}

const LataGuestMemOps lata_default_guest_mem_ops = {
    .probe = default_probe,
    .load = default_load,
    .store = default_store,
    .load_bytes = default_load_bytes,
    .store_bytes = default_store_bytes,
    .direct = NULL,
    /* Controlled bare-metal environment: guest VAs are host VAIs. */
    .va_is_hpa_identity = true,
};

void lata_set_guest_mem_ops(const LataGuestMemOps *ops)
{
    LataContext *ctx = lata_context_current();
    if (ctx != NULL) {
        ctx->mem_ops = ops != NULL ? ops : &lata_default_guest_mem_ops;
    }
}

const LataGuestMemOps *lata_get_guest_mem_ops(void)
{
    LataContext *ctx = lata_context_current();
    if (ctx != NULL && ctx->mem_ops != NULL) {
        return ctx->mem_ops;
    }
    return &lata_default_guest_mem_ops;
}

LataGuestMemStatus lata_guest_mem_probe(CPUARMState *env, uint64_t vaddr,
                                        size_t size, bool is_write,
                                        bool clean_tbi)
{
    const LataGuestMemOps *ops = lata_get_guest_mem_ops();
    return ops->probe(env, vaddr, size, is_write, clean_tbi);
}

LataGuestMemStatus lata_guest_mem_load(CPUARMState *env, uint64_t vaddr,
                                       size_t size, bool is_signed,
                                       uint64_t *out_value)
{
    const LataGuestMemOps *ops = lata_get_guest_mem_ops();
    return ops->load(env, vaddr, size, is_signed, out_value);
}

LataGuestMemStatus lata_guest_mem_store(CPUARMState *env, uint64_t vaddr,
                                        size_t size, uint64_t value)
{
    const LataGuestMemOps *ops = lata_get_guest_mem_ops();
    return ops->store(env, vaddr, size, value);
}

static const LataGuestMemDirectPolicy *direct_policy(void)
{
    const LataGuestMemOps *ops = lata_get_guest_mem_ops();

    if (ops == NULL || ops->direct == NULL) {
        return NULL;
    }
    if (ops->direct->epoch == NULL || ops->direct->span_admissible == NULL) {
        /* A half-installed policy is a provider bug; treat it as "no proof". */
        return NULL;
    }
    return ops->direct;
}

bool lata_guest_mem_direct_available(void)
{
    return direct_policy() != NULL;
}

/*
 * Only the explicit identity claim justifies a raw host access at translation
 * time.  A published `direct` policy is not enough: it proves spans only when
 * `epoch()`/`span_admissible()` are consulted at run time for the actual range,
 * which scalar/NEON lowering does not do yet (the SVE lowering does).  Such
 * providers therefore take the checked (provider call) path until that
 * admission is wired in.
 */
bool lata_guest_mem_inline_host_access_ok(void)
{
    const LataGuestMemOps *ops = lata_get_guest_mem_ops();

    if (ops == NULL) {
        return false;
    }
    return ops->va_is_hpa_identity;
}

static CPUARMState *mapping_env(void)
{
    LataContext *ctx = lata_context_current();

    if (ctx != NULL && ctx->cpu_state != NULL) {
        return (CPUARMState *)ctx->cpu_state;
    }
    return (lsenv != NULL) ? (CPUARMState *)lsenv->cpu_state : NULL;
}

uint64_t lata_guest_mem_direct_epoch(void)
{
    const LataGuestMemDirectPolicy *policy = direct_policy();

    if (policy == NULL) {
        return 0;
    }
    return policy->epoch(mapping_env());
}

bool lata_guest_mem_direct_admit(CPUARMState *env, uint64_t vaddr,
                                 uint64_t size, bool is_write,
                                 uint64_t translation_epoch)
{
    const LataGuestMemDirectPolicy *policy = direct_policy();

    if (policy == NULL || size == 0) {
        return false;
    }
    /*
     * LATA's own fault-window instrumentation is part of the memory contract,
     * not of the host mapping: a window can be installed after a block was
     * translated, so it must veto the fast path at execution time.
     */
    if (env != NULL && (env->sve_ctrl.fault_region_count > 0 ||
                        env->sve_ctrl.fault_mem_limit != 0)) {
        return false;
    }
    /* A span that wraps the address space cannot be one mapping. */
    if (vaddr + size < vaddr) {
        return false;
    }
    /* Stale proof: any mapping/permission change since translation. */
    if (policy->epoch(env) != translation_epoch) {
        return false;
    }
    return policy->span_admissible(env, vaddr, size, is_write);
}

void lata_guest_mem_clear_fault_regions(CPUARMState *env)
{
    if (env == NULL) {
        return;
    }
    memset(env->sve_ctrl.fault_regions, 0,
           sizeof(env->sve_ctrl.fault_regions));
    env->sve_ctrl.fault_region_count = 0;
}

bool lata_guest_mem_add_fault_region(CPUARMState *env, uint64_t base,
                                     uint64_t size, uint32_t perm)
{
    if (env == NULL || env->sve_ctrl.fault_region_count >= 8) {
        return false;
    }
    uint32_t idx = env->sve_ctrl.fault_region_count++;
    env->sve_ctrl.fault_regions[idx].base = base;
    env->sve_ctrl.fault_regions[idx].size = size;
    env->sve_ctrl.fault_regions[idx].perm = perm;
    env->sve_ctrl.fault_regions[idx].valid = true;
    return true;
}

uint64_t lata_guest_mem_load_checked(CPUARMState *env, uint64_t vaddr,
                                     uint32_t size_and_flags, uint64_t *out_val)
{
    size_t size = (size_t)(size_and_flags & 0xffu);
    bool is_signed = (size_and_flags & (1u << 8)) != 0;
    return (uint64_t)lata_guest_mem_load(env, vaddr, size, is_signed, out_val);
}

uint64_t lata_guest_mem_store_checked(CPUARMState *env, uint64_t vaddr,
                                      uint32_t size_and_flags, uint64_t val)
{
    size_t size = (size_t)(size_and_flags & 0xffu);
    return (uint64_t)lata_guest_mem_store(env, vaddr, size, val);
}

uint32_t lata_guest_mem_status_to_dfsc(uint32_t status)
{
    switch ((LataGuestMemStatus)status) {
    case LATA_MEM_FAULT_TRANSLATION:
    case LATA_MEM_FAULT_UNMAPPED:
        /* "Unmapped" is architecturally a translation fault: the walk found no
         * entry. Level 3 is the only level this interface can justify. */
        return 0x07u; /* Translation fault, level 3 */
    case LATA_MEM_FAULT_PERMISSION:
        return 0x0fu; /* Permission fault, level 3 */
    case LATA_MEM_FAULT_ALIGNMENT:
        return 0x21u; /* Alignment fault */
    case LATA_MEM_OK:
    default:
        /* LATA_MEM_OK has no DFSC. Reaching the abort path with it means the
         * caller raised a fault without a fault status; a synchronous external
         * abort is the only encoding that does not invent a page-table story. */
        return 0x10u; /* Synchronous external abort, not on a translation walk */
    }
}

void lata_guest_mem_raise_fault(CPUARMState *env, uint64_t fault_pc,
                                  uint64_t vaddr, uint32_t is_write,
                                  uint32_t status)
{
    uint32_t dfsc = lata_guest_mem_status_to_dfsc(status);
    uint32_t syn = syn_data_abort_curr_el(is_write != 0) | dfsc;

    if (env != NULL) {
        env->pc = fault_pc;
        env->sve_ctrl.last_fault_va = vaddr;
        env->sve_ctrl.last_fault_is_write = (is_write != 0);
    }
    helper_exception_with_syndrome(env, EXCP_DATA_ABORT, syn);
}
