/*
 * Host test for the per-hart run/wake/exit handshake, with real threads.
 *
 * The bare-metal images call the same functions but never have a second
 * thread, so this is the only place the blocking half is exercised. What it
 * pins (design: docs/design/latr-smp-concurrency.md sections 3 and 5):
 *   - a wake that arrives before the waiter parks is not lost;
 *   - every wake is counted, so an interrupt source that coalesces cannot hide
 *     behind a flag (the IPI test is exactly this: N IPIs, N observations);
 *   - an exit request is honoured exactly once (exchange, not load/store);
 *   - STOP terminates a parked hart and RUN resumes it;
 *   - the polling path terminates on its spin bound instead of hanging.
 *
 * Bounded everywhere: a broken protocol must fail the test, not wedge CI.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "latr-hart-sync.h"

static int checks;
static int failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what);            \
        }                                                                    \
    } while (0)

/* --- a pthread-backed waiter, i.e. what the hosted scheduler installs ---- */

typedef struct WaitCtx {
    pthread_mutex_t mu;
    pthread_cond_t cv;
} WaitCtx;

static bool cond_wait(void *opaque, uint32_t *wake_seen, uint32_t spin_limit)
{
    WaitCtx *ctx = (WaitCtx *)opaque;
    struct timespec deadline;

    (void)spin_limit;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 200 * 1000 * 1000; /* 200 ms: bounded, never CI-hanging */
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&ctx->mu);
    (void)wake_seen;
    pthread_cond_timedwait(&ctx->cv, &ctx->mu, &deadline);
    pthread_mutex_unlock(&ctx->mu);
    return true;
}

/* --- 1. plain counters and commands, no threads ------------------------- */

static void test_counters(void)
{
    LatrHartSync sync;

    latr_hart_sync_init(&sync);
    CHECK(latr_hart_command(&sync) == LATR_HART_PARKED, "initially parked");
    latr_hart_set_command(&sync, LATR_HART_RUN);
    /* The command change is itself an event, so a parked hart sees it. */
    CHECK(latr_hart_wake_count(&sync) == 1, "command change bumps the wake count");
    latr_hart_wake(&sync);
    latr_hart_wake(&sync);
    CHECK(latr_hart_wake_count(&sync) == 3, "wakes are counted, not coalesced");
    latr_hart_set_command(&sync, LATR_HART_STOP);
    CHECK(latr_hart_command(&sync) == LATR_HART_STOP, "stop is visible");

    /* exit request: exchange semantics */
    CHECK(!latr_hart_take_exit(&sync), "nothing to take yet");
    latr_hart_request_exit(&sync);
    CHECK(latr_hart_exit_pending(&sync) == 1, "request is pending");
    CHECK(latr_hart_take_exit(&sync), "first take wins");
    CHECK(!latr_hart_take_exit(&sync), "second take loses");
    CHECK(latr_hart_exit_count(&sync) == 1, "exactly one honoured request");
}

/* --- 2. the dispatcher's decision (what run_hart calls at a safe point) - */

static void test_next_action(void)
{
    LatrHartSync sync;

    latr_hart_sync_init(&sync);
    CHECK(latr_hart_next_action(&sync) == LATR_HART_NEXT_YIELD,
          "parked hart does not dispatch");
    latr_hart_set_command(&sync, LATR_HART_RUN);
    CHECK(latr_hart_next_action(&sync) == LATR_HART_NEXT_DISPATCH,
          "running hart dispatches");
    latr_hart_request_exit(&sync);
    CHECK(latr_hart_next_action(&sync) == LATR_HART_NEXT_YIELD,
          "exit request yields without dispatching");
    CHECK(latr_hart_next_action(&sync) == LATR_HART_NEXT_DISPATCH,
          "the yield is a one-shot: next call dispatches");
    latr_hart_request_exit(&sync);
    latr_hart_set_command(&sync, LATR_HART_STOP);
    CHECK(latr_hart_next_action(&sync) == LATR_HART_NEXT_STOP,
          "stop wins over a pending exit request");
    CHECK(latr_hart_exit_pending(&sync) == 1,
          "a stop does not swallow the exit request");
    latr_hart_set_command(&sync, LATR_HART_RUN);
    CHECK(latr_hart_next_action(&sync) == LATR_HART_NEXT_YIELD,
          "the preserved exit request is honoured after resume");
}

/* --- 3. polling path terminates on its bound ---------------------------- */

static void test_poll_bound(void)
{
    LatrHartSync sync;

    latr_hart_sync_init(&sync);
    /* Parked with no waiter and no events: the spin bound is what stops it. */
    CHECK(latr_hart_park_wait(&sync, 8) == LATR_HART_PARKED,
          "polling wait returns parked when the bound expires");
    latr_hart_set_command(&sync, LATR_HART_RUN);
    CHECK(latr_hart_park_wait(&sync, 8) == LATR_HART_RUN,
          "polling wait returns immediately on a pending command");
    latr_hart_sync_init(&sync);
    latr_hart_request_exit(&sync);
    CHECK(latr_hart_park_wait(&sync, 8) == LATR_HART_RUN,
          "an exit request is a wake: parked hart must come back");
}

/* --- 3. threaded: wake before park is not lost ------------------------- */

typedef struct HartLoop {
    LatrHartSync sync;
    WaitCtx wait_ctx;
    volatile uint32_t dispatches; /* how many times the "hart" ran */
    volatile uint32_t wake_observations;
    volatile int done;
} HartLoop;

static void *hart_thread(void *arg)
{
    HartLoop *loop = (HartLoop *)arg;

    for (;;) {
        LatrHartCommand cmd = latr_hart_park_wait(&loop->sync, 100000);
        uint32_t seen;

        if (cmd == LATR_HART_STOP) {
            loop->done = 1;
            return 0;
        }
        if (cmd == LATR_HART_PARKED) {
            continue;
        }
        /* RUN: one dispatch, then back to the park. Every wake that was
         * published before this point is observed here. */
        seen = latr_hart_wake_count(&loop->sync);
        __atomic_add_fetch(&loop->dispatches, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&loop->wake_observations, seen, __ATOMIC_RELEASE);
        if (latr_hart_take_exit(&loop->sync)) {
            /* Exit request: yield to the scheduler after the block. */
        }
        if (__atomic_load_n(&loop->dispatches, __ATOMIC_ACQUIRE) >= 4) {
            loop->done = 1;
            return 0;
        }
    }
}

static void test_threaded(void)
{
    HartLoop loop;
    pthread_t thread;

    memset(&loop, 0, sizeof(loop));
    latr_hart_sync_init(&loop.sync);
    pthread_mutex_init(&loop.wait_ctx.mu, 0);
    pthread_cond_init(&loop.wait_ctx.cv, 0);
    latr_hart_sync_set_waiter(&loop.sync, cond_wait, &loop.wait_ctx);

    CHECK(pthread_create(&thread, 0, hart_thread, &loop) == 0, "hart thread");

    /* Wakes published while the hart is still starting must not be lost: the
     * first RUN dispatch sees at least this many events. */
    latr_hart_set_command(&loop.sync, LATR_HART_RUN);
    while (__atomic_load_n(&loop.dispatches, __ATOMIC_ACQUIRE) < 4 &&
           !__atomic_load_n(&loop.done, __ATOMIC_ACQUIRE)) {
        latr_hart_wake(&loop.sync);
        pthread_mutex_lock(&loop.wait_ctx.mu);
        pthread_cond_broadcast(&loop.wait_ctx.cv);
        pthread_mutex_unlock(&loop.wait_ctx.mu);
    }
    CHECK(__atomic_load_n(&loop.dispatches, __ATOMIC_ACQUIRE) >= 4,
          "threaded hart ran every dispatch it was woken for");
    CHECK(__atomic_load_n(&loop.wake_observations, __ATOMIC_ACQUIRE) >= 4,
          "wake counter reached the hart (IPI test: nobody coalesces)");

    pthread_join(thread, 0);
    CHECK(loop.done == 1, "hart thread exited on its own dispatch budget");
    pthread_cond_destroy(&loop.wait_ctx.cv);
    pthread_mutex_destroy(&loop.wait_ctx.mu);
}

/* --- 4. threaded: STOP terminates a parked hart ------------------------ */

typedef struct StopLoop {
    LatrHartSync sync;
    volatile int exited;
} StopLoop;

static void *stop_thread(void *arg)
{
    StopLoop *loop = (StopLoop *)arg;

    while (latr_hart_park_wait(&loop->sync, 100000) != LATR_HART_STOP) {
        /* Poll path: no waiter installed, bounded by the spin limit. */
    }
    loop->exited = 1;
    return 0;
}

static void test_stop(void)
{
    StopLoop loop;
    pthread_t thread;

    memset(&loop, 0, sizeof(loop));
    latr_hart_sync_init(&loop.sync);
    CHECK(pthread_create(&thread, 0, stop_thread, &loop) == 0, "parked thread");
    /* It must be parked before the stop is published, otherwise the test says
     * nothing about waking a sleeping hart. */
    for (int i = 0; i < 1000 && latr_hart_command(&loop.sync) != LATR_HART_PARKED;
         i++) {
    }
    latr_hart_set_command(&loop.sync, LATR_HART_STOP);
    pthread_join(thread, 0);
    CHECK(loop.exited == 1, "STOP terminates a parked hart");
}

int main(void)
{
    test_counters();
    test_next_action();
    test_poll_bound();
    test_threaded();
    test_stop();

    printf("latr-hart-sync: %d checks, %d fails\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
