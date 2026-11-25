/*
 * Per-hart run/wake/exit handshake. See latr-hart-sync.h and
 * docs/design/latr-smp-concurrency.md sections 3 and 5.
 *
 * Deliberately free of OS headers: the freestanding images call the same
 * functions, they simply never have a second thread to wait for.
 */
#include "latr-hart-sync.h"

void latr_hart_sync_init(LatrHartSync *sync)
{
    if (sync == 0) {
        return;
    }
    sync->command = (uint32_t)LATR_HART_PARKED;
    sync->wake = 0;
    sync->exit_request = 0;
    sync->exit_count = 0;
    sync->wait = 0;
    sync->wait_opaque = 0;
}

void latr_hart_sync_set_waiter(LatrHartSync *sync, LatrHartWaitFn wait,
                               void *opaque)
{
    if (sync == 0) {
        return;
    }
    sync->wait = wait;
    sync->wait_opaque = opaque;
}

void latr_hart_set_command(LatrHartSync *sync, LatrHartCommand command)
{
    if (sync == 0) {
        return;
    }
    __atomic_store_n(&sync->command, (uint32_t)command, __ATOMIC_RELEASE);
    /* A command change is also an event: a parked hart must observe it even if
     * no interrupt follows. */
    __atomic_add_fetch(&sync->wake, 1, __ATOMIC_ACQ_REL);
}

LatrHartCommand latr_hart_command(const LatrHartSync *sync)
{
    if (sync == 0) {
        return LATR_HART_STOP;
    }
    return (LatrHartCommand)__atomic_load_n(&sync->command, __ATOMIC_ACQUIRE);
}

void latr_hart_wake(LatrHartSync *sync)
{
    if (sync != 0) {
        __atomic_add_fetch(&sync->wake, 1, __ATOMIC_ACQ_REL);
    }
}

uint32_t latr_hart_wake_count(const LatrHartSync *sync)
{
    return sync == 0 ? 0u
                     : __atomic_load_n(&sync->wake, __ATOMIC_ACQUIRE);
}

void latr_hart_request_exit(LatrHartSync *sync)
{
    if (sync == 0) {
        return;
    }
    __atomic_store_n(&sync->exit_request, 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&sync->wake, 1, __ATOMIC_ACQ_REL);
}

bool latr_hart_take_exit(LatrHartSync *sync)
{
    if (sync == 0) {
        return false;
    }
    /* Exchange, not load-then-store: two observers must not both see the same
     * request as theirs to honour. */
    if (__atomic_exchange_n(&sync->exit_request, 0u, __ATOMIC_ACQ_REL) == 0) {
        return false;
    }
    __atomic_add_fetch(&sync->exit_count, 1, __ATOMIC_RELAXED);
    return true;
}

uint32_t latr_hart_exit_pending(const LatrHartSync *sync)
{
    return sync == 0 ? 0u
                     : __atomic_load_n(&sync->exit_request, __ATOMIC_ACQUIRE);
}

LatrHartCommand latr_hart_park_wait(LatrHartSync *sync, uint32_t spin_limit)
{
    uint32_t seen;

    if (sync == 0) {
        return LATR_HART_STOP;
    }
    /*
     * Snapshot the event counter before looking at the command: an event that
     * arrives between the two makes the load below see the new wake counter,
     * so the loop iterates instead of sleeping through it.
     */
    seen = __atomic_load_n(&sync->wake, __ATOMIC_ACQUIRE);
    if (sync->wait != 0) {
        if (!sync->wait(sync->wait_opaque, &seen, spin_limit)) {
            /* The waiter timed out; report the command so the caller can
             * decide whether to keep parking or fail. */
            return latr_hart_command(sync);
        }
        return latr_hart_command(sync);
    }
    for (;;) {
        uint32_t now;

        if (latr_hart_command(sync) != LATR_HART_PARKED) {
            return latr_hart_command(sync);
        }
        if (sync->exit_request != 0) {
            return LATR_HART_RUN;
        }
        now = __atomic_load_n(&sync->wake, __ATOMIC_ACQUIRE);
        if (now != seen) {
            return LATR_HART_RUN;
        }
        if (spin_limit == 0) {
            return latr_hart_command(sync);
        }
        spin_limit--;
        __asm__ volatile("" ::: "memory");
    }
}

int latr_hart_next_action(LatrHartSync *sync)
{
    if (sync == 0) {
        return LATR_HART_NEXT_STOP;
    }
    /* A stop wins over a yield: the caller must not keep dispatching. */
    if (latr_hart_command(sync) == LATR_HART_STOP) {
        return LATR_HART_NEXT_STOP;
    }
    if (latr_hart_take_exit(sync)) {
        return LATR_HART_NEXT_YIELD;
    }
    if (latr_hart_command(sync) == LATR_HART_PARKED) {
        return LATR_HART_NEXT_YIELD; /* idle until an event arrives */
    }
    return LATR_HART_NEXT_DISPATCH;
}

uint32_t latr_hart_exit_count(const LatrHartSync *sync)
{
    return sync == 0 ? 0u
                     : (uint32_t)__atomic_load_n(&sync->exit_count,
                                                 __ATOMIC_RELAXED);
}
