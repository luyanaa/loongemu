#ifdef LOONGEMU_PERF
#include "../include/loongemu-perf.h"

static uint64_t perf_tb_count;
static uint64_t perf_host_insns;
static uint64_t perf_guest_insns;
static uint64_t perf_counters[LOONGEMU_PERF_COUNTERS];

void loongemu_perf_reset(void)
{
    __atomic_store_n(&perf_tb_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_host_insns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_guest_insns, 0, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < LOONGEMU_PERF_COUNTERS; i++) {
        __atomic_store_n(&perf_counters[i], 0, __ATOMIC_RELAXED);
    }
}

void loongemu_perf_count(unsigned counter, uint64_t delta)
{
    if (counter >= LOONGEMU_PERF_COUNTERS || delta == 0) {
        return;
    }
    __atomic_fetch_add(&perf_counters[counter], delta, __ATOMIC_RELAXED);
}

uint64_t loongemu_perf_read(unsigned counter)
{
    if (counter >= LOONGEMU_PERF_COUNTERS) {
        return 0;
    }
    return __atomic_load_n(&perf_counters[counter], __ATOMIC_RELAXED);
}

uint64_t loongemu_perf_now(void)
{
#if defined(__loongarch__)
    uint64_t tick;

    /* rdtime.d: the stable counter. Frequency is unknown here on purpose; the
     * timing counters are compared against a baseline run, not converted to
     * seconds. */
    __asm__ volatile("rdtime.d %0, $zero" : "=r"(tick)::"memory");
    return tick;
#else
    return 0;
#endif
}

void loongemu_perf_record_tb(uint64_t host_insns, uint64_t guest_insns)
{
    if (guest_insns == 0) {
        return;
    }
    __atomic_fetch_add(&perf_tb_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&perf_host_insns, host_insns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&perf_guest_insns, guest_insns, __ATOMIC_RELAXED);
}

uint64_t loongemu_perf_tb_count(void)
{
    return __atomic_load_n(&perf_tb_count, __ATOMIC_RELAXED);
}

uint64_t loongemu_perf_host_insns(void)
{
    return __atomic_load_n(&perf_host_insns, __ATOMIC_RELAXED);
}

uint64_t loongemu_perf_guest_insns(void)
{
    return __atomic_load_n(&perf_guest_insns, __ATOMIC_RELAXED);
}

double loongemu_perf_host_insns_per_guest_insn(void)
{
    uint64_t guest_insns = loongemu_perf_guest_insns();

    if (guest_insns == 0) {
        return 0.0;
    }
    return (double)loongemu_perf_host_insns() / (double)guest_insns;
}

#endif
