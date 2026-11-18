#ifndef LATX_BARE_QEMU_INT128_H
#define LATX_BARE_QEMU_INT128_H
/* 128-bit integer helpers backed by the compiler type. */
#include "qemu/osdep.h"
typedef unsigned __int128 Int128;
typedef __int128 SignedInt128;

static inline Int128 int128_make128(uint64_t lo, uint64_t hi)
{
    return ((Int128)hi << 64) | lo;
}
static inline uint64_t int128_getlo(Int128 v) { return (uint64_t)v; }
static inline int64_t int128_gethi(Int128 v) { return (int64_t)(v >> 64); }
static inline Int128 int128_add(Int128 a, Int128 b) { return a + b; }
static inline Int128 int128_sub(Int128 a, Int128 b) { return a - b; }
static inline Int128 int128_and(Int128 a, Int128 b) { return a & b; }
static inline Int128 int128_or(Int128 a, Int128 b) { return a | b; }
static inline Int128 int128_xor(Int128 a, Int128 b) { return a ^ b; }
static inline Int128 int128_neg(Int128 a) { return -a; }
static inline Int128 int128_shl(Int128 a, unsigned s) { return a << s; }
static inline Int128 int128_shr(Int128 a, unsigned s) { return a >> s; }
static inline Int128 int128_shrs(Int128 a, unsigned s) { return ((SignedInt128)a) >> s; }
#endif
