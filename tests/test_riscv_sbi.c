/* Host tests for the VMM-side SBI v2.0 dispatcher. */
#include <stdio.h>
#include <string.h>
#include "loongemu/riscv_sbi.h"

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "SBI: FAIL %s\n", name);                        \
        }                                                                   \
    } while (0)

static int call_count;
static uint64_t seen_when;
static uint64_t seen_base;
static uint32_t seen_mask;
static uint64_t seen_start;
static uint64_t seen_size;
static uint64_t seen_asid;
static uint64_t seen_hartid;
static uint64_t seen_start_addr;
static uint32_t seen_suspend;
static uint32_t seen_reset_type;
static uint32_t seen_reset_reason;
static int64_t next_error;
static uint64_t seen_debug_addr;
static uint64_t seen_debug_size;
static uint8_t seen_debug_byte;
static int32_t console_char;
static int clear_ipi_count;

static int64_t rec_debug_write(void *opaque, uint64_t addr, uint32_t size,
                               uint64_t *written)
{
    (void)opaque;
    call_count++;
    seen_debug_addr = addr;
    seen_debug_size = size;
    *written = size;
    return next_error;
}

static int64_t rec_debug_read(void *opaque, uint64_t addr, uint32_t size,
                              uint64_t *read)
{
    (void)opaque;
    call_count++;
    seen_debug_addr = addr;
    seen_debug_size = size;
    *read = size;
    return next_error;
}

static int64_t rec_debug_write_byte(void *opaque, uint8_t byte)
{
    (void)opaque;
    call_count++;
    seen_debug_byte = byte;
    return next_error;
}

static int64_t rec_console_putchar(void *opaque, uint8_t byte)
{
    (void)opaque;
    call_count++;
    seen_debug_byte = byte;
    return next_error;
}

static int64_t rec_console_getchar(void *opaque, int32_t *ch)
{
    (void)opaque;
    call_count++;
    *ch = console_char;
    return next_error;
}

static int64_t rec_clear_ipi(void *opaque)
{
    (void)opaque;
    clear_ipi_count++;
    return next_error;
}

static int64_t rec_set_timer(void *opaque, uint64_t when)
{
    (void)opaque;
    call_count++;
    seen_when = when;
    return next_error;
}

static int64_t rec_send_ipi(void *opaque, uint64_t base, uint32_t mask)
{
    (void)opaque;
    call_count++;
    seen_base = base;
    seen_mask = mask;
    return next_error;
}

static int64_t rec_fence_i(void *opaque, uint64_t base, uint32_t mask)
{
    (void)opaque;
    call_count++;
    seen_base = base;
    seen_mask = mask;
    return next_error;
}

static int64_t rec_sfence_vma(void *opaque, uint64_t base, uint32_t mask,
                              uint64_t start, uint64_t size)
{
    (void)opaque;
    call_count++;
    seen_base = base;
    seen_mask = mask;
    seen_start = start;
    seen_size = size;
    return next_error;
}

static int64_t rec_sfence_vma_asid(void *opaque, uint64_t base, uint32_t mask,
                                   uint64_t start, uint64_t size, uint64_t asid)
{
    (void)opaque;
    call_count++;
    seen_base = base;
    seen_mask = mask;
    seen_start = start;
    seen_size = size;
    seen_asid = asid;
    return next_error;
}

static int64_t rec_hart_start(void *opaque, uint64_t hartid,
                              uint64_t start_addr, uint64_t arg)
{
    (void)opaque;
    (void)arg;
    call_count++;
    seen_hartid = hartid;
    seen_start_addr = start_addr;
    return next_error;
}

static int64_t rec_hart_stop(void *opaque)
{
    (void)opaque;
    call_count++;
    return next_error;
}

static int64_t rec_hart_get_status(void *opaque, uint64_t hartid,
                                   uint32_t *status)
{
    (void)opaque;
    call_count++;
    seen_hartid = hartid;
    *status = hartid == 0 ? LOONGEMU_SBI_HSM_STATE_STARTED
                          : LOONGEMU_SBI_HSM_STATE_STOPPED;
    return next_error;
}

static int64_t rec_hart_suspend(void *opaque, uint32_t suspend_type,
                                uint64_t resume_addr, uint64_t arg)
{
    (void)opaque;
    (void)resume_addr;
    (void)arg;
    call_count++;
    seen_suspend = suspend_type;
    return next_error;
}

static int64_t rec_system_reset(void *opaque, uint32_t reset_type,
                                uint32_t reset_reason)
{
    (void)opaque;
    call_count++;
    seen_reset_type = reset_type;
    seen_reset_reason = reset_reason;
    return next_error;
}

int main(void)
{
    const LoongemuSbiCalls calls = {
        .set_timer = rec_set_timer,
        .send_ipi = rec_send_ipi,
        .fence_i = rec_fence_i,
        .sfence_vma = rec_sfence_vma,
        .sfence_vma_asid = rec_sfence_vma_asid,
        .hart_start = rec_hart_start,
        .hart_stop = rec_hart_stop,
        .hart_get_status = rec_hart_get_status,
        .hart_suspend = rec_hart_suspend,
        .system_reset = rec_system_reset,
        .debug_write = rec_debug_write,
        .debug_read = rec_debug_read,
        .debug_write_byte = rec_debug_write_byte,
        .console_putchar = rec_console_putchar,
        .console_getchar = rec_console_getchar,
        .clear_ipi = rec_clear_ipi,
    };
    LoongemuSbi sbi = {
        .calls = &calls,
        .opaque = NULL,
        .hart_count = 4,
        .impl_id = LOONGEMU_SBI_IMPL_ID,
        .impl_version = 0x10000,
        .mvendorid = 0x123,
        .marchid = 0,
        .mimpid = 0x456,
    };
    uint64_t args[6];
    LoongemuSbiRet ret;

    memset(args, 0, sizeof(args));

    /* Base extension. */
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_SPEC_VERSION, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS, "base spec version succeeds");
    CHECK(ret.value == LOONGEMU_SBI_SPEC_VERSION, "base spec version is 2.0");

    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_IMPL_ID, args);
    CHECK(ret.value == LOONGEMU_SBI_IMPL_ID, "impl id reported");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_IMPL_VERSION, args);
    CHECK(ret.value == 0x10000, "impl version reported");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_MVENDORID, args);
    CHECK(ret.value == 0x123, "mvendorid reported");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_MARCHID, args);
    CHECK(ret.value == 0, "marchid reported");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_MIMPID, args);
    CHECK(ret.value == 0x456, "mimpid reported");

    args[0] = LOONGEMU_SBI_EXT_TIME;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_PROBE_EXTENSION, args);
    CHECK(ret.value == 1, "TIME probes as available");
    args[0] = 0x1234;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_PROBE_EXTENSION, args);
    CHECK(ret.value == 0, "unknown extension probes as unavailable");
    args[0] = 0;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_PROBE_EXTENSION, args);
    CHECK(ret.value == 0, "legacy set_timer probes as unavailable");

    /* PMU is discoverable, but this backend explicitly exposes no counters. */
    args[0] = LOONGEMU_SBI_EXT_PMU;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_PROBE_EXTENSION, args);
    CHECK(ret.value == 1, "PMU probes as available");
    CHECK(loongemu_sbi_has_extension(LOONGEMU_SBI_EXT_PMU),
          "PMU is listed as an implemented extension");
    memset(args, 0, sizeof(args));
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_PMU,
                            LOONGEMU_SBI_PMU_NUM_COUNTERS, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && ret.value == 0,
          "PMU reports zero counters explicitly");
    for (uint64_t fid = LOONGEMU_SBI_PMU_COUNTER_GET_INFO;
         fid <= LOONGEMU_SBI_PMU_COUNTER_FW_READ_HI; fid++) {
        ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_PMU, fid, args);
        CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
              "counter-indexed PMU FID with zero counters is INVALID_PARAM");
    }
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_PMU,
                            LOONGEMU_SBI_PMU_SNAPSHOT_SET_SHMEM, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "PMU shared-memory snapshot is not supported");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_PMU, 99, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "unknown PMU FID is not supported");

    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_BASE, 99, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "unknown base FID is not supported");
    ret = loongemu_sbi_call(&sbi, 0x7777, 0, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "unknown EID is not supported");

    /* TIME. */
    call_count = 0;
    args[0] = 0x1122334455667788ULL;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_TIME,
                            LOONGEMU_SBI_TIME_SET_TIMER, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS, "set_timer succeeds");
    CHECK(call_count == 1 && seen_when == args[0],
          "set_timer forwards the absolute deadline");
    next_error = LOONGEMU_SBI_ERR_FAILED;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_TIME,
                            LOONGEMU_SBI_TIME_SET_TIMER, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_FAILED,
          "set_timer propagates callback failures");
    next_error = 0;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_TIME, 1, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "unknown TIME FID is not supported");

    /* IPI: hart-list validation and broadcast. */
    call_count = 0;
    args[0] = 1;
    args[1] = 0x5; /* harts 1 and 3 exist */
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_IPI,
                            LOONGEMU_SBI_IPI_SEND_IPI, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS, "send_ipi accepts valid harts");
    CHECK(call_count == 1 && seen_base == 1 && seen_mask == 0x5,
          "send_ipi forwards the hart list");
    args[0] = 2;
    args[1] = 0x8; /* hart 5 does not exist */
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_IPI,
                            LOONGEMU_SBI_IPI_SEND_IPI, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "send_ipi rejects out-of-range harts");
    args[0] = UINT64_MAX;
    args[1] = 0;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_IPI,
                            LOONGEMU_SBI_IPI_SEND_IPI, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS, "hart_mask_base -1 broadcasts");
    CHECK(seen_base == UINT64_MAX, "broadcast keeps base -1");

    /* RFENCE. */
    call_count = 0;
    args[0] = 0;
    args[1] = 0xf;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_RFENCE,
                            LOONGEMU_SBI_RFENCE_FENCE_I, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && call_count == 1,
          "remote fence.i is dispatched");
    args[0] = 0;
    args[1] = 0xf;
    args[2] = 0x1000;
    args[3] = 0x2000;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_RFENCE,
                            LOONGEMU_SBI_RFENCE_SFENCE_VMA, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS &&
          seen_start == 0x1000 && seen_size == 0x2000,
          "remote sfence.vma forwards the range");
    args[4] = 7;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_RFENCE,
                            LOONGEMU_SBI_RFENCE_SFENCE_VMA_ASID, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && seen_asid == 7,
          "remote sfence.vma.asid forwards the ASID");
    args[4] = LOONGEMU_SBI_ASID_LIMIT;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_RFENCE,
                            LOONGEMU_SBI_RFENCE_SFENCE_VMA_ASID, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "16-bit ASIDs are enforced");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_RFENCE,
                            LOONGEMU_SBI_RFENCE_HFENCE_GVMA, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_NOT_SUPPORTED,
          "HFENCE is not supported without the H extension");

    /* HSM. */
    call_count = 0;
    args[0] = 2;
    args[1] = 0x80001000;
    args[2] = 0x5a;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_START, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && seen_hartid == 2 &&
          seen_start_addr == 0x80001000,
          "hart_start forwards hartid and entry");
    next_error = LOONGEMU_SBI_ERR_ALREADY_STARTED;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_START, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_ALREADY_STARTED,
          "hart_start propagates ALREADY_STARTED");
    next_error = 0;
    args[0] = sbi.hart_count;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_START, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "hart_start rejects invalid hartids");

    args[0] = 1;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_GET_STATUS, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS &&
          ret.value == LOONGEMU_SBI_HSM_STATE_STOPPED,
          "hart_get_status returns the hart state");
    args[0] = 0;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_GET_STATUS, args);
    CHECK(ret.value == LOONGEMU_SBI_HSM_STATE_STARTED,
          "hart_get_status distinguishes started harts");
    args[0] = 99;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_GET_STATUS, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "hart_get_status rejects invalid hartids");

    call_count = 0;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_STOP, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && call_count == 1,
          "hart_stop is dispatched");

    args[0] = 0x80000000; /* non-retentive default suspend */
    args[1] = 0x80002000;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_HSM,
                            LOONGEMU_SBI_HSM_HART_SUSPEND, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS &&
          seen_suspend == 0x80000000,
          "hart_suspend forwards the suspend type");

    /* SRST. */
    next_error = 0;
    args[0] = LOONGEMU_SBI_SRST_TYPE_COLD_REBOOT;
    args[1] = LOONGEMU_SBI_SRST_REASON_SYSFAIL;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_SRST,
                            LOONGEMU_SBI_SRST_RESET, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS &&
          seen_reset_type == LOONGEMU_SBI_SRST_TYPE_COLD_REBOOT &&
          seen_reset_reason == LOONGEMU_SBI_SRST_REASON_SYSFAIL,
          "system_reset forwards type and reason");
    args[0] = 3;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_SRST,
                            LOONGEMU_SBI_SRST_RESET, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "system_reset rejects unknown types");
    args[0] = 0;
    args[1] = 2;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_SRST,
                            LOONGEMU_SBI_SRST_RESET, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "system_reset rejects unknown reasons");
    next_error = LOONGEMU_SBI_ERR_FAILED;
    args[0] = 0;
    args[1] = 0;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_SRST,
                            LOONGEMU_SBI_SRST_RESET, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_FAILED,
          "system_reset reports a failed reset");

    /* DBCN and legacy SBI calls. */
    next_error = 0;
    args[0] = 5;
    args[1] = 0x10000;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_DBCN,
                            LOONGEMU_SBI_DBCN_WRITE, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && ret.value == 5 &&
          seen_debug_addr == 0x10000 && seen_debug_size == 5,
          "DBCN write forwards address and size");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_DBCN,
                            LOONGEMU_SBI_DBCN_READ, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && ret.value == 5,
          "DBCN read returns the byte count");
    args[0] = 0x41;
    args[1] = 1;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_DBCN,
                            LOONGEMU_SBI_DBCN_WRITE_BYTE, args);
    CHECK(ret.error == LOONGEMU_SBI_SUCCESS && seen_debug_byte == 0x41,
          "DBCN write_byte forwards the byte");
    args[1] = 2;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_EXT_DBCN,
                            LOONGEMU_SBI_DBCN_WRITE_BYTE, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "DBCN write_byte requires count 1");

    args[0] = 0x42;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_LEGACY_CONSOLE_PUTCHAR, 0,
                            args);
    CHECK(ret.error == 0 && seen_debug_byte == 0x42,
          "legacy console_putchar forwards the byte");
    console_char = 0x43;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_LEGACY_CONSOLE_GETCHAR, 0,
                            args);
    CHECK(ret.error == 0 && ret.value == 0x43,
          "legacy console_getchar returns the character");
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_LEGACY_CLEAR_IPI, 0, args);
    CHECK(ret.error == 0 && clear_ipi_count == 1,
          "legacy clear_ipi invokes the callback");
    args[0] = 0x3;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_LEGACY_SEND_IPI, 0, args);
    CHECK(ret.error == 0 && seen_mask == 0x3,
          "legacy send_ipi forwards the hart mask");
    args[0] = 0x5;
    ret = loongemu_sbi_call(&sbi, LOONGEMU_SBI_LEGACY_SET_TIMER, 0, args);
    CHECK(ret.error == 0 && seen_when == 0x5,
          "legacy set_timer forwards the deadline");

    /* Missing callbacks must degrade to NOT_SUPPORTED, not crash. */
    LoongemuSbi sparse = { .calls = &calls, .hart_count = 1 };
    ret = loongemu_sbi_call(&sparse, LOONGEMU_SBI_EXT_BASE,
                            LOONGEMU_SBI_BASE_GET_SPEC_VERSION, args);
    CHECK(ret.value == LOONGEMU_SBI_SPEC_VERSION,
          "base works without side-effect callbacks");
    ret = loongemu_sbi_call(NULL, 0, 0, args);
    CHECK(ret.error == LOONGEMU_SBI_ERR_INVALID_PARAM,
          "null SBI state is rejected");

    printf("SBI: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
