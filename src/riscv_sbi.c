/*
 * VMM-side SBI v2.0 dispatcher (architecture-independent decoding and
 * validation). See include/loongemu/riscv_sbi.h for the contract.
 */
#include <stddef.h>
#include "loongemu/riscv_sbi.h"

#define SBI_RET(error, value) ((LoongemuSbiRet){ (error), (value) })

int loongemu_sbi_has_extension(uint64_t eid)
{
    switch (eid) {
    case LOONGEMU_SBI_EXT_BASE:
    case LOONGEMU_SBI_EXT_TIME:
    case LOONGEMU_SBI_EXT_IPI:
    case LOONGEMU_SBI_EXT_RFENCE:
    case LOONGEMU_SBI_EXT_HSM:
    case LOONGEMU_SBI_EXT_SRST:
    case LOONGEMU_SBI_EXT_DBCN:
    case LOONGEMU_SBI_EXT_PMU:
        return 1;
    default:
        return 0;
    }
}

/*
 * hart_mask_base == (uint64_t)-1 means "all harts"; the callback implements
 * the broadcast. Otherwise every set bit names hartid base + bit, which must
 * exist and be supervisor-visible.
 */
static int64_t validate_hart_list(const LoongemuSbi *sbi,
                                  uint64_t hart_mask_base, uint32_t hart_mask)
{
    if (hart_mask_base == UINT64_MAX) {
        return LOONGEMU_SBI_SUCCESS;
    }
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((hart_mask & (UINT32_C(1) << bit)) == 0) {
            continue;
        }
        if (hart_mask_base + bit >= sbi->hart_count) {
            return LOONGEMU_SBI_ERR_INVALID_PARAM;
        }
    }
    return LOONGEMU_SBI_SUCCESS;
}

static LoongemuSbiRet sbi_base(const LoongemuSbi *sbi, uint64_t fid,
                               const uint64_t args[6])
{
    switch (fid) {
    case LOONGEMU_SBI_BASE_GET_SPEC_VERSION:
        return SBI_RET(LOONGEMU_SBI_SUCCESS, LOONGEMU_SBI_SPEC_VERSION);
    case LOONGEMU_SBI_BASE_GET_IMPL_ID:
        return SBI_RET(LOONGEMU_SBI_SUCCESS, sbi->impl_id);
    case LOONGEMU_SBI_BASE_GET_IMPL_VERSION:
        return SBI_RET(LOONGEMU_SBI_SUCCESS, sbi->impl_version);
    case LOONGEMU_SBI_BASE_PROBE_EXTENSION:
        return SBI_RET(LOONGEMU_SBI_SUCCESS,
                       loongemu_sbi_has_extension(args[0]) ? 1 : 0);
    case LOONGEMU_SBI_BASE_GET_MVENDORID:
        return SBI_RET(LOONGEMU_SBI_SUCCESS, sbi->mvendorid);
    case LOONGEMU_SBI_BASE_GET_MARCHID:
        return SBI_RET(LOONGEMU_SBI_SUCCESS, sbi->marchid);
    case LOONGEMU_SBI_BASE_GET_MIMPID:
        return SBI_RET(LOONGEMU_SBI_SUCCESS, sbi->mimpid);
    default:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
}

static LoongemuSbiRet sbi_time(const LoongemuSbi *sbi, uint64_t fid,
                               const uint64_t args[6])
{
    if (fid != LOONGEMU_SBI_TIME_SET_TIMER) {
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    if (sbi->calls->set_timer == NULL) {
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    /* Units are the platform timebase ticks (LATR: 1 GHz == nanoseconds). */
    return SBI_RET(sbi->calls->set_timer(sbi->opaque, args[0]), 0);
}

static LoongemuSbiRet sbi_ipi(const LoongemuSbi *sbi, uint64_t fid,
                              const uint64_t args[6])
{
    if (fid != LOONGEMU_SBI_IPI_SEND_IPI) {
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    if (sbi->calls->send_ipi == NULL) {
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    uint64_t base = args[0];
    uint32_t mask = (uint32_t)args[1];
    int64_t error = validate_hart_list(sbi, base, mask);
    if (error != LOONGEMU_SBI_SUCCESS) {
        return SBI_RET(error, 0);
    }
    return SBI_RET(sbi->calls->send_ipi(sbi->opaque, base, mask), 0);
}

static LoongemuSbiRet sbi_rfence(const LoongemuSbi *sbi, uint64_t fid,
                                 const uint64_t args[6])
{
    uint64_t base = args[0];
    uint32_t mask = (uint32_t)args[1];
    int64_t error;

    switch (fid) {
    case LOONGEMU_SBI_RFENCE_FENCE_I:
        if (sbi->calls->fence_i == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        error = validate_hart_list(sbi, base, mask);
        if (error != LOONGEMU_SBI_SUCCESS) {
            return SBI_RET(error, 0);
        }
        return SBI_RET(sbi->calls->fence_i(sbi->opaque, base, mask), 0);

    case LOONGEMU_SBI_RFENCE_SFENCE_VMA:
        if (sbi->calls->sfence_vma == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        error = validate_hart_list(sbi, base, mask);
        if (error != LOONGEMU_SBI_SUCCESS) {
            return SBI_RET(error, 0);
        }
        return SBI_RET(sbi->calls->sfence_vma(sbi->opaque, base, mask,
                                              args[2], args[3]), 0);

    case LOONGEMU_SBI_RFENCE_SFENCE_VMA_ASID:
        if (sbi->calls->sfence_vma_asid == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        if (args[4] >= LOONGEMU_SBI_ASID_LIMIT) {
            return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
        }
        error = validate_hart_list(sbi, base, mask);
        if (error != LOONGEMU_SBI_SUCCESS) {
            return SBI_RET(error, 0);
        }
        return SBI_RET(sbi->calls->sfence_vma_asid(sbi->opaque, base, mask,
                                                   args[2], args[3], args[4]),
                       0);

    default:
        /* HFENCE requires the H extension, which LATR does not implement. */
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
}

static LoongemuSbiRet sbi_hsm(const LoongemuSbi *sbi, uint64_t fid,
                              const uint64_t args[6])
{
    switch (fid) {
    case LOONGEMU_SBI_HSM_HART_START:
        if (sbi->calls->hart_start == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        if (args[0] >= sbi->hart_count) {
            return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
        }
        return SBI_RET(sbi->calls->hart_start(sbi->opaque, args[0], args[1],
                                              args[2]), 0);

    case LOONGEMU_SBI_HSM_HART_STOP:
        if (sbi->calls->hart_stop == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        return SBI_RET(sbi->calls->hart_stop(sbi->opaque), 0);

    case LOONGEMU_SBI_HSM_HART_GET_STATUS: {
        uint32_t status = 0;
        if (sbi->calls->hart_get_status == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        if (args[0] >= sbi->hart_count) {
            return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
        }
        int64_t error = sbi->calls->hart_get_status(sbi->opaque, args[0],
                                                    &status);
        if (error != LOONGEMU_SBI_SUCCESS) {
            return SBI_RET(error, 0);
        }
        return SBI_RET(LOONGEMU_SBI_SUCCESS, status);
    }

    case LOONGEMU_SBI_HSM_HART_SUSPEND:
        if (sbi->calls->hart_suspend == NULL) {
            return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
        }
        return SBI_RET(sbi->calls->hart_suspend(sbi->opaque,
                                                (uint32_t)args[0], args[1],
                                                args[2]),
                       0);

    default:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
}

static LoongemuSbiRet sbi_legacy(const LoongemuSbi *sbi, uint64_t eid,
                                 const uint64_t args[6])
{
    uint32_t mask = (uint32_t)args[0];
    int64_t err;

    switch (eid) {
    case LOONGEMU_SBI_LEGACY_SET_TIMER:
        if (sbi->calls->set_timer == NULL) break;
        return SBI_RET(sbi->calls->set_timer(sbi->opaque, args[0]), 0);
    case LOONGEMU_SBI_LEGACY_CONSOLE_PUTCHAR:
        if (sbi->calls->console_putchar == NULL) break;
        return SBI_RET(sbi->calls->console_putchar(sbi->opaque,
                                                   (uint8_t)args[0]), 0);
    case LOONGEMU_SBI_LEGACY_CONSOLE_GETCHAR: {
        int32_t ch = -1;
        if (sbi->calls->console_getchar == NULL) break;
        err = sbi->calls->console_getchar(sbi->opaque, &ch);
        return SBI_RET(err, (uint64_t)(int64_t)ch);
    }
    case LOONGEMU_SBI_LEGACY_CLEAR_IPI:
        if (sbi->calls->clear_ipi == NULL) break;
        return SBI_RET(sbi->calls->clear_ipi(sbi->opaque), 0);
    case LOONGEMU_SBI_LEGACY_SEND_IPI:
        if (sbi->calls->send_ipi == NULL) break;
        return SBI_RET(sbi->calls->send_ipi(sbi->opaque, 0, mask), 0);
    case LOONGEMU_SBI_LEGACY_REMOTE_FENCE_I:
        if (sbi->calls->fence_i == NULL) break;
        return SBI_RET(sbi->calls->fence_i(sbi->opaque, 0, mask), 0);
    case LOONGEMU_SBI_LEGACY_REMOTE_SFENCE_VMA:
        if (sbi->calls->sfence_vma == NULL) break;
        return SBI_RET(sbi->calls->sfence_vma(sbi->opaque, 0, mask,
                                              args[1], args[2]), 0);
    case LOONGEMU_SBI_LEGACY_REMOTE_SFENCE_VMA_ASID:
        if (sbi->calls->sfence_vma_asid == NULL) break;
        return SBI_RET(sbi->calls->sfence_vma_asid(sbi->opaque, 0, mask,
                                                   args[1], args[2],
                                                   args[3] & 0xffffu), 0);
    case LOONGEMU_SBI_LEGACY_SHUTDOWN:
        if (sbi->calls->system_reset == NULL) break;
        return SBI_RET(sbi->calls->system_reset(
                           sbi->opaque, LOONGEMU_SBI_SRST_TYPE_SHUTDOWN,
                           LOONGEMU_SBI_SRST_REASON_NONE), 0);
    default:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
}

static LoongemuSbiRet sbi_dbcn(const LoongemuSbi *sbi, uint64_t fid,
                               const uint64_t args[6])
{
    uint64_t done = 0;
    int64_t err;

    switch (fid) {
    case LOONGEMU_SBI_DBCN_WRITE:
        if (sbi->calls->debug_write == NULL) break;
        err = sbi->calls->debug_write(sbi->opaque, args[1],
                                      (uint32_t)args[0], &done);
        return SBI_RET(err, done);
    case LOONGEMU_SBI_DBCN_READ:
        if (sbi->calls->debug_read == NULL) break;
        err = sbi->calls->debug_read(sbi->opaque, args[1],
                                     (uint32_t)args[0], &done);
        return SBI_RET(err, done);
    case LOONGEMU_SBI_DBCN_WRITE_BYTE:
        if (sbi->calls->debug_write_byte == NULL) break;
        if (args[1] != 1) {
            return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
        }
        return SBI_RET(sbi->calls->debug_write_byte(sbi->opaque,
                                                    (uint8_t)args[0]), 0);
    default:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
}

static LoongemuSbiRet sbi_srst(const LoongemuSbi *sbi, uint64_t fid,
                               const uint64_t args[6])
{
    if (fid != LOONGEMU_SBI_SRST_RESET) {
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    if (sbi->calls->system_reset == NULL) {
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
    if (args[0] > LOONGEMU_SBI_SRST_TYPE_WARM_REBOOT ||
        args[1] > LOONGEMU_SBI_SRST_REASON_SYSFAIL) {
        return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
    }
    /* A successful reset does not return; a return value means failure. */
    return SBI_RET(sbi->calls->system_reset(sbi->opaque, (uint32_t)args[0],
                                            (uint32_t)args[1]),
                   0);
}


static LoongemuSbiRet sbi_pmu(const LoongemuSbi *sbi, uint64_t fid,
                              const uint64_t args[6])
{
    (void)sbi;
    switch (fid) {
    case LOONGEMU_SBI_PMU_NUM_COUNTERS:
        /* Explicit zero-counter contract: probes can discover that HPM
         * events are not backed yet without guessing from errors. */
        return SBI_RET(LOONGEMU_SBI_SUCCESS, 0);
    case LOONGEMU_SBI_PMU_COUNTER_GET_INFO:
    case LOONGEMU_SBI_PMU_COUNTER_CONFIG_MATCHING:
    case LOONGEMU_SBI_PMU_COUNTER_START:
    case LOONGEMU_SBI_PMU_COUNTER_STOP:
    case LOONGEMU_SBI_PMU_COUNTER_FW_READ:
    case LOONGEMU_SBI_PMU_COUNTER_FW_READ_HI:
        (void)args;
        return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
    case LOONGEMU_SBI_PMU_SNAPSHOT_SET_SHMEM:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    default:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
}

LoongemuSbiRet loongemu_sbi_call(LoongemuSbi *sbi, uint64_t eid, uint64_t fid,
                                 const uint64_t args[6])
{
    if (sbi == NULL || sbi->calls == NULL || args == NULL) {
        return SBI_RET(LOONGEMU_SBI_ERR_INVALID_PARAM, 0);
    }
    if (eid <= LOONGEMU_SBI_LEGACY_SHUTDOWN) {
        return sbi_legacy(sbi, eid, args);
    }
    switch (eid) {
    case LOONGEMU_SBI_EXT_DBCN:
        return sbi_dbcn(sbi, fid, args);
    case LOONGEMU_SBI_EXT_PMU:
        return sbi_pmu(sbi, fid, args);
    case LOONGEMU_SBI_EXT_BASE:
        return sbi_base(sbi, fid, args);
    case LOONGEMU_SBI_EXT_TIME:
        return sbi_time(sbi, fid, args);
    case LOONGEMU_SBI_EXT_IPI:
        return sbi_ipi(sbi, fid, args);
    case LOONGEMU_SBI_EXT_RFENCE:
        return sbi_rfence(sbi, fid, args);
    case LOONGEMU_SBI_EXT_HSM:
        return sbi_hsm(sbi, fid, args);
    case LOONGEMU_SBI_EXT_SRST:
        return sbi_srst(sbi, fid, args);
    default:
        return SBI_RET(LOONGEMU_SBI_ERR_NOT_SUPPORTED, 0);
    }
}
