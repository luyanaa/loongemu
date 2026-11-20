#ifndef LOONGEMU_VCLOCK_H
#define LOONGEMU_VCLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified virtual clock model for LoongEMU.
 *
 * All guest time sources (x86 TSC, LAPIC timer, PIT 8254, ACPI PM Timer,
 * HPET, and AArch64 Generic Timer CNTVCT) derive from a single monotonic
 * virtual nanosecond timeline so their frequencies and event ordering stay
 * mutually consistent across vCPU switches and future physical-SMP runs.
 */

#define LOONGEMU_VCLOCK_NS_PER_SEC     UINT64_C(1000000000)
#define LOONGEMU_VCLOCK_ACPI_PM_HZ     UINT64_C(3579545)
#define LOONGEMU_VCLOCK_PIT_HZ         UINT64_C(1193182)
#define LOONGEMU_VCLOCK_HPET_HZ        UINT64_C(10000000)  /* 10 MHz (100 ns) */
#define LOONGEMU_VCLOCK_DEFAULT_EPOCH  UINT64_C(1767225600) /* 2026-01-01T00:00:00Z */

typedef enum LoongemuVClockMode {
    /* Anchored to monotonic host counter (rdtime.d) with non-decreasing guard. */
    LOONGEMU_VCLOCK_HOST_MONO = 0,
    /* Deterministic virtual timeline advanced explicitly (icount/replay). */
    LOONGEMU_VCLOCK_VIRTUAL_STEP = 1,
} LoongemuVClockMode;

typedef struct LoongemuVClock {
    LoongemuVClockMode mode;
    uint64_t host_hz;
    uint64_t tsc_hz;
    uint64_t lapic_bus_hz;
    uint64_t arm_cntfrq_hz;
    uint64_t boot_host_ticks;
    uint64_t last_ns;
    uint64_t virtual_ns;
    uint64_t wall_epoch_sec;
    uint64_t (*read_host_ticks)(void *opaque);
    void *opaque;
} LoongemuVClock;

void loongemu_vclock_init(LoongemuVClock *vc,
                          LoongemuVClockMode mode,
                          uint64_t host_hz,
                          uint64_t tsc_hz,
                          uint64_t lapic_bus_hz,
                          uint64_t arm_cntfrq_hz,
                          uint64_t (*read_host_ticks)(void *opaque),
                          void *opaque);

/* Advance deterministic virtual time (used in LOONGEMU_VCLOCK_VIRTUAL_STEP). */
void loongemu_vclock_advance_ns(LoongemuVClock *vc, uint64_t delta_ns);

/* Monotonic nanoseconds since VM boot. */
uint64_t loongemu_vclock_now_ns(LoongemuVClock *vc);

/* Unified virtual wall-clock (shared by CMOS/RTC and KVM wallclock MSR). */
void loongemu_vclock_set_wall_epoch(LoongemuVClock *vc, uint64_t unix_sec);
uint64_t loongemu_vclock_wall_sec(LoongemuVClock *vc);
void loongemu_vclock_wall_timespec(LoongemuVClock *vc,
                                   uint64_t *out_sec, uint32_t *out_nsec);

/* Derived guest time sources sharing the same monotonic timeline. */
uint64_t loongemu_vclock_read_tsc(LoongemuVClock *vc);
uint32_t loongemu_vclock_read_acpi_pm24(LoongemuVClock *vc);
uint16_t loongemu_vclock_read_pit16(LoongemuVClock *vc, uint16_t reload);
uint64_t loongemu_vclock_read_hpet(LoongemuVClock *vc);
uint64_t loongemu_vclock_read_arm_cntvct(LoongemuVClock *vc);

/*
 * LAPIC timer countdown from a virtual-nanosecond start timestamp.
 * Returns the current remaining count (0 if one-shot expired).
 */
uint32_t loongemu_vclock_lapic_current(LoongemuVClock *vc,
                                       uint64_t start_ns,
                                       uint32_t init_count,
                                       uint32_t divider,
                                       int periodic,
                                       int *expired_out);

/*
 * Probe the LoongArch constant timer (rdtime.d) frequency in Hz via
 * cpucfg 4 (CCFreq) and cpucfg 5 (CCMul/CCDiv). Falls back to 100 MHz
 * on non-LoongArch hosts or if cpucfg reports 0.
 */
uint64_t loongemu_host_timer_hz(void);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_VCLOCK_H */
