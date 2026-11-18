#ifndef QEMU_HOST_UTILS_H
#define QEMU_HOST_UTILS_H
#include "qemu/osdep.h"

static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline int ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
static inline int ctz64(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
static inline int ctpop32(uint32_t v) { return __builtin_popcount(v); }
static inline int ctpop64(uint64_t v) { return __builtin_popcountll(v); }
static inline int popcount32(uint32_t v) { return __builtin_popcount(v); }
static inline int popcount64(uint64_t v) { return __builtin_popcountll(v); }

/* QEMU's 128-bit helpers split the product/sum through out-parameters. */
static inline void mulu64(uint64_t *hi, uint64_t *lo, uint64_t a, uint64_t b)
{
    unsigned __int128 r = (unsigned __int128)a * b;

    *hi = (uint64_t)(r >> 64);
    *lo = (uint64_t)r;
}

static inline void muls64(int64_t *hi, int64_t *lo, int64_t a, int64_t b)
{
    __int128 r = (__int128)a * b;

    *hi = (int64_t)(r >> 64);
    *lo = (int64_t)r;
}

static inline uint64_t udiv_qrnnd(uint64_t *r, uint64_t n1, uint64_t n0, uint64_t d)
{
    unsigned __int128 n = ((unsigned __int128)n1 << 64) | n0;

    *r = (uint64_t)(n % d);
    return (uint64_t)(n / d);
}

/* QEMU's convention: out-parameter receives the sum, return value is the
 * overflow flag. */
static inline bool uadd64_overflow(uint64_t a, uint64_t b, uint64_t *ret)
{
    return __builtin_add_overflow(a, b, ret);
}

static inline bool uadd32_overflow(uint32_t a, uint32_t b, uint32_t *ret)
{
    return __builtin_add_overflow(a, b, ret);
}

static inline bool usub64_overflow(uint64_t a, uint64_t b, uint64_t *ret)
{
    return __builtin_sub_overflow(a, b, ret);
}

static inline bool usub32_overflow(uint32_t a, uint32_t b, uint32_t *ret)
{
    return __builtin_sub_overflow(a, b, ret);
}

static inline uint64_t uadd64_carry(uint64_t a, uint64_t b, bool *carry)
{
    unsigned __int128 r = (unsigned __int128)a + b + (*carry ? 1u : 0u);

    *carry = (r >> 64) != 0;
    return (uint64_t)r;
}

static inline uint64_t usub64_borrow(uint64_t a, uint64_t b, bool *borrow)
{
    unsigned __int128 rhs = (unsigned __int128)b + (*borrow ? 1u : 0u);

    *borrow = (unsigned __int128)a < rhs;
    return (uint64_t)((unsigned __int128)a - rhs);
}
#endif
