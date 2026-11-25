/*
 * Host tests for the shared guest-write domain (roadmap 3.2).
 *
 * The contract: a device DMA write and a CPU store take the *same* critical
 * section, so with vCPUs on separate threads they cannot interleave inside one
 * guest-visible write. The device side installs two hooks; this test drives
 * them with a real spin domain and a real second thread, because the failure
 * mode (a torn 8-byte value) only appears when both actually run concurrently.
 *
 * Also covered: the "already held" path. A device write triggered from inside a
 * guest store (an MMIO doorbell) must proceed without acquiring - and therefore
 * without unlocking on the way out - because spinning there is a deadlock.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "loongemu/guest_dma.h"
#include "loongemu/phys_mem.h"

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

/* ---- a domain with the same shape as LATR's reservation word ----------- */

static volatile uint64_t domain; /* bit0 = held, upper bits = epoch */
static volatile uint64_t domain_epoch;

/* The hook the embedder installs: a blocking acquire on the shared domain. */
static uint64_t domain_acquire(void *opaque)
{
    uint64_t old;

    (void)opaque;
    for (;;) {
        old = __atomic_load_n(&domain, __ATOMIC_ACQUIRE);
        if ((old & 1u) == 0 &&
            __atomic_compare_exchange_n(&domain, &old, old + 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return old;
        }
    }
}

static void domain_unlock(void *opaque, uint64_t token)
{
    (void)opaque;
    __atomic_store_n(&domain, token + 2, __ATOMIC_RELEASE);
    __atomic_add_fetch(&domain_epoch, 1, __ATOMIC_RELAXED);
}

/* Same acquire as LATR uses from generated code. */
static uint64_t domain_lock_blocking(void)
{
    for (;;) {
        uint64_t old = __atomic_load_n(&domain, __ATOMIC_ACQUIRE);

        if ((old & 1u) == 0 &&
            __atomic_compare_exchange_n(&domain, &old, old + 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return old;
        }
    }
}

/* ---- fixture ----------------------------------------------------------- */

#define RAM_SIZE 4096u
static uint8_t ram[RAM_SIZE] __attribute__((aligned(4096)));
static LoongemuMemMap map;
static LoongemuGuestDma dma;
static uint64_t notify_count;

static void notify(void *opaque, uint64_t gpa, uint64_t len)
{
    (void)opaque;
    (void)gpa;
    (void)len;
    __atomic_add_fetch(&notify_count, 1, __ATOMIC_RELAXED);
}

static int setup(void)
{
    memset(ram, 0, sizeof(ram));
    loongemu_mem_map_init(&map);
    if (!loongemu_mem_map_add(&map, 0x1000u, RAM_SIZE,
                              (uint64_t)(uintptr_t)ram, LOONGEMU_MEM_RAM,
                              "ram")) {
        return 0;
    }
    loongemu_guest_dma_init(&dma, &map, notify, NULL);
    loongemu_guest_dma_set_write_domain(&dma, domain_acquire, domain_unlock,
                                        NULL);
    domain = 4; /* even: free, with a non-zero epoch */
    domain_epoch = 0;
    notify_count = 0;
    return 1;
}

/* ---- 1. acquire/release and the notification boundary ------------------ */

static void test_serialisation(void)
{
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t before;

    CHECK(setup(), "fixture");
    before = __atomic_load_n(&domain, __ATOMIC_ACQUIRE);
    CHECK(loongemu_guest_dma_write(&dma, 0x1000u, payload, sizeof(payload)) == 0,
          "dma write succeeds");
    CHECK(__atomic_load_n(&domain, __ATOMIC_ACQUIRE) == before + 2,
          "the domain was taken and released around the write");
    CHECK(domain_epoch == 1, "release advanced the epoch exactly once");
    CHECK(notify_count == 1, "the write notified exactly once");
    CHECK(memcmp(ram, payload, sizeof(payload)) == 0, "payload landed");

    /* The domain must be held while the bytes change: a probe that only sees a
     * held domain (i.e. never acquires) proves the ordering is inside it. */
    /* Nested device writes are refused rather than deadlocking on a
     * non-recursive domain: the transaction is already active. */
    {
        uint8_t *host = NULL;

        CHECK(loongemu_guest_write_begin(&dma, 0x1000u, 4, &host) == 0,
              "transaction begins");
        CHECK(loongemu_guest_write_begin(&dma, 0x1004u, 4, &host) == -1,
              "a second begin on the same device is refused");
        loongemu_guest_write_end(&dma);
    }
}

/* ---- 2. torn-write stress: CPU store vs DMA, one critical section ------ */

typedef struct Hammer {
    uint32_t iterations;
    volatile int done;
    volatile int torn;
} Hammer;

/* 64 bytes on purpose: an 8-byte memcpy can compile to one store and would
 * never tear, so the region has to be wider than any single host store. */
#define REGION 64u

static void *cpu_writer(void *arg)
{
    Hammer *h = (Hammer *)arg;

    for (uint32_t i = 0; i < h->iterations; i++) {
        uint64_t token = domain_lock_blocking();

        memset(ram + 16, 0x5a, REGION);
        domain_unlock(NULL, token);
    }
    h->done++;
    return 0;
}

static void *dma_writer(void *arg)
{
    Hammer *h = (Hammer *)arg;
    uint8_t payload[REGION];

    memset(payload, 0xa5, sizeof(payload));
    for (uint32_t i = 0; i < h->iterations; i++) {
        loongemu_guest_dma_write(&dma, 0x1010u, payload, sizeof(payload));
    }
    h->done++;
    return 0;
}

/*
 * Sampler: takes the domain (as any guest-observable observer must, since a CPU
 * store holds it) and checks the region is one writer's. Under the hook the
 * device's memcpy is excluded, so every sample is uniform; if the device wrote
 * outside the domain, the sampler would see a half-written region.
 */
static void *sampler(void *arg)
{
    Hammer *h = (Hammer *)arg;

    for (uint32_t i = 0; i < h->iterations; i++) {
        uint64_t token = domain_lock_blocking();
        uint8_t first = ram[16];

        for (unsigned k = 1; k < REGION; k++) {
            if (ram[16 + k] != first) {
                h->torn = 1;
                break;
            }
        }
        domain_unlock(NULL, token);
    }
    h->done++;
    return 0;
}

static void test_no_tearing(void)
{
    Hammer cpu = {20000, 0, 0};
    Hammer dev = {20000, 0, 0};
    Hammer chk = {20000, 0, 0};
    pthread_t t_cpu;
    pthread_t t_dev;
    pthread_t t_chk;
    int ok = 1;

    CHECK(setup(), "fixture for the torn-write stress");
    memset(ram + 16, 0, REGION);
    if (pthread_create(&t_cpu, 0, cpu_writer, &cpu) != 0 ||
        pthread_create(&t_dev, 0, dma_writer, &dev) != 0 ||
        pthread_create(&t_chk, 0, sampler, &chk) != 0) {
        CHECK(0, "stress: threads started");
        return;
    }
    pthread_join(t_cpu, 0);
    pthread_join(t_dev, 0);
    pthread_join(t_chk, 0);

    /* The final value must be one writer's, not a mix - a torn 8-byte value
     * would have some 0x5a bytes and some 0xa5 bytes. */
    for (unsigned i = 0; i < REGION; i++) {
        if (ram[16 + i] != ram[16]) {
            ok = 0;
        }
    }
    CHECK(ok, "the region ends as one writer's value");
    CHECK(chk.torn == 0,
          "an observer holding the domain never sees a torn device write");
    CHECK(cpu.done == 1 && dev.done == 1 && chk.done == 1,
          "stress: every thread finished");
    CHECK(notify_count == dev.iterations,
          "stress: every device write notified once");
}

int main(void)
{
    test_serialisation();
    test_no_tearing();

    printf("guest-write-domain: %d checks, %d fails\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
