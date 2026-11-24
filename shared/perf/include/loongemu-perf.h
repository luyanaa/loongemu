#ifndef LOONGEMU_PERF_H
#define LOONGEMU_PERF_H

#include <stdint.h>

/*
 * Guest memory-access event counters. They answer the question the P1
 * fast-path work turns on: how often does a guest load/store hit the software
 * TLB, how often does it need a walk, and how much of the traffic is MMIO or a
 * fault. A build without LOONGEMU_PERF compiles them away.
 */
enum {
    LOONGEMU_PERF_TLB_HIT = 0,
    LOONGEMU_PERF_TLB_MISS,
    LOONGEMU_PERF_MMIO_ACCESS,
    LOONGEMU_PERF_MEM_FAULT,
    LOONGEMU_PERF_PLAN_HIT, /* TLB hit that also reused the cached map plan */
    LOONGEMU_PERF_FAST_HIT, /* access served without entering the walker */
    LOONGEMU_PERF_TB_COMPILE_TICKS, /* translate+assemble time per TB */
    LOONGEMU_PERF_TB_EXEC_TICKS,    /* time spent inside generated code */
    LOONGEMU_PERF_MMIO_TICKS,       /* ticks inside the MMIO dispatch helpers */
    LOONGEMU_PERF_RECOVERY_TICKS,   /* ticks spent replaying recovery bindings */
    LOONGEMU_PERF_HELPER_CALLS,     /* emitted-code calls into a runtime helper */
    LOONGEMU_PERF_COUNTERS
};

#ifdef LOONGEMU_PERF
void loongemu_perf_reset(void);
void loongemu_perf_record_tb(uint64_t host_insns, uint64_t guest_insns);
uint64_t loongemu_perf_tb_count(void);
uint64_t loongemu_perf_host_insns(void);
uint64_t loongemu_perf_guest_insns(void);
double loongemu_perf_host_insns_per_guest_insn(void);
void loongemu_perf_count(unsigned counter, uint64_t delta);
uint64_t loongemu_perf_read(unsigned counter);
/*
 * Monotonic tick source for the timing counters. On a LoongArch host this is
 * the architectural stable counter; elsewhere it reports 0 so callers can
 * treat the timing counters as unavailable instead of fabricating numbers.
 */
uint64_t loongemu_perf_now(void);
#define LOONGEMU_PERF_HAS_CLOCK 1
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
static inline void loongemu_perf_count(unsigned counter, uint64_t delta)
{
    (void)counter;
    (void)delta;
}
static inline uint64_t loongemu_perf_read(unsigned counter)
{
    (void)counter;
    return 0;
}
static inline uint64_t loongemu_perf_now(void) { return 0; }
#endif

#endif
