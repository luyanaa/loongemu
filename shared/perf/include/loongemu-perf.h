#ifndef LOONGEMU_PERF_H
#define LOONGEMU_PERF_H

#include <stdint.h>

#ifdef LOONGEMU_PERF
void loongemu_perf_reset(void);
void loongemu_perf_record_tb(uint64_t host_insns, uint64_t guest_insns);
uint64_t loongemu_perf_tb_count(void);
uint64_t loongemu_perf_host_insns(void);
uint64_t loongemu_perf_guest_insns(void);
double loongemu_perf_host_insns_per_guest_insn(void);
#else
static inline void loongemu_perf_reset(void) {}
static inline void loongemu_perf_record_tb(uint64_t host_insns,
                                           uint64_t guest_insns)
{
    (void)host_insns;
    (void)guest_insns;
}
static inline uint64_t loongemu_perf_tb_count(void) { return 0; }
static inline uint64_t loongemu_perf_host_insns(void) { return 0; }
static inline uint64_t loongemu_perf_guest_insns(void) { return 0; }
static inline double loongemu_perf_host_insns_per_guest_insn(void)
{
    return 0.0;
}
#endif

#endif
