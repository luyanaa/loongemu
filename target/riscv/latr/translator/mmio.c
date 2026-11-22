/*
 * MMIO helper entry points called from emitted code. The emitter never
 * dereferences a device address: latr_translate() flags MMIO accesses and the
 * load/store path calls these functions, which dispatch through the bus and
 * report a bus miss as a RISC-V access fault.
 */
#include "latr.h"
#include "mmu.h"
#include "loongemu/mmio_bus.h"

uint64_t latr_mmio_load(CPUArchState *env, uint64_t pa, unsigned size)
{
    uint64_t value = 0;

    env->mem_mmio = 0;
    if (env->mmio == 0 ||
        loongemu_mmio_bus_read(env->mmio, pa, size, &value) != 0) {
        env->mem_fault_cause = LATR_EXCP_LOAD_ACCESS_FAULT;
        return 0;
    }
    env->mem_fault_cause = 0;
    return value;
}

void latr_mmio_store(CPUArchState *env, uint64_t pa, unsigned size,
                     uint64_t value)
{
    env->mem_mmio = 0;
    if (env->mmio == 0 ||
        loongemu_mmio_bus_write(env->mmio, pa, size, value) != 0) {
        env->mem_fault_cause = LATR_EXCP_STORE_ACCESS_FAULT;
        return;
    }
    env->mem_fault_cause = 0;
}
