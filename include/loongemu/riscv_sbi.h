#ifndef LOONGEMU_RISCV_SBI_H
#define LOONGEMU_RISCV_SBI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VMM-side SBI v2.0 dispatcher.
 *
 * The translator routes an S-mode ECALL to the VMM as a trap with
 * mcause=9; the VMM calls loongemu_sbi_call() with a7 (EID), a6 (FID) and
 * a0..a5, then writes the returned pair into a0/a1 and advances mepc past
 * the ECALL. This module only implements the architecture-independent
 * decoding/validation; side effects belong to the callbacks so the same code
 * is usable by an emulator and unit-tested on the host.
 */

/* SBI extension IDs (SBI v0.2 and later). */
#define LOONGEMU_SBI_EXT_BASE   0x10
#define LOONGEMU_SBI_EXT_TIME   0x54494D45U /* "TIME" */
#define LOONGEMU_SBI_EXT_IPI    0x735049U   /* "sPI"  */
#define LOONGEMU_SBI_EXT_RFENCE 0x52464E43U /* "RFNC" */
#define LOONGEMU_SBI_EXT_HSM    0x48534D    /* "HSM"  */
#define LOONGEMU_SBI_EXT_SRST   0x53525354U /* "SRST" */
#define LOONGEMU_SBI_EXT_DBCN   0x4442434EU /* "DBCN" */
#define LOONGEMU_SBI_EXT_PMU    0x504D55U   /* "PMU"  */

/* Legacy (SBI v0.1) EIDs. */
#define LOONGEMU_SBI_LEGACY_SET_TIMER        0x00
#define LOONGEMU_SBI_LEGACY_CONSOLE_PUTCHAR  0x01
#define LOONGEMU_SBI_LEGACY_CONSOLE_GETCHAR  0x02
#define LOONGEMU_SBI_LEGACY_CLEAR_IPI        0x03
#define LOONGEMU_SBI_LEGACY_SEND_IPI         0x04
#define LOONGEMU_SBI_LEGACY_REMOTE_FENCE_I   0x05
#define LOONGEMU_SBI_LEGACY_REMOTE_SFENCE_VMA 0x06
#define LOONGEMU_SBI_LEGACY_REMOTE_SFENCE_VMA_ASID 0x07
#define LOONGEMU_SBI_LEGACY_SHUTDOWN         0x08

/* DBCN function IDs. */
#define LOONGEMU_SBI_DBCN_WRITE       0
#define LOONGEMU_SBI_DBCN_READ        1
#define LOONGEMU_SBI_DBCN_WRITE_BYTE  2

/* PMU function IDs.  The VMM reports zero counters: NUM_COUNTERS is a
 * well-defined query, while every counter-indexed operation is rejected.
 * The HPM CSR state exists, but no event backend drives the counters yet. */
#define LOONGEMU_SBI_PMU_NUM_COUNTERS       0
#define LOONGEMU_SBI_PMU_COUNTER_GET_INFO   1
#define LOONGEMU_SBI_PMU_COUNTER_CONFIG_MATCHING 2
#define LOONGEMU_SBI_PMU_COUNTER_START      3
#define LOONGEMU_SBI_PMU_COUNTER_STOP       4
#define LOONGEMU_SBI_PMU_COUNTER_FW_READ    5
#define LOONGEMU_SBI_PMU_COUNTER_FW_READ_HI 6
#define LOONGEMU_SBI_PMU_SNAPSHOT_SET_SHMEM 7

/* Base extension function IDs. */
#define LOONGEMU_SBI_BASE_GET_SPEC_VERSION 0
#define LOONGEMU_SBI_BASE_GET_IMPL_ID      1
#define LOONGEMU_SBI_BASE_GET_IMPL_VERSION 2
#define LOONGEMU_SBI_BASE_PROBE_EXTENSION  3
#define LOONGEMU_SBI_BASE_GET_MVENDORID    4
#define LOONGEMU_SBI_BASE_GET_MARCHID      5
#define LOONGEMU_SBI_BASE_GET_MIMPID       6

/* TIME / IPI function IDs. */
#define LOONGEMU_SBI_TIME_SET_TIMER 0
#define LOONGEMU_SBI_IPI_SEND_IPI   0

/* RFENCE function IDs. */
#define LOONGEMU_SBI_RFENCE_FENCE_I           0
#define LOONGEMU_SBI_RFENCE_SFENCE_VMA        1
#define LOONGEMU_SBI_RFENCE_SFENCE_VMA_ASID   2
#define LOONGEMU_SBI_RFENCE_HFENCE_GVMA_VMID  3
#define LOONGEMU_SBI_RFENCE_HFENCE_GVMA       4
#define LOONGEMU_SBI_RFENCE_HFENCE_VVMA_ASID  5
#define LOONGEMU_SBI_RFENCE_HFENCE_VVMA       6

/* HSM function IDs and states. */
#define LOONGEMU_SBI_HSM_HART_START      0
#define LOONGEMU_SBI_HSM_HART_STOP       1
#define LOONGEMU_SBI_HSM_HART_GET_STATUS 2
#define LOONGEMU_SBI_HSM_HART_SUSPEND    3

#define LOONGEMU_SBI_HSM_STATE_STARTED   0
#define LOONGEMU_SBI_HSM_STATE_STOPPED   1

/* SRST function ID, reset types and reasons. */
#define LOONGEMU_SBI_SRST_RESET             0
#define LOONGEMU_SBI_SRST_TYPE_SHUTDOWN     0
#define LOONGEMU_SBI_SRST_TYPE_COLD_REBOOT  1
#define LOONGEMU_SBI_SRST_TYPE_WARM_REBOOT  2
#define LOONGEMU_SBI_SRST_REASON_NONE       0
#define LOONGEMU_SBI_SRST_REASON_SYSFAIL    1

/* Standard SBI error codes (returned in a0). */
#define LOONGEMU_SBI_SUCCESS               0
#define LOONGEMU_SBI_ERR_FAILED           (-1)
#define LOONGEMU_SBI_ERR_NOT_SUPPORTED    (-2)
#define LOONGEMU_SBI_ERR_INVALID_PARAM    (-3)
#define LOONGEMU_SBI_ERR_DENIED           (-4)
#define LOONGEMU_SBI_ERR_INVALID_ADDRESS  (-5)
#define LOONGEMU_SBI_ERR_ALREADY_AVAILABLE (-6)
#define LOONGEMU_SBI_ERR_ALREADY_STARTED  (-7)
#define LOONGEMU_SBI_ERR_ALREADY_STOPPED  (-8)

/* SBI v2.0 is the implemented specification version. */
#define LOONGEMU_SBI_SPEC_VERSION 0x02000000U

/* Implementation ID used in the probe tables; documented as Loongemu. */
#define LOONGEMU_SBI_IMPL_ID 0x4000U

/* ASIDs wider than 16 bits are rejected by RFENCE per the SBI spec. */
#define LOONGEMU_SBI_ASID_LIMIT (1U << 16)

/*
 * Side-effect callbacks. Each returns an SBI error code (0 on success) and
 * receives the already validated arguments. A NULL callback means "the VMM
 * cannot service this call" and the dispatcher answers NOT_SUPPORTED.
 */
typedef struct LoongemuSbiCalls {
    int64_t (*set_timer)(void *opaque, uint64_t when);
    int64_t (*send_ipi)(void *opaque, uint64_t hart_mask_base,
                        uint32_t hart_mask);
    int64_t (*fence_i)(void *opaque, uint64_t hart_mask_base,
                       uint32_t hart_mask);
    int64_t (*sfence_vma)(void *opaque, uint64_t hart_mask_base,
                          uint32_t hart_mask, uint64_t start,
                          uint64_t size);
    int64_t (*sfence_vma_asid)(void *opaque, uint64_t hart_mask_base,
                               uint32_t hart_mask, uint64_t start,
                               uint64_t size, uint64_t asid);
    int64_t (*hart_start)(void *opaque, uint64_t hartid, uint64_t start_addr,
                          uint64_t arg);
    int64_t (*hart_stop)(void *opaque);
    int64_t (*hart_get_status)(void *opaque, uint64_t hartid,
                               uint32_t *status);
    int64_t (*hart_suspend)(void *opaque, uint32_t suspend_type,
                            uint64_t resume_addr, uint64_t arg);
    int64_t (*system_reset)(void *opaque, uint32_t reset_type,
                            uint32_t reset_reason);
    /* DBCN: guest_address/size are interpreted by the VMM. */
    int64_t (*debug_write)(void *opaque, uint64_t guest_addr, uint32_t size,
                           uint64_t *written);
    int64_t (*debug_read)(void *opaque, uint64_t guest_addr, uint32_t size,
                          uint64_t *read);
    int64_t (*debug_write_byte)(void *opaque, uint8_t byte);
    int64_t (*console_putchar)(void *opaque, uint8_t byte);
    int64_t (*console_getchar)(void *opaque, int32_t *ch);
    int64_t (*clear_ipi)(void *opaque);
} LoongemuSbiCalls;

typedef struct LoongemuSbi {
    const LoongemuSbiCalls *calls;
    void *opaque;
    /* Number of harts the supervisor may address; hartids are 0..count-1. */
    uint64_t hart_count;
    uint32_t impl_id;
    uint32_t impl_version;
    uint64_t mvendorid;
    uint64_t marchid;
    uint64_t mimpid;
} LoongemuSbi;

/* struct sbiret: a0 = error, a1 = value. */
typedef struct LoongemuSbiRet {
    int64_t error;
    uint64_t value;
} LoongemuSbiRet;

/*
 * Executes one SBI call. `args` is a0..a5. Unsupported EIDs/FIDs return
 * LOONGEMU_SBI_ERR_NOT_SUPPORTED; the caller must still write both registers.
 */
LoongemuSbiRet loongemu_sbi_call(LoongemuSbi *sbi, uint64_t eid, uint64_t fid,
                                 const uint64_t args[6]);

/* True for the extension IDs this dispatcher implements. */
int loongemu_sbi_has_extension(uint64_t eid);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_RISCV_SBI_H */
