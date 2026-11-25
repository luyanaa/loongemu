/*
 * Host harness for the LATR machine dispatcher (design:
 * docs/design/latr-smp-concurrency.md, phases P1a/P1b).
 *
 * Why this is a *scripted* machine and not a guest run: the translator emits
 * LoongArch words, and the trampoline that jumps into them (`latr_enter_tb`,
 * bare-metal/LoongArch asm) cannot exist on an x86/AArch64 host. Guest
 * execution therefore stays in the bare-metal images; this harness replaces
 * `enter_tb` with a scripted block model and exercises everything that runs in
 * C around it.
 *
 * It runs both scheduler shapes the design defines:
 *
 *   - cooperative (`latr_machine_run`, one caller thread): every hart runs in
 *     turn, which is what the freestanding build does;
 *   - threaded (`latr_machine_run_hart` per hart, one pthread each, `park_wait`
 *     for idle harts). This is the shape the P1b scheduler needs, and the only
 *     place the fence-ack wait, the WFI park and the join path are exercised
 *     with real concurrency.
 *
 * The script is keyed by hartid and drives the same entry points generated
 * code would (latr_system_mmio_write for CLINT and the exit device,
 * env->wfi_pending for WFI, env->sfence_pending for SFENCE.VMA), so a
 * scheduler bug surfaces here without a LoongArch host.
 */
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* The translator headers pull in QEMU-style shims whose stdio lacks
 * setvbuf/stderr, so the harness keeps its output to a single printf at the
 * end (flushed at exit). */
#include <string.h>
#include <stdint.h>
#include "latr-machine.h"
#include "mmu.h"

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

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t machine_work[(2u << 20) * 4]; /* 2 harts: arena + staging each */
static LatrMachine machine;

/* Valid RISC-V instructions for the scripted entry PCs: the dispatcher only
 * needs to decode and translate a block, it never executes the words here. */
static const uint32_t firmware_words[] = {
    0x00000013u, /* nop */
    0x00000013u, /* nop */
    0x0000006fu, /* j . */
};
#define FW_PC LATR_MACHINE_FIRMWARE_BASE
#define FW_LOOP (LATR_MACHINE_FIRMWARE_BASE + 8u)
#define HART1_DISC 0x80000040u /* hart 1's "discovered" marker */

/* Script state. */
static volatile uint64_t vclock_start;
static volatile int hart0_ipi_sent;
static volatile int hart1_woke;
static volatile int hart1_ready;   /* hart 1 is dispatching, not parked */
static volatile int hart1_wants_wfi; /* scripted WFI: the thread parks */
static volatile int timer_fired;   /* hart 1 took a timer interrupt */
static volatile int timer_armed;
static volatile int send_ipi;      /* hart 0 sends MSIP to hart 1 */
static volatile int send_fence;    /* hart 0 also emits SFENCE.VMA (threads) */
static volatile uint32_t exit_value;

static void guest_write(uint64_t pa, unsigned size, uint64_t value)
{
    latr_system_mmio_write(&machine.sys, pa, size, value);
}

/* Replacement for the LoongArch trampoline. */
static void scripted_enter_tb(void *envp, void *code)
{
    CPUArchState *env = (CPUArchState *)envp;

    (void)code;
    if (env->hartid == 0) {
        if (!hart0_ipi_sent && send_ipi) {
            /* Only in threaded mode does it pay to wait for the other hart: in
             * the cooperative pass it cannot progress while this block runs. */
            if (send_fence) {
                while (!hart1_ready) {
                }
            }
            /* What generated code does for `sd zero, 0(clint+4)`: an IPI. */
            guest_write(LATR_SYSTEM_CLINT_BASE + 4u, 4, 1);
            hart0_ipi_sent = 1;
            if (send_fence) {
                /* A guest SFENCE.VMA without an ASID (asid 0 = global). The
                 * issuer blocks in latr_tb_fence_complete until every other
                 * hart has acknowledged; the publication itself wakes parked
                 * harts, which is what lets a WFI hart ack at all. */
                env->sfence_asid = 0;
                env->sfence_pending = 1;
            }
        }
        if (hart1_woke) {
            exit_value = 0x5555;
            guest_write(LATR_SYSTEM_EXIT_BASE, 4, exit_value);
        }
        env->pc = FW_LOOP;
        return;
    }
    /* Hart 1: reach a safe point (ack the fence), then park in WFI until the
     * IPI has been delivered and "handle" it. */
    if (env->csr[0x344] & (UINT64_C(1) << 3)) {
        hart1_woke = 1;
        guest_write(LATR_SYSTEM_CLINT_BASE + 4u, 4, 0); /* clear MSIP */
        env->pc = HART1_DISC;
        return;
    }
    if (env->csr[0x344] & (UINT64_C(1) << 7)) {
        /* MTIP: the ticker woke a parked hart whose comparator came due. A real
         * guest would disable the timer here; this one records it and re-arms
         * so the next comparator also fires. */
        timer_fired = 1;
        env->pc = HART1_DISC;
        return;
    }
    hart1_ready = 1;
    env->pc = FW_PC;
    /* WFI: report it to the runtime (clock fast-forward) and tell this
     * harness's hart thread to park until a wake arrives. */
    env->wfi_pending = 1;
    hart1_wants_wfi = 1;
}

static int setup_machine(void)
{
    if (!latr_machine_init(&machine, ram, sizeof(ram), 2, machine_work,
                           sizeof(machine_work))) {
        return 0;
    }
    latr_machine_set_enter_tb(&machine, scripted_enter_tb);
    if (!latr_machine_load_firmware(&machine,
                                    (const uint8_t *)firmware_words,
                                    sizeof(firmware_words))) {
        return 0;
    }
    if (!latr_machine_build_dtb(&machine)) {
        return 0;
    }
    /* Hart 1's "handler" PC must decode for a block to be translated. */
    memcpy(ram + (HART1_DISC - LATR_SYSTEM_RAM_BASE), firmware_words,
           sizeof(firmware_words));
    /* Let hart 1 take the software interrupt: mtvec + mie.MSIE + mstatus.MIE,
     * which a real guest would program in its init block. */
    {
        LatrMachineHart *h1 = &machine.harts[1];

        h1->env.csr[0x305] = HART1_DISC;           /* mtvec */
        h1->env.csr[0x304] |= (UINT64_C(1) << 3);  /* mie.MSIE */
        h1->env.csr[0x300] |= (UINT64_C(1) << 3);  /* mstatus.MIE */
    }
    return latr_machine_boot(&machine);
}

static void reset_scenario(void)
{
    machine.sys.exit_request = 0;
    exit_value = 0;
    hart0_ipi_sent = 0;
    hart1_woke = 0;
    hart1_ready = 0;
    hart1_wants_wfi = 0;
    send_ipi = 0;
    send_fence = 0;
    timer_fired = 0;
    vclock_start = loongemu_vclock_now_ns(&machine.sys.vclock);
}

/* --- cooperative scheduler (one caller thread) ------------------------- */

static void scenario_cooperative(void)
{
    int rc;

    reset_scenario();
    send_ipi = 1;
    rc = latr_machine_run(&machine, 64);
    CHECK(machine.sys.exit_request == 0x5555, "cooperative: exit value");
    CHECK(machine.harts[0].tbs > 0 && machine.harts[1].tbs > 0,
          "cooperative: both harts dispatched blocks");
    CHECK(latr_machine_wake_events(&machine) == 1,
          "cooperative: exactly one CLINT MSIP reached the machine");
    CHECK(latr_hart_exit_count(&machine.harts[1].park) == 1,
          "cooperative: the woken hart honoured its exit request");
    CHECK(hart1_woke == 1, "cooperative: hart 1 came back from WFI");
    CHECK(loongemu_vclock_now_ns(&machine.sys.vclock) > vclock_start,
          "cooperative: WFI fast-forwarded the clock");
    CHECK(machine.harts[0].last_code != 0 && machine.harts[1].last_code != 0,
          "cooperative: both harts produced host code");
}

/* --- threaded scheduler: one pthread per hart -------------------------- */

typedef struct HartThread {
    LatrMachine *m;
    uint32_t id;
    pthread_t thread;
    volatile int exited;
    uint32_t dispatched;
} HartThread;

/*
 * The service loop a VMM thread runs: dispatch while RUN, park on the hart's
 * sync word when idle, leave on STOP. Exactly the shape the design describes
 * for P1b; the machine itself owns no threads.
 */
static void *hart_main(void *arg)
{
    HartThread *ht = (HartThread *)arg;

    for (;;) {
        LatrHartCommand cmd = latr_hart_command(&ht->m->harts[ht->id].park);

        if (cmd == LATR_HART_STOP) {
            break;
        }
        if (cmd == LATR_HART_PARKED || hart1_wants_wfi) {
            /* WFI: park until an interrupt source wakes this hart. The spin
             * bound keeps a protocol bug from hanging the test. */
            if (latr_hart_park_wait(&ht->m->harts[ht->id].park, 1u << 20) ==
                LATR_HART_STOP) {
                break;
            }
            if (ht->id == 1) {
                hart1_wants_wfi = 0;
            }
            continue;
        }
        {
            int ran = latr_machine_run_hart(ht->m, ht->id, 1);

            if (ran == 0) {
                break; /* machine failed */
            }
            if (ran == 3) {
                break; /* STOP honoured */
            }
            if (ran == 1) {
                ht->dispatched++;
            }
        }
        if (ht->m->sys.exit_request != 0) {
            break; /* guest asked to power off */
        }
    }
    ht->exited = 1;
    return NULL;
}

typedef struct Ticker {
    LatrMachine *m;
    pthread_t thread;
    volatile int stop;
} Ticker;

static void *ticker_main(void *arg)
{
    Ticker *t = (Ticker *)arg;

    while (!t->stop) {
        latr_machine_tick(t->m, UINT64_C(1000));
        struct timespec ts = {0, 100000}; /* 0.1 ms */

        nanosleep(&ts, 0);
    }
    return 0;
}

static void scenario_threaded(void)
{
    HartThread hts[2];
    uint64_t fence_before;
    uint64_t timeouts_before;
    int started = 1;

    reset_scenario();
    send_ipi = 1;
    send_fence = 1;
    fence_before = latr_tb_fence_seq();
    timeouts_before = latr_tb_fence_timeouts();

    for (uint32_t i = 0; i < 2; i++) {
        hts[i].m = &machine;
        hts[i].id = i;
        hts[i].exited = 0;
        hts[i].dispatched = 0;
        /* Each hart's waiter is a plain poll in this harness; the real VMM
         * installs a condvar-backed wait, which latr-hart-sync also tests. */
        latr_hart_sync_set_waiter(&machine.harts[i].park, 0, 0);
        if (pthread_create(&hts[i].thread, 0, hart_main, &hts[i]) != 0) {
            started = 0;
            break;
        }
    }
    CHECK(started, "threaded: one thread per hart");

    /* Let them run until the guest powers off or both exit on STOP. Bounded:
     * a scheduler bug must fail the test, not hang CI. */
    for (int spin = 0; spin < 2000; spin++) {
        if (machine.sys.exit_request != 0) {
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (hts[i].exited) {
                continue;
            }
        }
        if (hts[0].exited && hts[1].exited) {
            break;
        }
        struct timespec ts = {0, 1000000}; /* 1 ms */

        nanosleep(&ts, 0);
    }

    CHECK(machine.sys.exit_request == 0x5555,
          "threaded: the guest powered off through the exit device");
    CHECK(latr_machine_wake_events(&machine) >= 1,
          "threaded: the CLINT MSIP write woke the parked hart");
    CHECK(hts[1].dispatched > 0 && hts[0].dispatched > 0,
          "threaded: both harts dispatched blocks on their own threads");
    CHECK(latr_tb_fence_seq() > fence_before,
          "threaded: SFENCE.VMA published a fence sequence");
    CHECK(latr_tb_fence_wait(latr_tb_fence_seq(), UINT64_C(1) << 20) == 0,
          "threaded: the fence completed (every hart acknowledged)");
    CHECK(latr_tb_fence_timeouts() == timeouts_before,
          "threaded: no ack timeout was needed for this fence");

    /* STOP both harts and join: the scheduler's shutdown path. */
    for (uint32_t i = 0; i < 2; i++) {
        latr_hart_set_command(&machine.harts[i].park, LATR_HART_STOP);
        latr_hart_wake(&machine.harts[i].park);
    }
    for (uint32_t i = 0; i < 2; i++) {
        pthread_join(hts[i].thread, 0);
        CHECK(hts[i].exited == 1, "threaded: hart thread exited on STOP");
    }
    CHECK(machine.harts[0].online && machine.harts[1].online,
          "threaded: a guest exit request is not a hart stop");
}

/*
 * A hart parked in WFI is woken by its own timer: the ticker thread advances the
 * clock, sees the comparator has come due, and wakes that hart. Without the
 * ticker's wake this hart would sleep through its interrupt forever.
 */
static void scenario_timer_wake(void)
{
    Ticker ticker;
    HartThread ht1;
    uint64_t mtime_before;

    reset_scenario();
    send_ipi = 0;
    send_fence = 0;
    /* Restart both harts: the threaded scenario stopped them. */
    for (uint32_t i = 0; i < 2; i++) {
        machine.harts[i].online = 1;
        latr_hart_set_command(&machine.harts[i].park, LATR_HART_RUN);
    }
    latr_hart_sync_set_waiter(&machine.harts[1].park, 0, 0);
    memset(&ht1, 0, sizeof(ht1));
    ht1.m = &machine;
    ht1.id = 1;
    mtime_before = machine.sys.clint.mtime;

    /* Hart 1: enable MTIE + MIE and arm a comparator just ahead of the clock. */
    machine.harts[1].env.csr[0x304] |= (UINT64_C(1) << 7);
    machine.harts[1].env.csr[0x300] |= (UINT64_C(1) << 3);
    machine.harts[1].env.csr[0x305] = HART1_DISC;
    machine.sys.clint.mtimecmp[1] =
        loongemu_vclock_now_ns(&machine.sys.vclock) + 2000;

    memset(&ticker, 0, sizeof(ticker));
    ticker.m = &machine;
    CHECK(pthread_create(&ht1.thread, 0, hart_main, &ht1) == 0,
          "timer: hart 1 thread");
    CHECK(pthread_create(&ticker.thread, 0, ticker_main, &ticker) == 0,
          "timer: ticker thread");

    for (int spin = 0; spin < 2000 && !timer_fired; spin++) {
        struct timespec ts = {0, 1000000};

        nanosleep(&ts, 0);
    }
    CHECK(timer_fired == 1,
          "timer: a parked hart is woken by its own timer comparator");
    CHECK(machine.sys.clint.mtime > mtime_before, "timer: mtime advanced");
    CHECK(latr_system_lock_failures(&machine.sys) == 0,
          "timer: no device-lock acquisition failed");

    latr_hart_set_command(&machine.harts[1].park, LATR_HART_STOP);
    latr_hart_wake(&machine.harts[1].park);
    pthread_join(ht1.thread, 0);
    ticker.stop = 1;
    pthread_join(ticker.thread, 0);
}

/*
 * Lock order (design section 6): device lock -> DMA lock -> reservation epoch,
 * and a device step may call the DMA path while the reverse never happens.
 * Two threads hammer both sides; the bounded acquisition turns any inversion
 * into a counted failure rather than a hang.
 */
typedef struct LockHammer {
    LatrMachine *m;
    uint32_t iterations;
    volatile int done;
} LockHammer;

static void *mmio_hammer(void *arg)
{
    LockHammer *h = (LockHammer *)arg;

    for (uint32_t i = 0; i < h->iterations; i++) {
        latr_system_mmio_write(&h->m->sys, LATR_SYSTEM_CLINT_BASE + 4u, 4,
                               (i & 1u));
        latr_system_mmio_write(&h->m->sys, LATR_SYSTEM_CLINT_BASE + 4u, 4, 0);
    }
    h->done++;
    return 0;
}

static void *dma_hammer(void *arg)
{
    LockHammer *h = (LockHammer *)arg;
    uint8_t payload[16];

    memset(payload, 0xa5, sizeof(payload));
    for (uint32_t i = 0; i < h->iterations; i++) {
        loongemu_guest_dma_write(&h->m->sys.dma,
                                 LATR_SYSTEM_RAM_BASE + 0x2000u, payload,
                                 sizeof(payload));
    }
    h->done++;
    return 0;
}

static void scenario_lock_order(void)
{
    LockHammer bus = {&machine, 5000, 0};
    LockHammer dev = {&machine, 5000, 0};
    pthread_t t_bus;
    pthread_t t_dev;
    uint64_t epoch_before;
    uint64_t acquisitions_before;

    /* No harts: this is the device layer alone, so a failure here cannot be
     * blamed on the dispatcher. */
    for (uint32_t i = 0; i < 2; i++) {
        latr_hart_set_command(&machine.harts[i].park, LATR_HART_STOP);
    }
    epoch_before = __atomic_load_n(&latr_guest_store_lock, __ATOMIC_ACQUIRE);
    acquisitions_before = machine.sys.device_lock_acquisitions;
    machine.sys.device_lock_failures = 0;

    {
        int ok_bus = pthread_create(&t_bus, 0, mmio_hammer, &bus) == 0;
        int ok_dev = pthread_create(&t_dev, 0, dma_hammer, &dev) == 0;

        CHECK(ok_bus && ok_dev, "lock order: two hammer threads");
        if (!ok_bus || !ok_dev) {
            return;
        }
    }
    pthread_join(t_bus, 0);
    pthread_join(t_dev, 0);

    CHECK(bus.done == 1 && dev.done == 1, "lock order: both threads finished");
    CHECK(machine.sys.device_lock_failures == 0,
          "lock order: no bounded acquisition failed");
    CHECK(__atomic_load_n(&machine.sys.device_lock, __ATOMIC_ACQUIRE) == 0,
          "lock order: the device lock is released");
    CHECK(machine.sys.device_lock_acquisitions > acquisitions_before,
          "lock order: MMIO actually took the device lock");
    {
        /* DMA stores advance the reservation epoch; a writer lock left behind
         * would make the epoch odd forever. */
        uint64_t epoch = __atomic_load_n(&latr_guest_store_lock,
                                         __ATOMIC_ACQUIRE);

        CHECK((epoch & 1u) == 0, "lock order: no writer lock left held");
        CHECK(epoch > epoch_before, "lock order: DMA advanced the epoch");
    }
}

/*
 * Reproduce the machine's own translation of a guest block: cflags = 0 (stop at
 * terminators), the hart's context and code buffer, guest memory through the
 * machine's map. Used to keep translator hangs out of the image-level runs.
 */
static const uint32_t guest_block_words[] = {
    0x00828f13u, /* addi t5, t0, 8 */
    0x01028f93u, /* addi t6, t0, 16 */
    0x00800313u, /* li t1, 8 */
    0x006f302fu, /* amoadd.d zero, t1, (t5) */
    0x006f302fu,
    0x006f302fu,
    0x006f302fu,
    0x00000393u, /* li t2, 0 */
    0x01000e13u, /* li t3, 16 */
    0x100fb32fu, /* lr.d t1, (t6) */
    0x00130313u, /* addi t1, t1, 1 */
    0x186fbeafu, /* sc.d t4, t1, (t6) */
    0xfe0e9ae3u, /* bnez t4, -16 */
};

static void scenario_translate_guest_block(void)
{
    LatrMachineHart *h = &machine.harts[0];
    TranslationBlock tb;
    int limit = 64;
    int words;

    /* The exact AMO+LR/SC block the guest executes: translating it hung the
     * machine before the fix, so this keeps the shape reproducible on the host. */
    memcpy(ram + 0x10u, guest_block_words, sizeof(guest_block_words));
    memset(&tb, 0, sizeof(tb));
    latr_context_bind(h->ctx);
    latr_set_cpu_env(&h->env);
    tb.pc = LATR_SYSTEM_RAM_BASE + 0x10u;
    tb.tc.ptr = h->code_buf;
    tb.cflags = 0;
    target_disasm(&tb, &limit, &h->cpu);
    CHECK(tb.icount > 0, "translate: guest block decoded");
    tr_init(&tb);
    CHECK(tr_ir2_generate(&tb), "translate: IR2 generated");
    words = tr_ir2_assemble(h->code_buf);
    tr_fini();
    CHECK(words > 0, "translate: assembled");
}

static void scenario_stop_cooperative(void)
{
    int rc;

    machine.sys.exit_request = 0;
    hart1_woke = 0;
    hart0_ipi_sent = 1; /* no second IPI in this pass */
    /* The threaded scenario stopped both harts; this one restarts hart 0 so a
     * single stopped hart is what the pass actually observes. */
    machine.harts[0].online = 1;
    latr_hart_set_command(&machine.harts[0].park, LATR_HART_RUN);
    latr_hart_set_command(&machine.harts[1].park, LATR_HART_STOP);
    rc = latr_machine_run(&machine, 8);
    CHECK(rc == 2, "cooperative: the machine runs with one hart stopped");
    CHECK(!machine.harts[1].online, "cooperative: STOP takes the hart offline");
}

static void scenario_fini(void)
{
    latr_machine_fini(&machine);
    latr_hart_set_command(&machine.harts[0].park, LATR_HART_STOP);
    CHECK(latr_hart_command(&machine.harts[0].park) == LATR_HART_STOP,
          "fini leaves every hart with STOP published");
}

int main(void)
{
    CHECK(setup_machine(), "machine init on the host");
    scenario_cooperative();
    scenario_threaded();
    scenario_timer_wake();
    scenario_lock_order();
    scenario_translate_guest_block();
    scenario_stop_cooperative();
    scenario_fini();

    printf("latr-host-smp: %d checks, %d fails (guest blocks scripted: no "
           "LoongArch host)\n",
           checks, failures);
    return failures == 0 ? 0 : 1;
}
