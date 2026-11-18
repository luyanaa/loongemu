#ifndef LATX_BARE_QEMU_BSWAP_H
#define LATX_BARE_QEMU_BSWAP_H
#include "qemu/osdep.h"
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif
