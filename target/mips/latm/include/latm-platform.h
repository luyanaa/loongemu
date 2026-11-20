#ifndef LATM_PLATFORM_H
#define LATM_PLATFORM_H

#include <stdint.h>
#include "cpu.h"

typedef enum LatmMemStatus {
    LATM_MEM_OK = 0,
    LATM_MEM_FAULT = 1,
    LATM_MEM_RESERVED = 2,
} LatmMemStatus;

typedef struct LatmPlatform {
    int (*mem_read)(CPUMIPSState *env, uint64_t gpa, uint64_t *value,
                    uint32_t size);
    int (*mem_write)(CPUMIPSState *env, uint64_t gpa, uint64_t value,
                     uint32_t size);
    int (*fetch32)(CPUMIPSState *env, uint64_t gpa, uint32_t *value);
    uint32_t (*read_count)(CPUMIPSState *env, uint64_t hz);
    void (*store_count)(CPUMIPSState *env, uint32_t count);
    void (*set_compare)(CPUMIPSState *env, uint32_t compare);
    void (*tlb_flush)(CPUMIPSState *env);
    void (*smc_broadcast)(CPUMIPSState *env, uint64_t start, uint64_t end);
    void (*request_interrupt)(CPUMIPSState *env, uint32_t cause_bits);
    uint64_t (*iocsr_read)(CPUMIPSState *env, uint64_t addr, unsigned size);
    void (*iocsr_write)(CPUMIPSState *env, uint64_t addr, uint64_t val,
                        unsigned size);
    int (*atomic_ll)(CPUMIPSState *env, uint64_t gpa, unsigned size,
                     uint64_t *value);
    int (*atomic_sc)(CPUMIPSState *env, uint64_t gpa, unsigned size,
                     uint64_t expected, uint64_t desired, int *ok);
    void *opaque;
} LatmPlatform;

void latm_platform_set(const LatmPlatform *platform);
const LatmPlatform *latm_platform_get(void);

#endif
