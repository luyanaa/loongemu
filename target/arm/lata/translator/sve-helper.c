#include "sve-lasx-lower.h"
#include "target/arm/lata/include/env.h"
#include "target/arm/lata/include/lata.h"
#include "target/arm/lata/include/lata-mem.h"
#include "target/arm/lata/include/translate.h"
#include "target/arm/tcg/translate.h"
#include "qemu/log.h"

#include <limits.h>
#include <string.h>

/* helper-proto.h declares this with no prototype; the definition below matches
 * the real signature, exactly as mem.c does. */
void helper_exception_with_syndrome(CPUARMState *env, uint32_t excp,
                                    uint32_t syndrome);

/*
 * Descriptors are allocated in chunks that belong to one translation block.
 * A chunk is never split between blocks and never freed while its block can
 * still run, which is exactly the lifetime the generated code assumes when it
 * embeds `&chunk->descs[i]` as an immediate.
 */
#define LATA_SVE_HELPER_CHUNK_DESCS 8u

typedef struct LataSVEHelperChunk {
    struct LataSVEHelperChunk *next;
    const void *owner;
    uint64_t pc_first;
    uint64_t generation;
    uint32_t used;
} LataSVEHelperChunk;

#define LATA_SVE_HELPER_CHUNK_BYTES                                        \
    (sizeof(LataSVEHelperChunk) +                                          \
     LATA_SVE_HELPER_CHUNK_DESCS * sizeof(LataSVEHelperDesc))

static LataSVEHelperDesc *chunk_descs(LataSVEHelperChunk *chunk)
{
    return (LataSVEHelperDesc *)(chunk + 1);
}

static void chunk_free_chain(LataSVEHelperChunk *chunk)
{
    while (chunk != NULL) {
        LataSVEHelperChunk *next = chunk->next;

        mm_free(chunk);
        chunk = next;
    }
}

/*
 * Drop every chunk. Callers must only do this at a boundary where no guest
 * code can run again until retranslation - a code-cache flush, or context
 * teardown - because generated code embeds chunk addresses as immediates.
 */
void lata_sve_helper_release_all(void)
{
    LataContext *ctx = lata_context_current();
    LataSVEHelperChunk *chunk;

    if (ctx == NULL) {
        return;
    }
    chunk = (LataSVEHelperChunk *)ctx->sve_helper_list;
    ctx->sve_helper_list = NULL;
    ctx->sve_helper_active = NULL;
    ctx->sve_helper_owner = NULL;
    ctx->sve_helper_pc = 0;
    ctx->sve_helper_descs = 0;
    ctx->sve_helper_chunks = 0;
    chunk_free_chain(chunk);
}

void lata_sve_helper_begin_tb(const void *owner, uint64_t pc_first)
{
    LataContext *ctx = lata_context_current();
    LataSVEHelperChunk *chunk;

    if (ctx == NULL) {
        return;
    }
    /*
     * Only announce the group; nothing is freed here. Blocks produced by one
     * translation pass coexist (TU generation emits several before any of them
     * runs), so chunks of other groups may still be referenced by live code.
     * Reuse of a partial chunk of the *same* group is safe: it happens only
     * when the very same block is retranslated, and retranslation implies the
     * previous code for that block was invalidated or flushed. Appending keeps
     * the old (dead) descriptors' addresses valid until release_all/release_tb
     * drops the chunk.
     */
    /*
     * Multi-vCPU note: the code cache is shared, the chunk list is per-thread
     * (LataContext is TLS) and every TB is translated by exactly one thread, so
     * a thread only ever frees descriptors its own code embeds.  A flush by
     * another thread rewinds the shared allocator; checking at every block start
     * - not only at helper allocation - keeps the leak window to one block.
     */
    /*
     * A generation change is the authoritative signal: an explicit flush bumps
     * it, and so does a detected rewind.  The position comparison alone cannot
     * tell after the allocator has grown back past the old address.
     */
    if (ctx->sve_helper_chunks != 0) {
        if (ctx->sve_helper_codegen_gen != lata_code_cache_current_generation()) {
            lata_sve_helper_release_all();
        } else if (lata_sve_helper_codegen_rewound(ctx->sve_helper_codegen,
                                                   (uintptr_t)tcg_ctx->code_gen_ptr)) {
            lata_code_cache_bump_generation();
            lata_sve_helper_release_all();
        }
    }
    ctx->sve_helper_owner = owner;
    ctx->sve_helper_pc = pc_first;
    ctx->sve_helper_active = NULL;
    for (chunk = (LataSVEHelperChunk *)ctx->sve_helper_list; chunk != NULL;
         chunk = chunk->next) {
        if (chunk->owner == owner && chunk->pc_first == pc_first &&
            chunk->used < LATA_SVE_HELPER_CHUNK_DESCS) {
            ctx->sve_helper_active = chunk;
            break;
        }
    }
}

void lata_sve_helper_release_tb(const void *owner)
{
    LataContext *ctx = lata_context_current();
    LataSVEHelperChunk **link;

    if (ctx == NULL) {
        return;
    }
    link = (LataSVEHelperChunk **)&ctx->sve_helper_list;
    while (*link != NULL) {
        LataSVEHelperChunk *chunk = *link;

        if (chunk->owner == owner) {
            *link = chunk->next;
            ctx->sve_helper_descs -= chunk->used;
            ctx->sve_helper_chunks--;
            if (ctx->sve_helper_active == chunk) {
                ctx->sve_helper_active = NULL;
            }
            chunk->next = NULL;
            chunk_free_chain(chunk);
        } else {
            link = &chunk->next;
        }
    }
}

void lata_sve_helper_stats(uint32_t *live_descs, uint32_t *live_chunks)
{
    LataContext *ctx = lata_context_current();

    if (live_descs != NULL) {
        *live_descs = (ctx != NULL) ? ctx->sve_helper_descs : 0;
    }
    if (live_chunks != NULL) {
        *live_chunks = (ctx != NULL) ? ctx->sve_helper_chunks : 0;
    }
}

/*
 * A code-cache flush rewinds the allocator to the start of the buffer without
 * calling us (SMC, address-space change and cross-CPU invalidations all go
 * through QEMU's tb_flush).  Observing a position earlier than the one recorded
 * when the live chunks were handed out proves those chunks belong to dead code,
 * so they are dropped here instead of waiting for the next explicit flush site.
 */
bool lata_sve_helper_codegen_rewound(uintptr_t recorded, uintptr_t current)
{
    return recorded != 0 && current < recorded;
}

const LataSVEHelperDesc *lata_sve_alloc_helper_desc(
    const DisasContext *s, const PvirInstruction *instruction,
    const A64SVEIR1Instruction *source)
{
    LataContext *ctx = lata_context_current();
    LataSVEHelperChunk *chunk;
    LataSVEHelperDesc *desc;
    const void *owner;
    uint64_t pc_first;

    if (s == NULL || instruction == NULL || ctx == NULL) {
        return NULL;
    }
    /*
     * The group key is the TranslationBlock pointer, not the shared
     * DisasContextBase: every live TB has its own group, so consecutive TU
     * generations never collide. A caller that never announced a block
     * (hand-built IR2, fixtures) gets the same treatment with owner == NULL
     * keyed by the current PC.
     */
    owner = (s->base != NULL && s->base->tb != NULL)
                ? (const void *)s->base->tb->tc.ptr
                : NULL;
    pc_first = (s->base != NULL) ? s->base->pc_first : s->pc_curr;
    if (ctx->sve_helper_chunks != 0) {
        if (ctx->sve_helper_codegen_gen != lata_code_cache_current_generation()) {
            lata_sve_helper_release_all();
        } else if (lata_sve_helper_codegen_rewound(ctx->sve_helper_codegen,
                                                   (uintptr_t)tcg_ctx->code_gen_ptr)) {
            lata_code_cache_bump_generation();
            lata_sve_helper_release_all();
        }
    }
    if (ctx->sve_helper_chunks == 0) {
        ctx->sve_helper_codegen = (uintptr_t)tcg_ctx->code_gen_ptr;
        ctx->sve_helper_codegen_gen = lata_code_cache_current_generation();
    }
    if (ctx->sve_helper_owner != owner || ctx->sve_helper_pc != pc_first) {
        lata_sve_helper_begin_tb(owner, pc_first);
    }
    chunk = (LataSVEHelperChunk *)ctx->sve_helper_active;
    if (chunk == NULL || chunk->used >= LATA_SVE_HELPER_CHUNK_DESCS) {
        chunk = (LataSVEHelperChunk *)mm_calloc(1, LATA_SVE_HELPER_CHUNK_BYTES);
        chunk->owner = owner;
        chunk->pc_first = pc_first;
        chunk->generation = lata_code_cache_current_generation();
        chunk->next = (LataSVEHelperChunk *)ctx->sve_helper_list;
        ctx->sve_helper_list = chunk;
        ctx->sve_helper_active = chunk;
        ctx->sve_helper_chunks++;
    }
    desc = &chunk_descs(chunk)[chunk->used++];
    ctx->sve_helper_descs++;
    memset(desc, 0, sizeof(*desc));
    desc->pvir = *instruction;
    if (source != NULL) {
        desc->has_source = true;
        desc->source = *source;
    }
    desc->vl_bytes = (uint16_t)s->vl;
    desc->tbid = (uint8_t)s->tbid;
    desc->pc_curr = s->pc_curr;
    desc->pc_next = (s->base != NULL) ? s->base->pc_next : (s->pc_curr + 4u);
    if (s->sve_pvir_builder != NULL) {
        memcpy(desc->z_values, s->sve_pvir_builder->z_values,
               sizeof(desc->z_values));
        memcpy(desc->p_values, s->sve_pvir_builder->p_values,
               sizeof(desc->p_values));
        desc->ffr_value = s->sve_pvir_builder->ffr_value;
    }
    return desc;
}

void lata_sve_helper_list_free(void *head)
{
    chunk_free_chain((LataSVEHelperChunk *)head);
}

static int h_find_zreg(const LataSVEHelperDesc *d, PvirValueId id)
{
    if (id == PVIR_NO_VALUE) {
        return -1;
    }
    for (int i = 0; i < (int)A64_SVE_PVIR_NUM_ZREGS; ++i) {
        if (d->z_values[i] == id) {
            return i;
        }
    }
    if (id < A64_SVE_PVIR_NUM_ZREGS) {
        return (int)id;
    }
    return -1;
}

static int h_find_preg(const LataSVEHelperDesc *d, PvirValueId id)
{
    if (id == PVIR_NO_VALUE) {
        return -1;
    }
    for (int i = 0; i < (int)A64_SVE_PVIR_NUM_PREGS; ++i) {
        if (d->p_values[i] == id) {
            return i;
        }
    }
    if (id >= A64_SVE_PVIR_P_VALUE_BASE &&
        id < A64_SVE_PVIR_P_VALUE_BASE + A64_SVE_PVIR_NUM_PREGS) {
        return (int)(id - A64_SVE_PVIR_P_VALUE_BASE);
    }
    return -1;
}

static int h_find_gpr(PvirValueId id)
{
    if (id >= A64_SVE_PVIR_GPR_VALUE_BASE &&
        id < A64_SVE_PVIR_GPR_VALUE_BASE + 32u) {
        return (int)(id - A64_SVE_PVIR_GPR_VALUE_BASE);
    }
    return -1;
}

static uint64_t mask_bits(uint16_t bits)
{
    return bits >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
}

static int64_t sext_bits(uint64_t v, uint16_t bits)
{
    if (bits >= 64) {
        return (int64_t)v;
    }
    uint64_t sign = UINT64_C(1) << (bits - 1u);
    v &= mask_bits(bits);
    return (int64_t)((v ^ sign) - sign);
}

static uint64_t read_elem_u64(const uint8_t *buf, size_t lane, uint8_t elem_bytes)
{
    const uint8_t *p = buf + lane * elem_bytes;
    uint64_t v = 0;
    for (uint8_t i = 0; i < elem_bytes; ++i) {
        v |= (uint64_t)p[i] << (i * 8u);
    }
    return v;
}

static void write_elem_u64(uint8_t *buf, size_t lane, uint8_t elem_bytes,
                           uint64_t v)
{
    uint8_t *p = buf + lane * elem_bytes;
    for (uint8_t i = 0; i < elem_bytes; ++i) {
        p[i] = (uint8_t)(v >> (i * 8u));
    }
}

static bool pbit_get(const uint8_t *pred, size_t byte_idx)
{
    return (pred[byte_idx >> 3] & (uint8_t)(1u << (byte_idx & 7u))) != 0;
}

static void pbit_set(uint8_t *pred, size_t byte_idx, bool val)
{
    if (val) {
        pred[byte_idx >> 3] |= (uint8_t)(1u << (byte_idx & 7u));
    } else {
        pred[byte_idx >> 3] &= (uint8_t)~(1u << (byte_idx & 7u));
    }
}

static bool pred_lane_active(const CPUARMState *env, const LataSVEHelperDesc *d,
                             const PvirPredicate *pred, int explicit_pg,
                             size_t lane, uint16_t ebits)
{
    uint8_t eb = (uint8_t)(ebits / 8u);
    size_t lanes = d->vl_bytes / eb;

    if (explicit_pg >= 0 && explicit_pg < (int)ARM_SVE_NUM_PREGS) {
        return pbit_get(env->sve.pregs[explicit_pg], lane * eb);
    }
    if (pred == NULL) {
        return true;
    }
    switch (pred->form) {
    case PVIR_PREDICATE_ALL_TRUE:
        return lane < lanes;
    case PVIR_PREDICATE_ALL_FALSE:
        return false;
    case PVIR_PREDICATE_PREFIX:
        return lane < pred->value.active_lanes;
    case PVIR_PREDICATE_SUFFIX:
        return lane >= (lanes > pred->value.active_lanes
                            ? lanes - pred->value.active_lanes
                            : 0u);
    case PVIR_PREDICATE_BITSET:
        if (pred->value.bitset.words != NULL &&
            (lane / 64u) < pred->value.bitset.word_count) {
            return ((pred->value.bitset.words[lane / 64u] >> (lane & 63u)) & 1u) != 0;
        }
        return false;
    case PVIR_PREDICATE_VECTOR_MASK: {
        int pg = h_find_preg(d, pred->source.id);
        if (pg >= 0 && pg < (int)ARM_SVE_NUM_PREGS) {
            return pbit_get(env->sve.pregs[pg], lane * eb);
        }
        if (pred->source.id == d->ffr_value && d->ffr_value != PVIR_NO_VALUE) {
            return pbit_get(env->sve.ffr, lane * eb);
        }
        int zr = h_find_zreg(d, pred->source.id);
        if (zr >= 0 && zr < (int)ARM_SVE_NUM_ZREGS) {
            return (read_elem_u64(env->sve.zregs[zr], lane, eb) & 1u) != 0;
        }
        return false;
    }
    default:
        return false;
    }
}

static void h_update_nzcv_from_flags(CPUARMState *env, uint32_t flags)
{
    env->pstate = (env->pstate & ~UINT32_C(0xf0000000)) | (flags & UINT32_C(0xf0000000));
    env->NF = (flags & (UINT32_C(1) << 31)) ? UINT32_C(0x80000000) : 0u;
    env->ZF = (flags & (UINT32_C(1) << 30)) ? 0u : 1u;
    env->CF = (flags & (UINT32_C(1) << 29)) ? 1u : 0u;
    env->VF = (flags & (UINT32_C(1) << 28)) ? UINT32_C(0x80000000) : 0u;
}

static void h_predtest(CPUARMState *env, size_t vl_bytes, uint8_t ebits,
                       const uint8_t *pg, const uint8_t *pn)
{
    size_t eb = ebits / 8u;
    size_t lanes = vl_bytes / eb;
    bool seen_first = false;
    bool first_active = false;
    bool any_active = false;
    bool last_active = false;
    uint32_t flags = 0;

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t b = lane * eb;
        if (!pbit_get(pg, b)) {
            continue;
        }
        bool act = pbit_get(pn, b);
        if (!seen_first) {
            first_active = act;
            seen_first = true;
        }
        if (act) {
            any_active = true;
        }
        last_active = act;
    }
    if (first_active) {
        flags |= UINT32_C(1) << 31;
    }
    if (!any_active) {
        flags |= UINT32_C(1) << 30;
    }
    if (!last_active) {
        flags |= UINT32_C(1) << 29;
    }
    h_update_nzcv_from_flags(env, flags);
}

static bool f32_isnan(uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }
static bool f64_isnan(uint64_t u)
{
    return (u & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000);
}

static float u32_to_f32(uint32_t u)
{
    union { uint32_t u; float f; } v = { u };
    return v.f;
}

static uint32_t f32_to_u32(float f)
{
    union { float f; uint32_t u; } v = { f };
    return v.u;
}

static double u64_to_f64(uint64_t u)
{
    union { uint64_t u; double d; } v = { u };
    return v.d;
}

static uint64_t f64_to_u64(double d)
{
    union { double d; uint64_t u; } v = { d };
    return v.u;
}

static uint32_t h_f32_sqrt_bits(uint32_t a)
{
    if (f32_isnan(a)) {
        return a | 0x00400000u;
    }
    if ((a & 0x7fffffffu) == 0u) {
        return a;
    }
    if (a & 0x80000000u) {
        return 0x7fc00000u;
    }
    if (a == 0x7f800000u) {
        return a;
    }
    int exp = (int)((a >> 23) & 0xffu) - 127;
    uint64_t sig = (uint64_t)(a & 0x7fffffu);
    if (exp == -127) {
        exp = -126;
        while ((sig & 0x800000u) == 0u) {
            sig <<= 1;
            exp--;
        }
    } else {
        sig |= 0x800000u;
    }
    if (exp & 1) {
        sig <<= 1;
        exp--;
    }
    int res_exp = exp / 2;
    /* 26-bit integer sqrt of (sig << 27) yields 24 mantissa bits + round + sticky. */
    uint64_t rad = sig << 27;
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << 50;
    while (bit > rad) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (rad >= root + bit) {
            rad -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    /* Root is scaled cleanly around bit 25; normalize carefully. */
    while (root < (UINT64_C(1) << 25)) {
        root <<= 1;
        rad <<= 2;
    }
    while (root >= (UINT64_C(1) << 26)) {
        root >>= 1;
    }
    uint64_t round_bit = (root >> 1) & 1u;
    uint64_t mant = (root >> 2);
    if ((root & 1u) != 0 || rad != 0u) {
        /* non-zero remainder */
        if (round_bit && ((root & 1u) || rad != 0u || (mant & 1u))) {
            mant++;
        }
    } else if (round_bit && (mant & 1u)) {
        mant++;
    }
    if (mant >= 0x1000000u) {
        mant >>= 1;
        res_exp++;
    }
    return ((uint32_t)(res_exp + 127) << 23) | (uint32_t)(mant & 0x7fffffu);
}

static uint64_t h_f64_sqrt_bits(uint64_t a)
{
    if (f64_isnan(a)) {
        return a | UINT64_C(0x0008000000000000);
    }
    if ((a & UINT64_C(0x7fffffffffffffff)) == 0u) {
        return a;
    }
    if (a & UINT64_C(0x8000000000000000)) {
        return UINT64_C(0x7ff8000000000000);
    }
    if (a == UINT64_C(0x7ff0000000000000)) {
        return a;
    }
    double x = u64_to_f64(a);
    /* Bit-accurate initial guess via IEEE half-exponent, followed by Heron steps. */
    uint64_t guess_bits = (a >> 1) + UINT64_C(0x1ff7a3bea91d9b1b);
    double y = u64_to_f64(guess_bits);
    for (int i = 0; i < 6; ++i) {
        y = 0.5 * (y + x / y);
    }
    return f64_to_u64(y);
}

static uint32_t h_f32_cmp_op(uint32_t a, uint32_t b, PvirFPKind kind)
{
    bool na = f32_isnan(a), nb = f32_isnan(b);
    if (kind == PVIR_FP_MINNM || kind == PVIR_FP_MAXNM) {
        if (na && nb) return 0x7fc00000u;
        if (na) return b;
        if (nb) return a;
    } else {
        if (na || nb) return 0x7fc00000u;
    }
    float fa = u32_to_f32(a), fb = u32_to_f32(b);
    if (fa == 0.0f && fb == 0.0f) {
        bool sa = (a >> 31) != 0, sb = (b >> 31) != 0;
        if (kind == PVIR_FP_MIN || kind == PVIR_FP_MINNM) {
            return (sa || sb) ? 0x80000000u : 0u;
        }
        return (sa && sb) ? 0x80000000u : 0u;
    }
    if (kind == PVIR_FP_MIN || kind == PVIR_FP_MINNM) {
        return (fa < fb) ? a : b;
    }
    return (fa > fb) ? a : b;
}

static uint64_t h_f64_cmp_op(uint64_t a, uint64_t b, PvirFPKind kind)
{
    bool na = f64_isnan(a), nb = f64_isnan(b);
    if (kind == PVIR_FP_MINNM || kind == PVIR_FP_MAXNM) {
        if (na && nb) return UINT64_C(0x7ff8000000000000);
        if (na) return b;
        if (nb) return a;
    } else {
        if (na || nb) return UINT64_C(0x7ff8000000000000);
    }
    double da = u64_to_f64(a), db = u64_to_f64(b);
    if (da == 0.0 && db == 0.0) {
        bool sa = (a >> 63) != 0, sb = (b >> 63) != 0;
        if (kind == PVIR_FP_MIN || kind == PVIR_FP_MINNM) {
            return (sa || sb) ? UINT64_C(0x8000000000000000) : 0u;
        }
        return (sa && sb) ? UINT64_C(0x8000000000000000) : 0u;
    }
    if (kind == PVIR_FP_MIN || kind == PVIR_FP_MINNM) {
        return (da < db) ? a : b;
    }
    return (da > db) ? a : b;
}

static uint64_t eval_int_bin(PvirOpcode op, uint16_t ebits, bool is_signed,
                             uint64_t a, uint64_t b)
{
    uint64_t m = mask_bits(ebits);
    a &= m;
    b &= m;
    switch (op) {
    case PVIR_OP_ADD:
        return (a + b) & m;
    case PVIR_OP_SUB:
        return (a - b) & m;
    case PVIR_OP_MUL:
        return (a * b) & m;
    case PVIR_OP_AND:
        return a & b;
    case PVIR_OP_OR:
        return a | b;
    case PVIR_OP_XOR:
        return a ^ b;
    case PVIR_OP_SHL:
        return (b >= ebits) ? 0u : ((a << b) & m);
    case PVIR_OP_SHR:
        return (b >= ebits) ? 0u : (a >> b);
    case PVIR_OP_SAR: {
        int64_t sa = sext_bits(a, ebits);
        unsigned sh = (b >= ebits) ? (ebits - 1u) : (unsigned)b;
        return ((uint64_t)(sa >> sh)) & m;
    }
    case PVIR_OP_MAX:
        if (is_signed) {
            return (sext_bits(a, ebits) >= sext_bits(b, ebits)) ? a : b;
        }
        return (a >= b) ? a : b;
    case PVIR_OP_MIN:
        if (is_signed) {
            return (sext_bits(a, ebits) <= sext_bits(b, ebits)) ? a : b;
        }
        return (a <= b) ? a : b;
    case PVIR_OP_DIV:
        if (b == 0u) return 0u;
        if (is_signed) {
            int64_t sa = sext_bits(a, ebits), sb = sext_bits(b, ebits);
            int64_t minv = -(int64_t)(UINT64_C(1) << (ebits - 1u));
            if (sa == minv && sb == -1) return a;
            return ((uint64_t)(sa / sb)) & m;
        }
        return (a / b) & m;
    case PVIR_OP_REM:
        if (b == 0u) return a;
        if (is_signed) {
            int64_t sa = sext_bits(a, ebits), sb = sext_bits(b, ebits);
            int64_t minv = -(int64_t)(UINT64_C(1) << (ebits - 1u));
            if (sa == minv && sb == -1) return 0u;
            return ((uint64_t)(sa % sb)) & m;
        }
        return (a % b) & m;
    default:
        return 0u;
    }
}

static bool eval_cmp_cond(PvirCompareCondition cond, bool is_float, bool is_uo,
                          uint16_t ebits, uint64_t a, uint64_t b)
{
    if (is_float) {
        if (ebits == 32) {
            bool na = f32_isnan((uint32_t)a), nb = f32_isnan((uint32_t)b);
            if (na || nb) {
                return is_uo || cond == PVIR_COMPARE_NE;
            }
            if (is_uo) return false;
            float fa = u32_to_f32((uint32_t)a), fb = u32_to_f32((uint32_t)b);
            switch (cond) {
            case PVIR_COMPARE_EQ: return fa == fb;
            case PVIR_COMPARE_NE: return fa != fb;
            case PVIR_COMPARE_LT: return fa < fb;
            case PVIR_COMPARE_LE: return fa <= fb;
            case PVIR_COMPARE_GT: return fa > fb;
            case PVIR_COMPARE_GE: return fa >= fb;
            default: return false;
            }
        } else {
            bool na = f64_isnan(a), nb = f64_isnan(b);
            if (na || nb) {
                return is_uo || cond == PVIR_COMPARE_NE;
            }
            if (is_uo) return false;
            double da = u64_to_f64(a), db = u64_to_f64(b);
            switch (cond) {
            case PVIR_COMPARE_EQ: return da == db;
            case PVIR_COMPARE_NE: return da != db;
            case PVIR_COMPARE_LT: return da < db;
            case PVIR_COMPARE_LE: return da <= db;
            case PVIR_COMPARE_GT: return da > db;
            case PVIR_COMPARE_GE: return da >= db;
            default: return false;
            }
        }
    }
    uint64_t m = mask_bits(ebits);
    a &= m;
    b &= m;
    switch (cond) {
    case PVIR_COMPARE_EQ: return a == b;
    case PVIR_COMPARE_NE: return a != b;
    case PVIR_COMPARE_LT: return sext_bits(a, ebits) < sext_bits(b, ebits);
    case PVIR_COMPARE_LE: return sext_bits(a, ebits) <= sext_bits(b, ebits);
    case PVIR_COMPARE_GT: return sext_bits(a, ebits) > sext_bits(b, ebits);
    case PVIR_COMPARE_GE: return sext_bits(a, ebits) >= sext_bits(b, ebits);
    case PVIR_COMPARE_LTU: return a < b;
    case PVIR_COMPARE_LEU: return a <= b;
    case PVIR_COMPARE_GTU: return a > b;
    case PVIR_COMPARE_GEU: return a >= b;
    default: return false;
    }
}

static uint64_t eval_fp_arith_elem(PvirFPKind kind, uint16_t ebits, uint64_t a,
                                   uint64_t b)
{
    if (ebits == 32) {
        uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
        float fa = u32_to_f32(ua), fb = u32_to_f32(ub);
        switch (kind) {
        case PVIR_FP_ADD: return f32_to_u32(fa + fb);
        case PVIR_FP_SUB: return f32_to_u32(fa - fb);
        case PVIR_FP_MUL: return f32_to_u32(fa * fb);
        case PVIR_FP_DIV: return f32_to_u32(fa / fb);
        case PVIR_FP_SQRT: return h_f32_sqrt_bits(ua);
        case PVIR_FP_MIN:
        case PVIR_FP_MAX:
        case PVIR_FP_MINNM:
        case PVIR_FP_MAXNM:
            return h_f32_cmp_op(ua, ub, kind);
        case PVIR_FP_RECPE:
            return f32_isnan(ua) ? (ua | 0x00400000u) : f32_to_u32(1.0f / fa);
        case PVIR_FP_RSQRTE:
            return f32_to_u32(1.0f / u32_to_f32(h_f32_sqrt_bits(ua)));
        case PVIR_FP_RECPS:
            if (f32_isnan(ua) || f32_isnan(ub)) {
                return f32_to_u32(2.0f - fa * fb);
            } else {
                float p = fa * fb;
                if (f32_isnan(f32_to_u32(p))) {
                    return f32_to_u32(2.0f);
                }
                return f32_to_u32(2.0f - p);
            }
        case PVIR_FP_RSQRTS:
            if (f32_isnan(ua) || f32_isnan(ub)) {
                return f32_to_u32((3.0f - fa * fb) * 0.5f);
            } else {
                float p = fa * fb;
                if (f32_isnan(f32_to_u32(p))) {
                    return f32_to_u32(1.5f);
                }
                return f32_to_u32((3.0f - p) * 0.5f);
            }
        default:
            return ua;
        }
    } else {
        double da = u64_to_f64(a), db = u64_to_f64(b);
        switch (kind) {
        case PVIR_FP_ADD: return f64_to_u64(da + db);
        case PVIR_FP_SUB: return f64_to_u64(da - db);
        case PVIR_FP_MUL: return f64_to_u64(da * db);
        case PVIR_FP_DIV: return f64_to_u64(da / db);
        case PVIR_FP_SQRT: return h_f64_sqrt_bits(a);
        case PVIR_FP_MIN:
        case PVIR_FP_MAX:
        case PVIR_FP_MINNM:
        case PVIR_FP_MAXNM:
            return h_f64_cmp_op(a, b, kind);
        case PVIR_FP_RECPE:
            return f64_isnan(a) ? (a | UINT64_C(0x0008000000000000))
                                : f64_to_u64(1.0 / da);
        case PVIR_FP_RSQRTE:
            return f64_to_u64(1.0 / u64_to_f64(h_f64_sqrt_bits(a)));
        case PVIR_FP_RECPS:
            if (f64_isnan(a) || f64_isnan(b)) {
                return f64_to_u64(2.0 - da * db);
            } else {
                double p = da * db;
                if (f64_isnan(f64_to_u64(p))) {
                    return f64_to_u64(2.0);
                }
                return f64_to_u64(2.0 - p);
            }
        case PVIR_FP_RSQRTS:
            if (f64_isnan(a) || f64_isnan(b)) {
                return f64_to_u64((3.0 - da * db) * 0.5);
            } else {
                double p = da * db;
                if (f64_isnan(f64_to_u64(p))) {
                    return f64_to_u64(1.5);
                }
                return f64_to_u64((3.0 - p) * 0.5);
            }
        default:
            return a;
        }
    }
}

static void exec_predicate_constant(CPUARMState *env, size_t vl_bytes,
                                    const PvirPredicateGeneratorOperation *g)
{
    size_t elem_bytes = g->lane_type.element_bits / 8u;
    size_t lanes = vl_bytes / elem_bytes;
    size_t active_lanes = g->active_lanes;
    uint32_t pd = g->state_id - A64_SVE_PVIR_P_STATE_BASE;
    uint8_t *dst = env->sve.pregs[pd];

    if (g->form == PVIR_PREDICATE_ALL_TRUE) {
        active_lanes = lanes;
    } else if (g->form == PVIR_PREDICATE_ALL_FALSE) {
        active_lanes = 0;
    } else if (g->form == PVIR_PREDICATE_SUFFIX) {
        active_lanes = lanes - g->active_lanes;
    }
    memset(dst, 0, vl_bytes / 8u);
    for (size_t lane = 0; lane < lanes; ++lane) {
        bool act = (g->form == PVIR_PREDICATE_SUFFIX) ? (lane >= active_lanes)
                                                      : (lane < active_lanes);
        if (act) {
            pbit_set(dst, lane * elem_bytes, true);
        }
    }
    if (g->status_state.present) {
        uint32_t flags = 0;
        if (active_lanes != 0) flags |= UINT32_C(1) << 31;
        else flags |= UINT32_C(1) << 30;
        if (active_lanes != lanes) flags |= UINT32_C(1) << 29;
        h_update_nzcv_from_flags(env, flags);
    }
}

static void exec_while(CPUARMState *env, size_t vl_bytes,
                       const LataSVEHelperDesc *d)
{
    const PvirPredicateGeneratorOperation *g = &d->pvir.operation.predicate_generator;
    uint16_t ebits = g->lane_type.element_bits;
    uint8_t eb = (uint8_t)(ebits / 8u);
    size_t lanes = vl_bytes / eb;
    unsigned pd = g->state_id - A64_SVE_PVIR_P_STATE_BASE;
    int rn = d->has_source ? (int)d->source.arg.f_sve_whilelo.rn
                           : h_find_gpr(g->scalar_start);
    int rm = d->has_source ? (int)d->source.arg.f_sve_whilelo.rm
                           : h_find_gpr(g->scalar_limit);
    uint16_t sbits = d->has_source ? d->source.arg.f_sve_whilelo.scalar_bits
                                   : g->scalar_bits;
    uint64_t smask = mask_bits(sbits);
    uint64_t start = (rn >= 0 && rn < 31) ? (env->xregs[rn] & smask) : 0u;
    uint64_t limit = (rm >= 0 && rm < 31) ? (env->xregs[rm] & smask) : 0u;
    uint8_t *dst = env->sve.pregs[pd];
    uint8_t all_true[ARM_SVE_MAX_PRED_BYTES];

    memset(dst, 0, vl_bytes / 8u);
    memset(all_true, 0xff, vl_bytes / 8u);
    if (g->conflict_check) {
        /* WHILERW/WHILEWR pointer conflict check */
        for (size_t l = 0; l < lanes; ++l) {
            uint64_t a_lo = start + l * eb;
            uint64_t a_hi = a_lo + eb;
            bool ok = true;
            for (size_t k = 0; k <= l; ++k) {
                uint64_t b_lo = limit + k * eb;
                uint64_t b_hi = b_lo + eb;
                if (g->conflict_write_first ? (a_lo < b_hi && b_lo < a_hi)
                                            : (l > 0 && k < l && a_lo < b_hi && b_lo < a_hi)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
            pbit_set(dst, l * eb, true);
        }
    } else {
        for (size_t l = 0; l < lanes; ++l) {
            uint64_t cur = (start + l) & smask;
            bool cond = eval_cmp_cond(g->compare_condition, false, false, sbits,
                                      cur, limit);
            if (!cond) break;
            pbit_set(dst, l * eb, true);
        }
    }
    h_predtest(env, vl_bytes, (uint8_t)ebits, all_true, dst);
}



static uint64_t exec_fault_or_contiguous_mem(CPUARMState *env,
                                             const LataSVEHelperDesc *d)
{
    const PvirInstruction *ins = &d->pvir;
    size_t vl_bytes = d->vl_bytes;
    bool is_first = (ins->effects.fault.behavior == PVIR_FAULT_FIRST_FAULT);
    bool is_nonfault = (ins->effects.fault.behavior == PVIR_FAULT_NONFAULTING);
    const PvirVectorOperation *op = &ins->operation.vector;
    uint8_t elem_bits = op->type.element_bits;
    uint8_t mem_bits = (uint8_t)op->memory_element_bits;
    uint8_t eb = elem_bits / 8u;
    uint8_t mb = mem_bits / 8u;
    size_t lanes = vl_bytes / eb;
    bool is_load = (ins->opcode == PVIR_OP_LOAD);
    bool is_signed = (op->type.signedness == PVIR_SIGNED);
    int zt = h_find_zreg(d, is_load ? op->destination : op->operands[0]);
    int pg = h_find_preg(d, op->predicate.source.id);
    int rn = (op->memory_address.base_kind == PVIR_MEMORY_BASE_STACK_POINTER)
                 ? 31
                 : h_find_gpr(op->memory_address.base_value);
    int rm = 31;
    bool has_idx = false;
    bool base_is_sp = (rn == 31);
    int64_t disp = op->memory_address.displacement;

    if (d->has_source) {
        if (is_first) {
            zt = d->source.arg.f_sve_fault_load.zt;
            pg = d->source.arg.f_sve_fault_load.pg;
            rn = d->source.arg.f_sve_fault_load.rn;
            rm = d->source.arg.f_sve_fault_load.rm;
            has_idx = d->source.arg.f_sve_fault_load.has_index;
            base_is_sp = d->source.arg.f_sve_fault_load.base_is_sp;
        } else if (is_nonfault) {
            zt = d->source.arg.f_sve_nonfault_load.zt;
            pg = d->source.arg.f_sve_nonfault_load.pg;
            rn = d->source.arg.f_sve_nonfault_load.rn;
            base_is_sp = d->source.arg.f_sve_nonfault_load.base_is_sp;
        } else {
            zt = d->source.arg.f_sve_contiguous.zt;
            pg = d->source.arg.f_sve_contiguous.pg;
            rn = d->source.arg.f_sve_contiguous.rn;
            base_is_sp = d->source.arg.f_sve_contiguous.base_is_sp;
        }
    }

    uint64_t base = (base_is_sp || rn < 31) ? env->xregs[base_is_sp ? 31 : rn] : 0u;
    if (has_idx && rm < 31) {
        base += env->xregs[rm] * mb;
    }
    base += (uint64_t)disp;
    if (d->tbid) {
        base &= UINT64_C(0x00ffffffffffffff);
    }

    if (!is_first && !is_nonfault) {
        uint8_t staged[ARM_SVE_MAX_VL_BYTES];
        memset(staged, 0, vl_bytes);
        for (size_t lane = 0; lane < lanes; ++lane) {
            if (!pbit_get(env->sve.pregs[pg], lane * eb)) {
                continue;
            }
            uint64_t ea = base + lane * mb;
            if (is_load) {
                uint64_t val = 0;
                LataGuestMemStatus st =
                    lata_guest_mem_load(env, ea, mb, is_signed, &val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, ea, 0, (uint32_t)st);
                    return 1;
                }
                write_elem_u64(staged, lane, eb, val);
            } else {
                uint64_t val = read_elem_u64(env->sve.zregs[zt], lane, eb);
                LataGuestMemStatus st = lata_guest_mem_store(env, ea, mb, val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, ea, 1, (uint32_t)st);
                    return 1;
                }
            }
        }
        if (is_load) {
            memcpy(env->sve.zregs[zt], staged, vl_bytes);
        }
        return 0;
    }

    /* LDFF1* / LDNF1* exact first-fault and non-faulting semantics */
    uint8_t staged[ARM_SVE_MAX_VL_BYTES];
    uint8_t new_ffr[ARM_SVE_MAX_PRED_BYTES];
    bool seen_active_ok = false;
    bool faulted = false;
    memset(staged, 0, vl_bytes);
    memcpy(new_ffr, env->sve.ffr, vl_bytes / 8u);

    for (size_t lane = 0; lane < lanes; ++lane) {
        size_t b = lane * eb;
        if (!pbit_get(env->sve.pregs[pg], b)) {
            if (faulted) {
                for (size_t k = 0; k < eb; ++k) pbit_set(new_ffr, b + k, false);
            }
            continue;
        }
        if (!pbit_get(new_ffr, b) || faulted) {
            faulted = true;
            for (size_t k = 0; k < eb; ++k) pbit_set(new_ffr, b + k, false);
            continue;
        }
        uint64_t ea = base + lane * mb;
        uint64_t val = 0;
        LataGuestMemStatus st = lata_guest_mem_load(env, ea, mb, is_signed, &val);
        if (st != LATA_MEM_OK) {
            if (is_first && !seen_active_ok) {
                /* First active element fault on LDFF1 is synchronous and preserves Z/FFR. */
                lata_guest_mem_raise_fault(env, d->pc_curr, ea, 0, (uint32_t)st);
                return 1;
            }
            faulted = true;
            for (size_t k = 0; k < eb; ++k) pbit_set(new_ffr, b + k, false);
            continue;
        }
        seen_active_ok = true;
        write_elem_u64(staged, lane, eb, val);
    }
    memcpy(env->sve.zregs[zt], staged, vl_bytes);
    memcpy(env->sve.ffr, new_ffr, vl_bytes / 8u);
    return 0;
}

static uint64_t exec_mem_ext(CPUARMState *env, const LataSVEHelperDesc *d)
{
    const arg_sve_mem_ext *m = &d->source.arg.f_sve_mem_ext;
    size_t vl_bytes = d->vl_bytes;
    uint8_t eb = m->element_bits / 8u;
    uint8_t mb = m->memory_bits / 8u;
    size_t lanes = vl_bytes / eb;
    uint8_t staged[4][ARM_SVE_MAX_VL_BYTES];

    if (m->kind == SVE_MEM_REP_IMM || m->kind == SVE_MEM_REP_QUAD) {
        uint64_t addr = (m->base_is_sp || m->rn < 31) ? env->xregs[m->base_is_sp ? 31 : m->rn] : 0u;
        addr += (uint64_t)(int64_t)m->imm;
        if (d->tbid) addr &= UINT64_C(0x00ffffffffffffff);
        memset(staged[0], 0, vl_bytes);
        if (m->kind == SVE_MEM_REP_IMM) {
            bool any = false;
            for (size_t l = 0; l < lanes; ++l) {
                if (pbit_get(env->sve.pregs[m->pg], l * eb)) { any = true; break; }
            }
            if (any) {
                uint64_t val = 0;
                LataGuestMemStatus st = lata_guest_mem_load(env, addr, mb, m->sign_extend, &val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, addr, 0, (uint32_t)st);
                    return 1;
                }
                for (size_t l = 0; l < lanes; ++l) {
                    if (pbit_get(env->sve.pregs[m->pg], l * eb)) {
                        write_elem_u64(staged[0], l, eb, val);
                    }
                }
            }
        } else {
            size_t qlanes = 16u / mb;
            for (size_t ql = 0; ql < qlanes; ++ql) {
                if (!pbit_get(env->sve.pregs[m->pg], ql * eb)) continue;
                uint64_t val = 0;
                LataGuestMemStatus st = lata_guest_mem_load(env, addr + ql * eb, mb, false, &val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, addr + ql * eb, 0, (uint32_t)st);
                    return 1;
                }
                for (size_t qoff = 0; qoff < vl_bytes; qoff += 16u) {
                    write_elem_u64(staged[0] + qoff, ql, eb, val);
                }
            }
        }
        memcpy(env->sve.zregs[m->zt], staged[0], vl_bytes);
        return 0;
    }

    if (m->kind == SVE_MEM_GATHER_VEC_BASE || m->kind == SVE_MEM_GATHER_VEC_OFFSET) {
        int vreg = (m->kind == SVE_MEM_GATHER_VEC_BASE) ? m->rn : m->rm;
        uint64_t base = (m->kind == SVE_MEM_GATHER_VEC_BASE)
                            ? (uint64_t)(int64_t)m->imm
                            : ((m->base_is_sp || m->rn < 31) ? env->xregs[m->base_is_sp ? 31 : m->rn] : 0u);
        unsigned sh = (mb == 1) ? 0u : (mb == 2 ? 1u : (mb == 4 ? 2u : 3u));
        if (m->is_load) memset(staged[0], 0, vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            if (!pbit_get(env->sve.pregs[m->pg], l * eb)) continue;
            uint64_t ea;
            if (m->kind == SVE_MEM_GATHER_VEC_BASE) {
                ea = base + read_elem_u64(env->sve.zregs[vreg], l, eb);
            } else {
                uint8_t ob = (eb == 4 || !m->offset_is_64) ? 4u : 8u;
                uint64_t off = read_elem_u64(env->sve.zregs[vreg], l, ob);
                if (m->offset_is_signed && ob == 4) {
                    off = (uint64_t)(int64_t)(int32_t)off;
                }
                if (m->scaled) off <<= sh;
                ea = base + off;
            }
            if (d->tbid) ea &= UINT64_C(0x00ffffffffffffff);
            if (m->is_load) {
                uint64_t val = 0;
                LataGuestMemStatus st = lata_guest_mem_load(env, ea, mb, m->sign_extend, &val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, ea, 0, (uint32_t)st);
                    return 1;
                }
                write_elem_u64(staged[0], l, eb, val);
            } else {
                uint64_t val = read_elem_u64(env->sve.zregs[m->zt], l, eb);
                LataGuestMemStatus st = lata_guest_mem_store(env, ea, mb, val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, ea, 1, (uint32_t)st);
                    return 1;
                }
            }
        }
        if (m->is_load) memcpy(env->sve.zregs[m->zt], staged[0], vl_bytes);
        return 0;
    }

    uint64_t base = (m->base_is_sp || m->rn < 31) ? env->xregs[m->base_is_sp ? 31 : m->rn] : 0u;
    if (m->rm < 31) {
        unsigned sh = (mb == 1) ? 0u : (mb == 2 ? 1u : (mb == 4 ? 2u : 3u));
        base += m->scaled ? (env->xregs[m->rm] << sh) : env->xregs[m->rm];
    } else if (m->imm != 0) {
        base += (uint64_t)((int64_t)m->imm * (int64_t)m->nregs * (int64_t)vl_bytes);
    }
    if (d->tbid) base &= UINT64_C(0x00ffffffffffffff);
    if (m->is_load) {
        for (uint8_t r = 0; r < m->nregs && r < 4; ++r) {
            memset(staged[r], 0, vl_bytes);
        }
    }
    for (size_t l = 0; l < lanes; ++l) {
        if (!pbit_get(env->sve.pregs[m->pg], l * eb)) continue;
        for (uint8_t r = 0; r < m->nregs && r < 4; ++r) {
            uint64_t ea = base + (l * m->nregs + r) * mb;
            if (m->is_load) {
                uint64_t val = 0;
                LataGuestMemStatus st = lata_guest_mem_load(env, ea, mb, m->sign_extend, &val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, ea, 0, (uint32_t)st);
                    return 1;
                }
                write_elem_u64(staged[r], l, eb, val);
            } else {
                uint64_t val = read_elem_u64(env->sve.zregs[(m->zt + r) & 31u], l, eb);
                LataGuestMemStatus st = lata_guest_mem_store(env, ea, mb, val);
                if (st != LATA_MEM_OK) {
                    lata_guest_mem_raise_fault(env, d->pc_curr, ea, 1, (uint32_t)st);
                    return 1;
                }
            }
        }
    }
    if (m->is_load) {
        for (uint8_t r = 0; r < m->nregs && r < 4; ++r) {
            memcpy(env->sve.zregs[(m->zt + r) & 31u], staged[r], vl_bytes);
        }
    }
    return 0;
}

static bool instruction_writes_nzcv(const PvirInstruction *ins,
                                    const LataSVEHelperDesc *d)
{
    if (d != NULL && d->has_source) {
        switch (d->source.insn_type) {
        case AARCH64_A64_SVE_PTRUE:
            return d->source.arg.f_sve_ptrue.setflags;
        case AARCH64_A64_SVE_RDFFR:
            return d->source.arg.f_sve_ffr.setflags;
        case AARCH64_A64_SVE_PTEST:
            return true;
        case AARCH64_A64_SVE_BRK:
        case AARCH64_A64_SVE_PFIRST:
        case AARCH64_A64_SVE_PNEXT:
        case AARCH64_A64_SVE_PRED_LOGICAL:
            return d->source.arg.f_sve_pred_op.setflags;
        case AARCH64_A64_SVE_WHILELO:
        case AARCH64_A64_SVE_WHILE:
        case AARCH64_A64_SVE_FCMP:
        case AARCH64_A64_SVE_ICMP:
            return true;
        default:
            break;
        }
    }
    if (ins->opcode == PVIR_OP_PREDICATE_GENERATE) {
        return ins->operation.predicate_generator.status_state.present;
    }
    return false;
}

LataSVEHelperStatus lata_sve_exec_helper_status(void *env_ptr,
                                                const LataSVEHelperDesc *d)
{
    CPUARMState *env = (CPUARMState *)env_ptr;
    const PvirInstruction *ins;
    size_t vl_bytes;

    if (env == NULL || d == NULL) {
        return LATA_SVE_HELPER_INTERNAL;
    }
    ins = &d->pvir;
    vl_bytes = d->vl_bytes;

    if (d->has_source) {
        switch (d->source.insn_type) {
        case AARCH64_A64_SVE_LD1B:
        case AARCH64_A64_SVE_LD1H:
        case AARCH64_A64_SVE_LD1W:
        case AARCH64_A64_SVE_LD1D:
        case AARCH64_A64_SVE_ST1B:
        case AARCH64_A64_SVE_ST1H:
        case AARCH64_A64_SVE_ST1W:
        case AARCH64_A64_SVE_ST1D:
        case AARCH64_A64_SVE_LD1SB:
        case AARCH64_A64_SVE_LD1SH:
        case AARCH64_A64_SVE_LD1SW:
        case AARCH64_A64_SVE_LDFF1B:
        case AARCH64_A64_SVE_LDFF1H:
        case AARCH64_A64_SVE_LDFF1W:
        case AARCH64_A64_SVE_LDFF1D:
        case AARCH64_A64_SVE_LDFF1SB:
        case AARCH64_A64_SVE_LDFF1SH:
        case AARCH64_A64_SVE_LDFF1SW:
        case AARCH64_A64_SVE_LDNF1B:
        case AARCH64_A64_SVE_LDNF1H:
        case AARCH64_A64_SVE_LDNF1W:
        case AARCH64_A64_SVE_LDNF1D:
        case AARCH64_A64_SVE_LDNF1SB:
        case AARCH64_A64_SVE_LDNF1SH:
        case AARCH64_A64_SVE_LDNF1SW:
            return exec_fault_or_contiguous_mem(env, d)
                   ? LATA_SVE_HELPER_FAULT : LATA_SVE_HELPER_OK;
        case AARCH64_A64_SVE_LD1_SCALAR:
        case AARCH64_A64_SVE_ST1_SCALAR:
        case AARCH64_A64_SVE_LDN:
        case AARCH64_A64_SVE_STN:
        case AARCH64_A64_SVE_LD1R:
        case AARCH64_A64_SVE_LD1RQ:
        case AARCH64_A64_SVE_GATHER_LD:
        case AARCH64_A64_SVE_SCATTER_ST:
        case AARCH64_A64_SVE_LDNT1:
        case AARCH64_A64_SVE_STNT1:
            return exec_mem_ext(env, d) ? LATA_SVE_HELPER_FAULT
                                        : LATA_SVE_HELPER_OK;
        case AARCH64_A64_SVE_SETFFR:
            memset(env->sve.ffr, 0xff, vl_bytes / 8u);
            return LATA_SVE_HELPER_OK;
        case AARCH64_A64_SVE_WRFFR:
            memcpy(env->sve.ffr, env->sve.pregs[d->source.arg.f_sve_ffr.pn],
                   vl_bytes / 8u);
            return LATA_SVE_HELPER_OK;
        case AARCH64_A64_SVE_RDFFR: {
            const arg_sve_ffr *f = &d->source.arg.f_sve_ffr;
            size_t pb = vl_bytes / 8u;
            for (size_t i = 0; i < pb; ++i) {
                env->sve.pregs[f->pd][i] =
                    f->predicated ? (env->sve.ffr[i] & env->sve.pregs[f->pg][i])
                                  : env->sve.ffr[i];
            }
            if (f->setflags && f->predicated) {
                h_predtest(env, vl_bytes, 8, env->sve.pregs[f->pg],
                           env->sve.pregs[f->pd]);
            }
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_PTEST: {
            const arg_sve_pred_op *p = &d->source.arg.f_sve_pred_op;
            h_predtest(env, vl_bytes, 8, env->sve.pregs[p->pg],
                       env->sve.pregs[p->pn]);
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_BRK:
        case AARCH64_A64_SVE_PFIRST:
        case AARCH64_A64_SVE_PNEXT:
        case AARCH64_A64_SVE_PRED_LOGICAL: {
            const arg_sve_pred_op *p = &d->source.arg.f_sve_pred_op;
            size_t pb = vl_bytes / 8u;
            uint8_t out[ARM_SVE_MAX_PRED_BYTES];
            memset(out, 0, pb);
            if (p->subop == SVE_PRED_OP_BRKA || p->subop == SVE_PRED_OP_BRKB) {
                bool after = (p->subop == SVE_PRED_OP_BRKB);
                bool stopped = false;
                if (p->merge) memcpy(out, env->sve.pregs[p->pd], pb);
                for (size_t b = 0; b < vl_bytes; ++b) {
                    if (!pbit_get(env->sve.pregs[p->pg], b)) continue;
                    bool nv = false;
                    if (!stopped) {
                        bool srcb = pbit_get(env->sve.pregs[p->pn], b);
                        if (srcb) {
                            nv = !after;
                            stopped = true;
                        } else {
                            nv = true;
                        }
                    }
                    pbit_set(out, b, nv);
                }
            } else if (p->subop == SVE_PRED_OP_PFIRST || p->subop == SVE_PRED_OP_PNEXT) {
                size_t step = (p->subop == SVE_PRED_OP_PFIRST) ? 1u : (p->element_bits / 8u);
                memcpy(out, env->sve.pregs[p->pn], pb);
                if (p->subop == SVE_PRED_OP_PFIRST) {
                    for (size_t b = 0; b < vl_bytes; ++b) {
                        if (pbit_get(env->sve.pregs[p->pg], b)) {
                            pbit_set(out, b, true);
                            break;
                        }
                    }
                } else {
                    size_t cur = 0;
                    bool have_prev = false;
                    for (size_t b = 0; b < vl_bytes; b += step) {
                        if (pbit_get(env->sve.pregs[p->pn], b)) {
                            cur = b + step;
                            have_prev = true;
                            pbit_set(out, b, false);
                            break;
                        }
                    }
                    if (!have_prev) cur = 0;
                    for (size_t b = cur; b < vl_bytes; b += step) {
                        if (pbit_get(env->sve.pregs[p->pg], b)) {
                            pbit_set(out, b, true);
                            break;
                        }
                    }
                }
            } else {
                for (size_t i = 0; i < pb; ++i) {
                    uint8_t g = env->sve.pregs[p->pg][i];
                    uint8_t a = env->sve.pregs[p->pn][i];
                    uint8_t b = env->sve.pregs[p->pm][i];
                    uint8_t prev = env->sve.pregs[p->pd][i];
                    uint8_t v = 0;
                    switch (p->subop) {
                    case SVE_PRED_OP_AND: v = (a & b) & g; break;
                    case SVE_PRED_OP_ORR: v = (a | b) & g; break;
                    case SVE_PRED_OP_EOR: v = (a ^ b) & g; break;
                    case SVE_PRED_OP_BIC: v = (a & ~b) & g; break;
                    case SVE_PRED_OP_ORN: v = (a | ~b) & g; break;
                    case SVE_PRED_OP_NOR: v = ~(a | b) & g; break;
                    case SVE_PRED_OP_NAND: v = ~(a & b) & g; break;
                    case SVE_PRED_OP_SEL: v = (a & g) | (b & ~g); break;
                    default: break;
                    }
                    out[i] = p->merge ? ((v & g) | (prev & ~g)) : v;
                }
            }
            memcpy(env->sve.pregs[p->pd], out, pb);
            if (p->setflags) {
                h_predtest(env, vl_bytes,
                           (p->subop == SVE_PRED_OP_PNEXT) ? p->element_bits : 8u,
                           env->sve.pregs[p->pg], env->sve.pregs[p->pd]);
            }
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_CNTP:
        case AARCH64_A64_SVE_CNT_SCALAR: {
            const arg_sve_cnt *c = &d->source.arg.f_sve_cnt;
            uint8_t eb = c->element_bits / 8u;
            size_t lanes = vl_bytes / eb;
            uint64_t cnt = 0;
            if (c->subop == SVE_CNT_SUBOP_CNTP) {
                for (size_t l = 0; l < lanes; ++l) {
                    if (pbit_get(env->sve.pregs[c->pg], l * eb) &&
                        pbit_get(env->sve.pregs[c->pn], l * eb)) {
                        cnt++;
                    }
                }
                if (c->rd < 31) env->xregs[c->rd] = cnt;
            } else {
                uint64_t pat_lanes = 0;
                if (c->pattern <= 4) {
                    pat_lanes = (c->pattern == 0) ? 1u : c->pattern;
                } else if (c->pattern == 31) {
                    pat_lanes = lanes;
                } else if (c->pattern == 5) pat_lanes = 5;
                else if (c->pattern == 6) pat_lanes = 6;
                else if (c->pattern == 7) pat_lanes = 7;
                else if (c->pattern == 8) pat_lanes = 8;
                else if (c->pattern == 9) pat_lanes = 16;
                else if (c->pattern == 10) pat_lanes = 32;
                else if (c->pattern == 11) pat_lanes = 64;
                else if (c->pattern == 12) pat_lanes = 128;
                else if (c->pattern == 13) pat_lanes = 256;
                else if (c->pattern == 29) pat_lanes = lanes - (lanes % 4u);
                else if (c->pattern == 30) pat_lanes = lanes - (lanes % 3u);
                else pat_lanes = 0;
                if (pat_lanes > lanes) pat_lanes = 0;
                cnt = pat_lanes * c->imm_mul;
                if (c->subop == SVE_CNT_SUBOP_CNT) {
                    if (c->rd < 31) env->xregs[c->rd] = cnt;
                } else if (c->rd < 31) {
                    uint64_t src = env->xregs[c->rd];
                    if (c->subop == SVE_CNT_SUBOP_INC) {
                        env->xregs[c->rd] = c->is_64 ? (src + cnt) : ((uint32_t)src + (uint32_t)cnt);
                    } else if (c->subop == SVE_CNT_SUBOP_DEC) {
                        env->xregs[c->rd] = c->is_64 ? (src - cnt) : ((uint32_t)src - (uint32_t)cnt);
                    } else if (c->subop == SVE_CNT_SUBOP_SQINC || c->subop == SVE_CNT_SUBOP_SQDEC) {
                        int64_t delta = (c->subop == SVE_CNT_SUBOP_SQDEC) ? -(int64_t)cnt : (int64_t)cnt;
                        if (c->is_64) {
                            int64_t s = (int64_t)src;
                            int64_t r = (delta >= 0 && s > INT64_MAX - delta) ? INT64_MAX
                                      : ((delta < 0 && s < INT64_MIN - delta) ? INT64_MIN : s + delta);
                            env->xregs[c->rd] = (uint64_t)r;
                        } else {
                            int64_t s = (int32_t)src;
                            int64_t r = s + delta;
                            if (r > INT32_MAX) r = INT32_MAX;
                            if (r < INT32_MIN) r = INT32_MIN;
                            env->xregs[c->rd] = c->is_signed ? (uint64_t)r : (uint64_t)(uint32_t)r;
                        }
                    } else {
                        bool dec = (c->subop == SVE_CNT_SUBOP_UQDEC);
                        if (c->is_64) {
                            env->xregs[c->rd] = dec ? (src >= cnt ? src - cnt : 0u)
                                                    : (src > UINT64_MAX - cnt ? UINT64_MAX : src + cnt);
                        } else {
                            uint64_t s32 = (uint32_t)src;
                            uint64_t r = dec ? (s32 >= cnt ? s32 - cnt : 0u)
                                             : (s32 + cnt > UINT32_MAX ? UINT32_MAX : s32 + cnt);
                            env->xregs[c->rd] = r;
                        }
                    }
                }
            }
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_LAST: {
            const arg_sve_last *la = &d->source.arg.f_sve_last;
            uint8_t eb = la->element_bits / 8u;
            size_t lanes = vl_bytes / eb;
            int found = -1;
            for (int l = (int)lanes - 1; l >= 0; --l) {
                if (pbit_get(env->sve.pregs[la->pg], (size_t)l * eb)) {
                    found = l;
                    break;
                }
            }
            uint64_t val = 0;
            bool write_it = true;
            if (found >= 0) {
                size_t pick = la->is_after ? ((size_t)(found + 1) % lanes) : (size_t)found;
                val = read_elem_u64(env->sve.zregs[la->zm], pick, eb);
            } else if (la->is_cond) {
                write_it = false;
            } else {
                size_t pick = la->is_after ? 0u : (lanes - 1u);
                val = read_elem_u64(env->sve.zregs[la->zm], pick, eb);
            }
            if (write_it) {
                if (la->dst_is_vector) {
                    write_elem_u64(env->sve.zregs[la->rd_zd], 0, eb, val);
                    if (eb < 8u) {
                        memset(env->sve.zregs[la->rd_zd] + eb, 0, vl_bytes - eb);
                    } else {
                        memset(env->sve.zregs[la->rd_zd] + 8u, 0, vl_bytes - 8u);
                    }
                } else if (la->rd_zd < 31) {
                    env->xregs[la->rd_zd] = val & mask_bits(eb <= 4 ? 32 : 64);
                }
            }
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_DUP: {
            const arg_sve_dup *du = &d->source.arg.f_sve_dup;
            uint8_t eb = du->element_bits / 8u;
            size_t lanes = vl_bytes / eb;
            uint64_t v = (du->rn < 31) ? env->xregs[du->rn] : 0u;
            for (size_t l = 0; l < lanes; ++l) {
                write_elem_u64(env->sve.zregs[du->zd], l, eb, v);
            }
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_FABS:
        case AARCH64_A64_SVE_FNEG: {
            const arg_sve_fp_unary *u = (d->source.insn_type == AARCH64_A64_SVE_FABS)
                                            ? &d->source.arg.f_sve_fabs
                                            : &d->source.arg.f_sve_fneg;
            uint16_t ebits = u->element_bits;
            uint8_t eb = (uint8_t)(ebits / 8u);
            size_t lanes = vl_bytes / eb;
            uint64_t sbit = UINT64_C(1) << (ebits - 1u);
            uint8_t out[ARM_SVE_MAX_VL_BYTES];
            memcpy(out, env->sve.zregs[u->zd], vl_bytes);
            for (size_t l = 0; l < lanes; ++l) {
                if (!pbit_get(env->sve.pregs[u->pg], l * eb)) continue;
                uint64_t v = read_elem_u64(env->sve.zregs[u->zm], l, eb);
                v = (d->source.insn_type == AARCH64_A64_SVE_FABS) ? (v & ~sbit) : (v ^ sbit);
                write_elem_u64(out, l, eb, v);
            }
            memcpy(env->sve.zregs[u->zd], out, vl_bytes);
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_SHIFT_IMM: {
            const arg_sve_ext_op *ex = &d->source.arg.f_sve_ext_op;
            uint16_t ebits = ex->src_bits;
            uint8_t eb = (uint8_t)(ebits / 8u);
            size_t lanes = vl_bytes / eb;
            unsigned sh = (unsigned)ex->imm;
            uint8_t out[ARM_SVE_MAX_VL_BYTES];
            memcpy(out, env->sve.zregs[ex->zd_pd], vl_bytes);
            for (size_t l = 0; l < lanes; ++l) {
                if (ex->predicated && !pbit_get(env->sve.pregs[ex->pg], l * eb)) {
                    continue;
                }
                uint64_t v = read_elem_u64(env->sve.zregs[ex->zn], l, eb);
                uint64_t r = 0;
                if (ex->subop == SVE_SHIFT_IMM_LSL) {
                    r = (sh >= ebits) ? 0u : ((v << sh) & mask_bits(ebits));
                } else if (ex->subop == SVE_SHIFT_IMM_LSR) {
                    r = (sh >= ebits) ? 0u : (v >> sh);
                } else {
                    int64_t sv = sext_bits(v, ebits);
                    if (ex->subop == SVE_SHIFT_IMM_ASRD && sv < 0 && sh > 0) {
                        uint64_t bias = (sh >= ebits) ? mask_bits(ebits)
                                                      : ((UINT64_C(1) << sh) - 1u);
                        sv += (int64_t)bias;
                    }
                    unsigned eff = (sh >= ebits) ? (ebits - 1u) : sh;
                    r = ((uint64_t)(sv >> eff)) & mask_bits(ebits);
                }
                write_elem_u64(out, l, eb, r);
            }
            memcpy(env->sve.zregs[ex->zd_pd], out, vl_bytes);
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_INT_UNARY: {
            const arg_sve_general *g = &d->source.arg.f_sve_general;
            uint16_t ebits = g->element_bits;
            uint8_t eb = (uint8_t)(ebits / 8u);
            size_t lanes = vl_bytes / eb;
            uint8_t out[ARM_SVE_MAX_VL_BYTES];
            memcpy(out, env->sve.zregs[g->zd], vl_bytes);
            for (size_t l = 0; l < lanes; ++l) {
                if (g->predicated && !pbit_get(env->sve.pregs[g->pg], l * eb)) {
                    continue;
                }
                uint64_t v = read_elem_u64(env->sve.zregs[g->zn], l, eb);
                uint64_t r = v;
                switch (g->subop) {
                case SVE_UNARY_ABS: {
                    int64_t sv = sext_bits(v, ebits);
                    r = (uint64_t)(sv < 0 ? -sv : sv);
                    break;
                }
                case SVE_UNARY_NEG:
                    r = (uint64_t)(-sext_bits(v, ebits));
                    break;
                case SVE_UNARY_CNOT:
                    r = (v == 0u) ? 1u : 0u;
                    break;
                case SVE_UNARY_SXTB: r = (uint64_t)(int64_t)(int8_t)v; break;
                case SVE_UNARY_SXTH: r = (uint64_t)(int64_t)(int16_t)v; break;
                case SVE_UNARY_SXTW: r = (uint64_t)(int64_t)(int32_t)v; break;
                case SVE_UNARY_UXTB: r = v & 0xffu; break;
                case SVE_UNARY_UXTH: r = v & 0xffffu; break;
                case SVE_UNARY_UXTW: r = v & 0xffffffffu; break;
                case SVE_UNARY_CNT: {
                    uint64_t c = 0;
                    for (uint16_t bit = 0; bit < ebits; ++bit) {
                        if ((v >> bit) & 1u) c++;
                    }
                    r = c;
                    break;
                }
                case SVE_UNARY_CLZ: {
                    uint64_t c = 0;
                    for (int bit = (int)ebits - 1; bit >= 0; --bit) {
                        if ((v >> bit) & 1u) break;
                        c++;
                    }
                    r = c;
                    break;
                }
                case SVE_UNARY_CLS: {
                    uint64_t sign_bit = (v >> (ebits - 1u)) & 1u;
                    uint64_t c = 0;
                    for (int bit = (int)ebits - 2; bit >= 0; --bit) {
                        if (((v >> bit) & 1u) != sign_bit) break;
                        c++;
                    }
                    r = c;
                    break;
                }
                case SVE_UNARY_RBIT: {
                    uint64_t rev = 0;
                    for (uint16_t bit = 0; bit < ebits; ++bit) {
                        rev = (rev << 1) | ((v >> bit) & 1u);
                    }
                    r = rev;
                    break;
                }
                default:
                    break;
                }
                write_elem_u64(out, l, eb, r & mask_bits(ebits));
            }
            memcpy(env->sve.zregs[g->zd], out, vl_bytes);
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_WMUL: {
            const arg_sve_general *g = &d->source.arg.f_sve_general;
            uint8_t dst_eb = g->element_bits / 8u;
            uint8_t src_eb = dst_eb / 2u;
            size_t lanes = vl_bytes / dst_eb;
            uint8_t out[ARM_SVE_MAX_VL_BYTES];
            memcpy(out, env->sve.zregs[g->zd], vl_bytes);
            for (size_t l = 0; l < lanes; ++l) {
                size_t sl = 2u * l + (g->top_half ? 1u : 0u);
                uint64_t vn = read_elem_u64(env->sve.zregs[g->zn], sl, src_eb);
                uint64_t vm = read_elem_u64(env->sve.zregs[g->zm], sl, src_eb);
                uint64_t prod = g->is_signed
                                    ? (uint64_t)(sext_bits(vn, src_eb * 8u) *
                                                 sext_bits(vm, src_eb * 8u))
                                    : (vn * vm);
                if (g->subop == SVE_WMUL_MLA) {
                    prod += read_elem_u64(env->sve.zregs[g->zd], l, dst_eb);
                } else if (g->subop == SVE_WMUL_MLS) {
                    prod = read_elem_u64(env->sve.zregs[g->zd], l, dst_eb) - prod;
                }
                write_elem_u64(out, l, dst_eb, prod);
            }
            memcpy(env->sve.zregs[g->zd], out, vl_bytes);
            return LATA_SVE_HELPER_OK;
        }
        case AARCH64_A64_SVE_IMM_BIN: {
            const arg_sve_general *g = &d->source.arg.f_sve_general;
            uint16_t ebits = ins->operation.vector.type.element_bits;
            uint8_t eb = (uint8_t)(ebits / 8u);
            size_t lanes = vl_bytes / eb;
            uint64_t imm = (g->subop == SVE_IMM_BIN_DUP)
                               ? (uint64_t)g->imm1
                               : ins->operation.vector.operand_immediates[1];
            uint8_t out[ARM_SVE_MAX_VL_BYTES];
            for (size_t l = 0; l < lanes; ++l) {
                if (g->subop == SVE_IMM_BIN_DUP) {
                    write_elem_u64(out, l, eb, imm);
                } else {
                    uint64_t v = read_elem_u64(env->sve.zregs[g->zn], l, eb);
                    uint64_t r = 0;
                    if (ins->opcode == PVIR_OP_AND) r = v & imm;
                    else if (ins->opcode == PVIR_OP_OR) r = v | imm;
                    else if (ins->opcode == PVIR_OP_XOR) r = v ^ imm;
                    else if (ins->opcode == PVIR_OP_ADD) r = v + imm;
                    else if (ins->opcode == PVIR_OP_MUL) r = v * imm;
                    else if (g->subop == SVE_IMM_BIN_SUBR) r = imm - v;
                    else r = v - imm;
                    write_elem_u64(out, l, eb, r);
                }
            }
            memcpy(env->sve.zregs[g->zd], out, vl_bytes);
            return LATA_SVE_HELPER_OK;
        }
        default:
            break;
        }
    }

    /* Generic PVIR dispatch (covers arithmetic, FMA, compare, select, convert, fixed, fp, reduce, permute, etc.) */
    switch (ins->opcode) {
    case PVIR_OP_PREDICATE_GENERATE: {
        const PvirPredicateGeneratorOperation *g =
            &ins->operation.predicate_generator;
        if (g->kind == PVIR_PREDICATE_GENERATOR_CONSTANT) {
            if (ins->effects.arch_state.domain == PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT) {
                memset(env->sve.ffr, 0xff, vl_bytes / 8u);
            } else {
                exec_predicate_constant(env, vl_bytes, g);
            }
        } else if (g->kind == PVIR_PREDICATE_GENERATOR_LANE_COMPARE) {
            exec_while(env, vl_bytes, d);
        } else {
            uint32_t pd = g->state_id - A64_SVE_PVIR_P_STATE_BASE;
            memcpy(env->sve.pregs[pd], env->sve.ffr, vl_bytes / 8u);
        }
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_ARCH_STATE:
        if (ins->effects.arch_state.domain == PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT &&
            ins->effects.arch_state.action != PVIR_ARCH_STATE_CLEAR) {
            int pn = h_find_preg(d, ins->operation.arch_state.input_value);
            if (pn >= 0) {
                memcpy(env->sve.ffr, env->sve.pregs[pn], vl_bytes / 8u);
            }
        }
        return LATA_SVE_HELPER_OK;
    case PVIR_OP_LOAD:
    case PVIR_OP_STORE:
        return exec_fault_or_contiguous_mem(env, d)
                   ? LATA_SVE_HELPER_FAULT : LATA_SVE_HELPER_OK;
    case PVIR_OP_NOT: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint8_t eb = (uint8_t)(op->type.element_bits / 8u);
        size_t lanes = vl_bytes / eb;
        int zd = d->has_source ? (int)d->source.arg.f_sve_not.zd
                               : h_find_zreg(d, op->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_not.zm
                               : h_find_zreg(d, op->operands[0]);
        int pg = d->has_source ? (int)d->source.arg.f_sve_not.pg : -1;
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            bool act = pred_lane_active(env, d, &op->predicate, pg, l, op->type.element_bits);
            if (act) {
                write_elem_u64(out, l, eb, ~read_elem_u64(env->sve.zregs[zn], l, eb));
            } else if (op->inactive_policy == PVIR_INACTIVE_ZERO) {
                write_elem_u64(out, l, eb, 0);
            }
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_BROADCAST: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint8_t eb = (uint8_t)(op->type.element_bits / 8u);
        size_t lanes = vl_bytes / eb;
        int zd = d->has_source ? (int)d->source.arg.f_sve_dup.zd
                               : h_find_zreg(d, op->destination);
        int rn = d->has_source ? (int)d->source.arg.f_sve_dup.rn
                               : h_find_gpr(op->operands[0]);
        uint64_t v = (op->operand_kinds[0] == PVIR_OPERAND_IMMEDIATE)
                         ? op->operand_immediates[0]
                         : ((rn >= 0 && rn < 31) ? env->xregs[rn] : 0u);
        for (size_t l = 0; l < lanes; ++l) {
            write_elem_u64(env->sve.zregs[zd], l, eb, v);
        }
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_COMPARE: {
        const PvirVectorOperation *op = &ins->operation.vector;
        bool is_flt = (op->operand_types[0].kind == PVIR_ELEMENT_FLOAT);
        uint16_t ebits = op->operand_types[0].element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        bool is_uo = (d->has_source && d->source.insn_type == AARCH64_A64_SVE_FCMP &&
                      d->source.arg.f_sve_ext_op.subop == PVIR_COMPARE_NONE);
        int pd = d->has_source ? (int)d->source.arg.f_sve_ext_op.zd_pd
                               : h_find_preg(d, op->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_ext_op.zn
                               : h_find_zreg(d, op->operands[0]);
        int zm = (op->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                     ? (d->has_source ? (int)d->source.arg.f_sve_ext_op.zm
                                      : h_find_zreg(d, op->operands[1]))
                     : -1;
        int pg = d->has_source ? (int)d->source.arg.f_sve_ext_op.pg : -1;
        int sr = (op->operand_kinds[1] == PVIR_OPERAND_SCALAR) ? h_find_gpr(op->operands[1]) : -1;
        uint8_t out[ARM_SVE_MAX_PRED_BYTES];
        memset(out, 0, vl_bytes / 8u);
        for (size_t l = 0; l < lanes; ++l) {
            if (!pred_lane_active(env, d, &op->predicate, pg, l, ebits)) {
                continue;
            }
            uint64_t a = read_elem_u64(env->sve.zregs[zn], l, eb);
            uint64_t b = (zm >= 0) ? read_elem_u64(env->sve.zregs[zm], l, eb)
                                   : ((sr >= 0 && sr < 31) ? env->xregs[sr]
                                                           : op->operand_immediates[1]);
            if (eval_cmp_cond(op->compare_condition, is_flt, is_uo, ebits, a, b)) {
                pbit_set(out, l * eb, true);
            }
        }
        memcpy(env->sve.pregs[pd], out, vl_bytes / 8u);
        if (instruction_writes_nzcv(ins, d)) {
            uint8_t *pg_ptr = (pg >= 0) ? env->sve.pregs[pg] : out;
            h_predtest(env, vl_bytes, (uint8_t)ebits, pg_ptr, out);
        }
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_SELECT: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint8_t eb = (uint8_t)(op->type.element_bits / 8u);
        size_t lanes = vl_bytes / eb;
        int zd = d->has_source ? (int)d->source.arg.f_sve_general.zd
                               : h_find_zreg(d, op->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_general.zn
                               : h_find_zreg(d, op->operands[0]);
        int zm = d->has_source ? (int)d->source.arg.f_sve_general.zm
                               : h_find_zreg(d, op->operands[1]);
        int pg = d->has_source ? (int)d->source.arg.f_sve_general.pg : -1;
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        for (size_t l = 0; l < lanes; ++l) {
            bool act = pred_lane_active(env, d, &op->predicate, pg, l, op->type.element_bits);
            uint64_t v = read_elem_u64(env->sve.zregs[act ? zn : zm], l, eb);
            write_elem_u64(out, l, eb, v);
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_FMA: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint16_t ebits = op->type.element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        bool is_flt = (op->type.kind == PVIR_ELEMENT_FLOAT);
        int zd = h_find_zreg(d, op->destination);
        int zn = h_find_zreg(d, op->operands[0]);
        int zm = h_find_zreg(d, op->operands[1]);
        int za = h_find_zreg(d, op->operands[2]);
        int pg = (op->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                     ? h_find_preg(d, op->predicate.source.id) : -1;
        bool sub_prod = false, neg_acc = false;
        if (d->has_source && d->source.insn_type == AARCH64_A64_SVE_FMLA) {
            const arg_sve_fp_ext *f = &d->source.arg.f_sve_fp_ext;
            zd = f->zd;
            zn = f->destructive_madd ? f->zd : f->zn;
            zm = f->zm;
            za = f->destructive_madd ? f->zn : f->zd;
            pg = f->pg;
            sub_prod = (f->subop == SVE_FP_EXT_FMLS || f->subop == SVE_FP_EXT_FNMLA);
            neg_acc = (f->subop == SVE_FP_EXT_FNMLA || f->subop == SVE_FP_EXT_FNMLS);
        } else if (d->has_source && d->source.insn_type == AARCH64_A64_SVE_WMUL) {
            const arg_sve_general *g = &d->source.arg.f_sve_general;
            zd = g->zd; zn = g->zn; zm = g->zm; za = g->zd; pg = g->pg;
            sub_prod = (g->subop == SVE_WMUL_MLS);
        }
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            if (!pred_lane_active(env, d, &op->predicate, pg, l, ebits)) continue;
            uint64_t a = read_elem_u64(env->sve.zregs[zn], l, eb);
            uint64_t b = read_elem_u64(env->sve.zregs[zm], l, eb);
            uint64_t c = read_elem_u64(env->sve.zregs[za], l, eb);
            uint64_t r = 0;
            if (is_flt) {
                if (ebits == 32) {
                    double prod = (double)u32_to_f32((uint32_t)a) * (double)u32_to_f32((uint32_t)b);
                    double acc = (double)u32_to_f32((uint32_t)c);
                    if (sub_prod) prod = -prod;
                    if (neg_acc) acc = -acc;
                    r = f32_to_u32((float)(acc + prod));
                } else {
                    double prod = u64_to_f64(a) * u64_to_f64(b);
                    double acc = u64_to_f64(c);
                    if (sub_prod) prod = -prod;
                    if (neg_acc) acc = -acc;
                    r = f64_to_u64(acc + prod);
                }
            } else {
                uint64_t prod = a * b;
                r = sub_prod ? (c - prod) : (c + prod);
            }
            write_elem_u64(out, l, eb, r);
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_FP_ARITH: {
        PvirFPKind fkind = ins->operation.fp.kind;
        uint16_t ebits = ins->operation.fp.type.element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        const PvirPredicate *pred = &ins->operation.fp.predicate;
        PvirInactivePolicy pol = ins->operation.fp.inactive_policy;
        int zd = h_find_zreg(d, ins->operation.fp.destination);
        int zn = h_find_zreg(d, ins->operation.fp.operands[0]);
        int zm = h_find_zreg(d, ins->operation.fp.operands[1]);
        int pg = -1;
        if (d->has_source) {
            zd = d->source.arg.f_sve_fp_ext.zd;
            zn = d->source.arg.f_sve_fp_ext.zn;
            zm = d->source.arg.f_sve_fp_ext.zm;
            pg = d->source.arg.f_sve_fp_ext.pg;
        }
        if (zm < 0) zm = zn;
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            bool act = (fkind == PVIR_FP_RECPS || fkind == PVIR_FP_RSQRTS ||
                        pred_lane_active(env, d, pred, pg, l, ebits));
            if (act) {
                uint64_t a = read_elem_u64(env->sve.zregs[zn], l, eb);
                uint64_t b = read_elem_u64(env->sve.zregs[zm], l, eb);
                write_elem_u64(out, l, eb, eval_fp_arith_elem(fkind, ebits, a, b));
            } else if (pol == PVIR_INACTIVE_ZERO) {
                write_elem_u64(out, l, eb, 0);
            }
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_ADD:
    case PVIR_OP_SUB:
    case PVIR_OP_MUL:
    case PVIR_OP_DIV:
    case PVIR_OP_REM:
    case PVIR_OP_AND:
    case PVIR_OP_OR:
    case PVIR_OP_XOR:
    case PVIR_OP_SHL:
    case PVIR_OP_SHR:
    case PVIR_OP_SAR:
    case PVIR_OP_MIN:
    case PVIR_OP_MAX: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint16_t ebits = op->type.element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        bool is_flt = (op->type.kind == PVIR_ELEMENT_FLOAT);
        bool is_signed = (op->type.signedness == PVIR_SIGNED);
        int zd = h_find_zreg(d, op->destination);
        int zn = h_find_zreg(d, op->operands[0]);
        int zm = (op->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                     ? h_find_zreg(d, op->operands[1]) : -1;
        int pg = -1;
        bool rev = false;
        if (d->has_source) {
            if (d->source.insn_type == AARCH64_A64_SVE_IDIV) {
                const arg_sve_general *g = &d->source.arg.f_sve_general;
                zd = g->zd; zn = g->zn; zm = g->zm; pg = g->pg;
                rev = (g->subop == SVE_IDIV_SDIVR || g->subop == SVE_IDIV_UDIVR);
            } else {
                zd = d->source.arg.f_sve_add.zd;
                zn = zd;
                zm = d->source.arg.f_sve_add.zm;
                pg = d->source.arg.f_sve_add.pg;
            }
        }
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            if (pred_lane_active(env, d, &op->predicate, pg, l, ebits)) {
                uint64_t a = read_elem_u64(env->sve.zregs[zn], l, eb);
                uint64_t b = (zm >= 0) ? read_elem_u64(env->sve.zregs[zm], l, eb)
                                       : op->operand_immediates[1];
                uint64_t lhs = rev ? b : a;
                uint64_t rhs = rev ? a : b;
                if (is_flt) {
                    PvirFPKind fk = (ins->opcode == PVIR_OP_ADD) ? PVIR_FP_ADD
                                    : (ins->opcode == PVIR_OP_SUB ? PVIR_FP_SUB
                                       : (ins->opcode == PVIR_OP_MUL ? PVIR_FP_MUL : PVIR_FP_DIV));
                    write_elem_u64(out, l, eb, eval_fp_arith_elem(fk, ebits, lhs, rhs));
                } else {
                    write_elem_u64(out, l, eb, eval_int_bin(ins->opcode, ebits, is_signed, lhs, rhs));
                }
            } else if (op->inactive_policy == PVIR_INACTIVE_ZERO) {
                write_elem_u64(out, l, eb, 0);
            }
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_CONVERT: {
        const PvirConvertOperation *c = &ins->operation.convert;
        uint16_t sb = c->source_type.element_bits, db = c->destination_type.element_bits;
        bool sf = (c->source_type.kind == PVIR_ELEMENT_FLOAT);
        bool df = (c->destination_type.kind == PVIR_ELEMENT_FLOAT);
        bool ss = (c->source_type.signedness == PVIR_SIGNED);
        bool ds = (c->destination_type.signedness == PVIR_SIGNED);
        int zd = d->has_source ? (int)d->source.arg.f_sve_ext_op.zd_pd : h_find_zreg(d, c->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_ext_op.zn : h_find_zreg(d, c->source);
        int pg = d->has_source ? (int)d->source.arg.f_sve_ext_op.pg : -1;
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        if (sb == db) {
            uint8_t eb = (uint8_t)(sb / 8u);
            size_t lanes = vl_bytes / eb;
            for (size_t l = 0; l < lanes; ++l) {
                if (pg >= 0 && !pbit_get(env->sve.pregs[pg], l * eb)) continue;
                uint64_t v = read_elem_u64(env->sve.zregs[zn], l, eb);
                uint64_t r = v;
                if (sf && !df) {
                    if (sb == 32) {
                        float f = u32_to_f32((uint32_t)v);
                        r = ds ? (uint64_t)(int32_t)f : (uint64_t)(uint32_t)f;
                    } else {
                        double dv = u64_to_f64(v);
                        r = ds ? (uint64_t)(int64_t)dv : (uint64_t)dv;
                    }
                } else if (!sf && df) {
                    if (sb == 32) {
                        float f = ss ? (float)(int32_t)v : (float)(uint32_t)v;
                        r = f32_to_u32(f);
                    } else {
                        double dv = ss ? (double)(int64_t)v : (double)v;
                        r = f64_to_u64(dv);
                    }
                }
                write_elem_u64(out, l, eb, r);
            }
        } else {
            size_t lanes = vl_bytes / 8u;
            uint8_t seb = (uint8_t)(sb / 8u), deb = (uint8_t)(db / 8u);
            for (size_t l = 0; l < lanes; ++l) {
                if (pg >= 0 && !pbit_get(env->sve.pregs[pg], l * 8u)) continue;
                uint64_t v = read_elem_u64(env->sve.zregs[zn], l * (8u / seb), seb);
                uint64_t r = 0;
                if (sf && df) {
                    r = (sb == 32) ? f64_to_u64((double)u32_to_f32((uint32_t)v))
                                   : (uint64_t)f32_to_u32((float)u64_to_f64(v));
                } else if (sf && !df) {
                    double dv = (sb == 32) ? (double)u32_to_f32((uint32_t)v) : u64_to_f64(v);
                    r = ds ? ((db == 64) ? (uint64_t)(int64_t)dv : (uint64_t)(uint32_t)(int32_t)dv)
                           : ((db == 64) ? (uint64_t)dv : (uint64_t)(uint32_t)dv);
                } else if (!sf && df) {
                    if (db == 64) {
                        double dv = ss ? (double)(int32_t)v : (double)(uint32_t)v;
                        r = f64_to_u64(dv);
                    } else {
                        float fv = ss ? (float)(int64_t)v : (float)v;
                        r = f32_to_u32(fv);
                    }
                }
                write_elem_u64(out, l * (8u / deb), deb, r);
            }
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_FIXED: {
        const PvirFixedPointOperation *fx = &ins->operation.fixed;
        uint16_t ebits = fx->type.element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        bool is_signed = (fx->type.signedness == PVIR_SIGNED);
        int zd = d->has_source ? (int)d->source.arg.f_sve_general.zd : h_find_zreg(d, fx->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_general.zn : h_find_zreg(d, fx->operands[0]);
        int zm = (fx->operand_kinds[1] == PVIR_OPERAND_VECTOR)
                     ? (d->has_source ? (int)d->source.arg.f_sve_general.zm : h_find_zreg(d, fx->operands[1]))
                     : -1;
        int pg = (d->has_source && d->source.arg.f_sve_general.predicated)
                     ? (int)d->source.arg.f_sve_general.pg : -1;
        uint64_t m = mask_bits(ebits);
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            if (!pred_lane_active(env, d, &fx->predicate, pg, l, ebits)) {
                if (fx->inactive_policy == PVIR_INACTIVE_ZERO) write_elem_u64(out, l, eb, 0);
                continue;
            }
            uint64_t a = read_elem_u64(env->sve.zregs[zn], l, eb);
            uint64_t b = (zm >= 0) ? read_elem_u64(env->sve.zregs[zm], l, eb) : fx->operand_immediates[1];
            uint64_t r = 0;
            if (fx->kind == PVIR_FIXED_AVG_ADD || fx->kind == PVIR_FIXED_AVG_SUB) {
                bool sub = (fx->kind == PVIR_FIXED_AVG_SUB);
                if (is_signed) {
                    int64_t sa = sext_bits(a, ebits), sb = sext_bits(b, ebits);
                    if (ebits < 64) {
                        r = ((uint64_t)((sub ? (sa - sb) : (sa + sb)) >> 1)) & m;
                    } else {
                        int64_t ha = sa >> 1, hb = sb >> 1;
                        int64_t corr = sub ? (-((~sa & sb) & 1)) : (sa & sb & 1);
                        r = (uint64_t)(ha + (sub ? -hb : hb) + corr);
                    }
                } else {
                    if (ebits < 64) {
                        r = ((sub ? (a - b) : (a + b)) >> 1) & m;
                    } else {
                        uint64_t ha = a >> 1, hb = b >> 1;
                        uint64_t corr = sub ? ((~a & b) & 1u) : (a & b & 1u);
                        r = sub ? (ha - hb - corr) : (ha + hb + corr);
                    }
                }
            } else {
                bool is_sub = (fx->kind == PVIR_FIXED_SAT_SUB);
                if (is_signed) {
                    int64_t sa = sext_bits(a, ebits), sb = sext_bits(b, ebits);
                    int64_t maxv = (int64_t)(m >> 1), minv = -maxv - 1;
                    if (ebits < 64) {
                        int64_t v = is_sub ? (sa - sb) : (sa + sb);
                        if (v > maxv) v = maxv;
                        if (v < minv) v = minv;
                        r = ((uint64_t)v) & m;
                    } else {
                        int64_t v;
                        if (is_sub) {
                            if (sb < 0 && sa > INT64_MAX + sb) v = INT64_MAX;
                            else if (sb > 0 && sa < INT64_MIN + sb) v = INT64_MIN;
                            else v = sa - sb;
                        } else {
                            if (sb > 0 && sa > INT64_MAX - sb) v = INT64_MAX;
                            else if (sb < 0 && sa < INT64_MIN - sb) v = INT64_MIN;
                            else v = sa + sb;
                        }
                        r = (uint64_t)v;
                    }
                } else {
                    if (is_sub) {
                        r = (a >= b) ? (a - b) : 0u;
                    } else {
                        r = (a > m - b) ? m : (a + b);
                    }
                }
            }
            write_elem_u64(out, l, eb, r);
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_REDUCE: {
        const PvirReduceOperation *red = &ins->operation.reduce;
        uint16_t ebits = red->type.element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        bool is_flt = (red->type.kind == PVIR_ELEMENT_FLOAT);
        bool is_signed = (red->type.signedness == PVIR_SIGNED);
        int zd = d->has_source ? (int)d->source.arg.f_sve_general.zd : h_find_zreg(d, red->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_general.zn : h_find_zreg(d, red->source);
        int pg = d->has_source ? (int)d->source.arg.f_sve_general.pg
                               : ((red->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                                      ? h_find_preg(d, red->predicate.source.id) : -1);
        if (zd < 0) zd = 0;
        uint64_t acc = 0;
        if (is_flt) {
            if (red->kind == PVIR_REDUCE_ADD_ORDERED) {
                acc = read_elem_u64(env->sve.zregs[zd], 0, eb);
            } else if (red->kind == PVIR_REDUCE_ADD) {
                acc = 0u;
            } else if (red->kind == PVIR_REDUCE_MAX || red->kind == PVIR_REDUCE_MAXNM) {
                acc = (ebits == 32) ? UINT64_C(0xff800000) : UINT64_C(0xfff0000000000000);
            } else {
                acc = (ebits == 32) ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
            }
        } else {
            if (red->kind == PVIR_REDUCE_AND) acc = mask_bits(64);
            else if (red->kind == PVIR_REDUCE_MAX) acc = is_signed ? (uint64_t)INT64_MIN : 0u;
            else if (red->kind == PVIR_REDUCE_MIN) acc = is_signed ? (uint64_t)INT64_MAX : UINT64_MAX;
            else acc = 0u;
        }
        for (size_t l = 0; l < lanes; ++l) {
            if (pg >= 0 && !pbit_get(env->sve.pregs[pg], l * eb)) continue;
            uint64_t v = read_elem_u64(env->sve.zregs[zn], l, eb);
            if (is_flt) {
                PvirFPKind fk = (red->kind == PVIR_REDUCE_ADD || red->kind == PVIR_REDUCE_ADD_ORDERED) ? PVIR_FP_ADD
                                : (red->kind == PVIR_REDUCE_MAX ? PVIR_FP_MAX
                                   : (red->kind == PVIR_REDUCE_MIN ? PVIR_FP_MIN
                                      : (red->kind == PVIR_REDUCE_MAXNM ? PVIR_FP_MAXNM : PVIR_FP_MINNM)));
                acc = eval_fp_arith_elem(fk, ebits, acc, v);
            } else {
                int64_t sv = sext_bits(v, ebits);
                switch (red->kind) {
                case PVIR_REDUCE_ADD:
                case PVIR_REDUCE_ADD_ORDERED:
                    acc += is_signed ? (uint64_t)sv : v;
                    break;
                case PVIR_REDUCE_AND: acc &= v; break;
                case PVIR_REDUCE_OR: acc |= v; break;
                case PVIR_REDUCE_XOR: acc ^= v; break;
                case PVIR_REDUCE_MAX:
                    if (is_signed) { if (sv > (int64_t)acc) acc = (uint64_t)sv; }
                    else { if (v > acc) acc = v; }
                    break;
                case PVIR_REDUCE_MIN:
                    if (is_signed) { if (sv < (int64_t)acc) acc = (uint64_t)sv; }
                    else { if (v < acc) acc = v; }
                    break;
                default: break;
                }
            }
        }
        if (is_flt) {
            memset(env->sve.zregs[zd], 0, vl_bytes);
            write_elem_u64(env->sve.zregs[zd], 0, eb, acc);
        } else {
            uint8_t rdb = (uint8_t)(red->result_type.element_bits ? red->result_type.element_bits / 8u : 8u);
            memset(env->sve.zregs[zd], 0, vl_bytes);
            write_elem_u64(env->sve.zregs[zd], 0, rdb, acc);
        }
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_PERMUTE: {
        const PvirPermuteOperation *p = &ins->operation.permute;
        uint8_t eb = (uint8_t)(p->type.element_bits / 8u);
        size_t lanes = vl_bytes / eb;
        int zd = d->has_source ? (int)d->source.arg.f_sve_general.zd : h_find_zreg(d, p->destination);
        int zn = d->has_source ? (int)d->source.arg.f_sve_general.zn : h_find_zreg(d, p->operands[0]);
        int zm = (p->operand_count >= 2) ? (d->has_source ? (int)d->source.arg.f_sve_general.zm : h_find_zreg(d, p->operands[1])) : zn;
        int pg = p->has_predicate ? (d->has_source ? (int)d->source.arg.f_sve_general.pg : h_find_preg(d, p->predicate.source.id)) : -1;
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memset(out, 0, vl_bytes);
        if (p->kind == PVIR_PERMUTE_COMPACT) {
            size_t dst_l = 0;
            for (size_t l = 0; l < lanes; ++l) {
                if (pg >= 0 && !pbit_get(env->sve.pregs[pg], l * eb)) continue;
                write_elem_u64(out, dst_l++, eb, read_elem_u64(env->sve.zregs[zn], l, eb));
            }
        } else if (p->kind == PVIR_PERMUTE_REVB || p->kind == PVIR_PERMUTE_REVH ||
                   p->kind == PVIR_PERMUTE_REVW) {
            size_t sb = (p->kind == PVIR_PERMUTE_REVB) ? 1u : (p->kind == PVIR_PERMUTE_REVH ? 2u : 4u);
            memcpy(out, env->sve.zregs[zd], vl_bytes);
            for (size_t l = 0; l < lanes; ++l) {
                if (sb < eb && pg >= 0 && !pbit_get(env->sve.pregs[pg], l * eb)) continue;
                if (sb < eb) {
                    size_t sub_cnt = eb / sb;
                    for (size_t k = 0; k < sub_cnt; ++k) {
                        memcpy(out + l * eb + k * sb,
                               env->sve.zregs[zn] + l * eb + (sub_cnt - 1u - k) * sb, sb);
                    }
                } else {
                    memcpy(out + l * eb, env->sve.zregs[zn] + l * eb, eb);
                }
            }
        } else if (p->kind == PVIR_PERMUTE_REV) {
            for (size_t l = 0; l < lanes; ++l) {
                write_elem_u64(out, l, eb, read_elem_u64(env->sve.zregs[zn], lanes - 1u - l, eb));
            }
        } else if (p->kind == PVIR_PERMUTE_TBL || p->kind == PVIR_PERMUTE_TBX) {
            bool two = (p->kind == PVIR_PERMUTE_TBL && p->imm == 1u);
            size_t lmax = two ? lanes * 2u : lanes;
            for (size_t l = 0; l < lanes; ++l) {
                uint64_t idx = read_elem_u64(env->sve.zregs[zm], l, eb);
                if (idx < lmax) {
                    uint8_t src_z = (idx >= lanes) ? ((zn + 1u) & 31u) : (uint8_t)zn;
                    write_elem_u64(out, l, eb, read_elem_u64(env->sve.zregs[src_z], idx % lanes, eb));
                } else if (p->kind == PVIR_PERMUTE_TBX) {
                    write_elem_u64(out, l, eb, read_elem_u64(env->sve.zregs[zd], l, eb));
                }
            }
        } else if (p->kind == PVIR_PERMUTE_ZIP1 || p->kind == PVIR_PERMUTE_ZIP2) {
            size_t base = (p->kind == PVIR_PERMUTE_ZIP2) ? (lanes / 2u) : 0u;
            for (size_t l = 0; l < lanes / 2u; ++l) {
                write_elem_u64(out, 2u * l, eb, read_elem_u64(env->sve.zregs[zn], base + l, eb));
                write_elem_u64(out, 2u * l + 1u, eb, read_elem_u64(env->sve.zregs[zm], base + l, eb));
            }
        } else if (p->kind == PVIR_PERMUTE_UZP1 || p->kind == PVIR_PERMUTE_UZP2) {
            size_t parity = (p->kind == PVIR_PERMUTE_UZP2) ? 1u : 0u;
            size_t half = lanes / 2u;
            for (size_t l = 0; l < half; ++l) {
                write_elem_u64(out, l, eb, read_elem_u64(env->sve.zregs[zn], 2u * l + parity, eb));
                write_elem_u64(out, half + l, eb, read_elem_u64(env->sve.zregs[zm], 2u * l + parity, eb));
            }
        } else if (p->kind == PVIR_PERMUTE_TRN1 || p->kind == PVIR_PERMUTE_TRN2) {
            size_t parity = (p->kind == PVIR_PERMUTE_TRN2) ? 1u : 0u;
            for (size_t l = 0; l < lanes; l += 2u) {
                write_elem_u64(out, l, eb, read_elem_u64(env->sve.zregs[zn], l + parity, eb));
                write_elem_u64(out, l + 1u, eb, read_elem_u64(env->sve.zregs[zm], l + parity, eb));
            }
        } else if (p->kind == PVIR_PERMUTE_EXT) {
            size_t off = (p->imm < vl_bytes) ? p->imm : 0u;
            memcpy(out, env->sve.zregs[zn] + off, vl_bytes - off);
            memcpy(out + (vl_bytes - off), env->sve.zregs[zm], off);
        } else if (p->kind == PVIR_PERMUTE_SPLICE) {
            int first = -1, last = -1;
            for (size_t l = 0; l < lanes; ++l) {
                if (pg >= 0 && pbit_get(env->sve.pregs[pg], l * eb)) {
                    if (first < 0) first = (int)l;
                    last = (int)l;
                }
            }
            size_t pos = 0;
            if (first >= 0) {
                for (int l = first; l <= last && pos < lanes; ++l) {
                    write_elem_u64(out, pos++, eb, read_elem_u64(env->sve.zregs[zn], (size_t)l, eb));
                }
            }
            for (size_t l = 0; pos < lanes && l < lanes; ++l) {
                write_elem_u64(out, pos++, eb, read_elem_u64(env->sve.zregs[zm], l, eb));
            }
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_LANE_INDEX: {
        const PvirIndexOperation *idx = &ins->operation.index;
        uint8_t eb = (uint8_t)(idx->type.element_bits / 8u);
        size_t lanes = vl_bytes / eb;
        int zd = d->has_source ? (int)d->source.arg.f_sve_general.zd : h_find_zreg(d, idx->destination);
        uint64_t start = 0, step = 1;
        if (d->has_source && d->source.insn_type == AARCH64_A64_SVE_INDEX) {
            const arg_sve_general *g = &d->source.arg.f_sve_general;
            start = g->src1_is_imm ? (uint64_t)g->imm1 : env->xregs[g->zn];
            step = g->src2_is_imm ? (uint64_t)g->imm2 : env->xregs[g->zm];
        } else {
            int g1 = h_find_gpr(idx->start_value), g2 = h_find_gpr(idx->step_value);
            if (g1 >= 0 && g1 < 31) start = env->xregs[g1];
            if (idx->step_value != PVIR_NO_VALUE && g2 >= 0 && g2 < 31) step = env->xregs[g2];
        }
        for (size_t l = 0; l < lanes; ++l) {
            write_elem_u64(env->sve.zregs[zd], l, eb, start + l * step);
        }
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_INSERT: {
        const PvirLaneOperation *op = &ins->operation.lane;
        uint8_t eb = (uint8_t)(op->vector_type.element_bits / 8u);
        if (d->has_source && d->source.insn_type == AARCH64_A64_SVE_PERMUTE) {
            const arg_sve_general *g = &d->source.arg.f_sve_general;
            size_t lanes = vl_bytes / eb;
            uint64_t ins_val = g->src1_is_imm ? read_elem_u64(env->sve.zregs[g->zm], 0, eb)
                                              : (g->zm < 31 ? env->xregs[g->zm] : 0u);
            for (size_t l = lanes; l > 1; --l) {
                write_elem_u64(env->sve.zregs[g->zd], l - 1u, eb,
                               read_elem_u64(env->sve.zregs[g->zd], l - 2u, eb));
            }
            write_elem_u64(env->sve.zregs[g->zd], 0, eb, ins_val);
            return LATA_SVE_HELPER_OK;
        }
        int zd = h_find_zreg(d, op->destination);
        int zsrc = h_find_zreg(d, op->merge_value);
        int gpr = h_find_gpr(op->source);
        if (zd >= 0 && zsrc >= 0 && zd != zsrc) {
            memcpy(env->sve.zregs[zd], env->sve.zregs[zsrc], vl_bytes);
        }
        if (zd >= 0) {
            uint64_t val = (gpr >= 0 && gpr < 31) ? env->xregs[gpr] : 0u;
            write_elem_u64(env->sve.zregs[zd], op->lane, eb, val);
        }
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_EXTRACT: {
        const PvirLaneOperation *op = &ins->operation.lane;
        uint8_t eb = (uint8_t)(op->vector_type.element_bits / 8u);
        int zn = h_find_zreg(d, op->source);
        uint64_t val = read_elem_u64(env->sve.zregs[zn], op->lane, eb);
        int dg = h_find_gpr(op->destination), dz = h_find_zreg(d, op->destination);
        if (dg >= 0 && dg < 31) env->xregs[dg] = val;
        else if (dz >= 0) write_elem_u64(env->sve.zregs[dz], 0, eb, val);
        else env->xregs[0] = val;
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_EXTEND:
    case PVIR_OP_MOVE: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint16_t ebits = op->type.element_bits;
        uint8_t eb = (uint8_t)(ebits / 8u);
        size_t lanes = vl_bytes / eb;
        bool is_ext = (ins->opcode == PVIR_OP_EXTEND);
        uint16_t sbits = is_ext ? op->operand_types[0].element_bits : ebits;
        bool esign = (op->operand_types[0].signedness == PVIR_SIGNED);
        int zd = h_find_zreg(d, op->destination);
        int zn = h_find_zreg(d, op->operands[0]);
        int pg = (op->predicate.source.kind == PVIR_PREDICATE_SOURCE_PREDICATE)
                     ? h_find_preg(d, op->predicate.source.id) : -1;
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memcpy(out, env->sve.zregs[zd], vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            if (!pred_lane_active(env, d, &op->predicate, pg, l, ebits)) {
                if (op->inactive_policy == PVIR_INACTIVE_ZERO) write_elem_u64(out, l, eb, 0);
                continue;
            }
            uint64_t v = read_elem_u64(env->sve.zregs[zn], l, eb);
            if (is_ext) {
                v = esign ? (uint64_t)sext_bits(v, sbits) : (v & mask_bits(sbits));
            }
            write_elem_u64(out, l, eb, v);
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    case PVIR_OP_TRUNCATE: {
        const PvirVectorOperation *op = &ins->operation.vector;
        uint16_t dbits = op->type.element_bits, sbits = op->operand_types[0].element_bits;
        uint8_t deb = (uint8_t)(dbits / 8u), seb = (uint8_t)(sbits / 8u);
        size_t lanes = vl_bytes / seb;
        int zd = h_find_zreg(d, op->destination), zn = h_find_zreg(d, op->operands[0]);
        uint8_t out[ARM_SVE_MAX_VL_BYTES];
        memset(out, 0, vl_bytes);
        for (size_t l = 0; l < lanes; ++l) {
            write_elem_u64(out, l, deb, read_elem_u64(env->sve.zregs[zn], l, seb));
        }
        memcpy(env->sve.zregs[zd], out, vl_bytes);
        return LATA_SVE_HELPER_OK;
    }
    default:
        /* Neither the source-type switch nor the opcode switch claimed this
         * instruction. If the translate-time query said it was supported, the
         * two have drifted apart -- report INTERNAL so the caller traps instead
         * of the old behaviour, which returned success for work that never
         * happened. */
        return lata_sve_helper_support(ins,
                                       d->has_source ? &d->source : NULL) ==
                       LATA_SVE_HELPER_SUPPORTED
                   ? LATA_SVE_HELPER_INTERNAL
                   : LATA_SVE_HELPER_UNSUPPORTED;
    }
}

uint64_t lata_sve_exec_helper(void *env_ptr, const LataSVEHelperDesc *d)
{
    return (uint64_t)lata_sve_exec_helper_status(env_ptr, d);
}

void lata_sve_helper_unexpected(void *env_ptr, const LataSVEHelperDesc *d,
                                uint64_t status)
{
    CPUARMState *env = (CPUARMState *)env_ptr;

    qemu_log_mask(LOG_UNIMP,
                  "sve-helper: unexpected status %u for opcode %u at 0x%llx\n",
                  (unsigned)status, (unsigned)(d != NULL ? d->pvir.opcode : 0),
                  (unsigned long long)(d != NULL ? d->pc_curr : 0));
    if (env == NULL) {
        return;
    }
    /*
     * The instruction did not execute. Continuing at pc_next would leave the
     * destination register holding whatever it had before -- the exact silent
     * no-op this whole status path exists to prevent -- so trap instead.
     */
    env->pc = (d != NULL) ? d->pc_curr : env->pc;
    helper_exception_with_syndrome(env, EXCP_UDEF, syn_uncategorized());
}
