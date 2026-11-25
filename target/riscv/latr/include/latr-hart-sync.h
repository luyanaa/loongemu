#ifndef LATR_HART_SYNC_H
#define LATR_HART_SYNC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Per-hart run/wake/exit handshake (design:
 * docs/design/latr-smp-concurrency.md sections 3 and 5).
 *
 * The runtime has three ways to influence another hart:
 *   - `command`: PARKED (do not dispatch), RUN, STOP (leave the thread);
 *   - `wake`: an event counter, bumped by an interrupt source that has made
 *     progress possible. It is a counter, not a flag, because a wake that
 *     arrives before the waiter parks must not be lost;
 *   - `exit_request`: "finish the block you are in and come back to the
 *     dispatcher". Taken (and cleared) at a safe point, never mid-block.
 *
 * Everything here is plain C11 atomics so it compiles both in the hosted
 * scheduler and in the freestanding images, where there is no clock, no futex
 * and no other thread: in that build the same calls are still made, they just
 * never block. A hosted scheduler installs `wait` to block properly; without a
 * waiter `latr_hart_park_wait()` polls.
 */
typedef enum LatrHartCommand {
    LATR_HART_PARKED = 0, /* idle until an event arrives */
    LATR_HART_RUN = 1,    /* dispatch guest code */
    LATR_HART_STOP = 2    /* leave the scheduler thread */
} LatrHartCommand;

typedef bool (*LatrHartWaitFn)(void *opaque, uint32_t *wake_seen,
                               uint32_t spin_limit);

typedef struct LatrHartSync {
    volatile uint32_t command;
    volatile uint32_t wake;
    volatile uint32_t exit_request;
    /* Number of times the exit request was taken; diagnostics only. */
    volatile uint64_t exit_count;
    /* Hosted schedulers install a blocking wait; NULL polls. */
    LatrHartWaitFn wait;
    void *wait_opaque;
} LatrHartSync;

void latr_hart_sync_init(LatrHartSync *sync);
void latr_hart_sync_set_waiter(LatrHartSync *sync, LatrHartWaitFn wait,
                               void *opaque);

/* Command transitions. Waking is what makes a parked hart look at the command. */
void latr_hart_set_command(LatrHartSync *sync, LatrHartCommand command);
LatrHartCommand latr_hart_command(const LatrHartSync *sync);

/* One event: an interrupt became pending, a device moved, the clock advanced. */
void latr_hart_wake(LatrHartSync *sync);
uint32_t latr_hart_wake_count(const LatrHartSync *sync);

/*
 * Block (or poll) until there is work or the command changes. Returns the
 * current command; on LATR_HART_RUN the caller dispatches at least once before
 * parking again. `spin_limit` bounds the polling path so a waiter with no
 * installed blocking wait cannot spin forever without reporting.
 */
LatrHartCommand latr_hart_park_wait(LatrHartSync *sync, uint32_t spin_limit);

/*
 * The dispatcher's decision at a safe point, in one place so it is testable:
 * 0 = dispatch, 1 = yielded (an exit request was honoured, do not dispatch
 * this round), 2 = stopped (leave the scheduler loop).
 */
enum { LATR_HART_NEXT_DISPATCH = 0, LATR_HART_NEXT_YIELD, LATR_HART_NEXT_STOP };
int latr_hart_next_action(LatrHartSync *sync);

/* "Come back to the dispatcher after the block you are in." */
void latr_hart_request_exit(LatrHartSync *sync);
/* Test-and-clear; true exactly once per request. */
bool latr_hart_take_exit(LatrHartSync *sync);
uint32_t latr_hart_exit_pending(const LatrHartSync *sync);
/* How many requests were honoured (diagnostics: a hart that never takes its
 * exit request is the one a fence issuer will time out on). */
uint32_t latr_hart_exit_count(const LatrHartSync *sync);

#endif
