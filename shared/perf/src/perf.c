#ifdef LOONGEMU_PERF
#include "../include/loongemu-perf.h"

static uint64_t perf_tb_count;
static uint64_t perf_host_insns;
static uint64_t perf_guest_insns;

void loongemu_perf_reset(void)
{
    __atomic_store_n(&perf_tb_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_host_insns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_guest_insns, 0, __ATOMIC_RELAXED);
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
