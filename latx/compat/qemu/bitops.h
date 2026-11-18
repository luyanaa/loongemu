#ifndef QEMU_BITOPS_H
#define QEMU_BITOPS_H
#include "qemu/osdep.h"

static inline uint32_t rol32(uint32_t v, int s) { return (v << (s & 31)) | (v >> ((32 - s) & 31)); }
static inline uint32_t ror32(uint32_t v, int s) { return (v >> (s & 31)) | (v << ((32 - s) & 31)); }
static inline uint64_t rol64(uint64_t v, int s) { return (v << (s & 63)) | (v >> ((64 - s) & 63)); }
static inline uint64_t ror64(uint64_t v, int s) { return (v >> (s & 63)) | (v << ((64 - s) & 63)); }

static inline uint32_t extract32(uint32_t v, int start, int len)
{
    uint32_t mask = (len == 32) ? 0xffffffffu : ((1u << len) - 1);
    return (v >> start) & mask;
}
static inline uint64_t extract64(uint64_t v, int start, int len)
{
    uint64_t mask = (len == 64) ? ~0ull : ((1ull << len) - 1);
    return (v >> start) & mask;
}
static inline int32_t sextract32(uint32_t v, int start, int len)
{
    uint32_t mask = (len == 32) ? 0xffffffffu : ((1u << len) - 1);
    uint32_t raw = (v >> start) & mask;
    if (len < 32 && (raw & (1u << (len - 1)))) raw |= ~mask;
    return (int32_t)raw;
}
static inline uint32_t deposit32(uint32_t v, int start, int len, uint32_t f)
{
    uint32_t mask = ((len == 32) ? 0xffffffffu : ((1u << len) - 1)) << start;
    return (v & ~mask) | ((f << start) & mask);
}
static inline uint64_t deposit64(uint64_t v, int start, int len, uint64_t f)
{
    uint64_t mask = ((len == 64) ? ~0ull : ((1ull << len) - 1)) << start;
    return (v & ~mask) | ((f << start) & mask);
}
#define MAKE_64BIT_MASK(shift, length) \
    (((~UINT64_C(0)) >> (64 - (length))) << (shift))

static inline bool is_power_of_2(uint64_t v) { return v && !(v & (v - 1)); }
#endif
