#include "loongemu/vclock.h"

/*
 * Compute floor(a * num / den) without 64-bit overflow by splitting `a`
 * into quotient and remainder modulo `den`.
 */
static uint64_t mul_div_u64(uint64_t a, uint64_t num, uint64_t den)
{
    uint64_t q, r;

    if (den == 0) {
        return 0;
    }
    q = a / den;
    r = a % den;
    return q * num + (r * num) / den;
}

void loongemu_vclock_init(LoongemuVClock *vc,
                          LoongemuVClockMode mode,
                          uint64_t host_hz,
                          uint64_t tsc_hz,
                          uint64_t lapic_bus_hz,
                          uint64_t arm_cntfrq_hz,
                          uint64_t (*read_host_ticks)(void *opaque),
                          void *opaque)
{
    if (vc == 0) {
        return;
    }
    vc->mode = mode;
    vc->host_hz = host_hz ? host_hz : UINT64_C(100000000);
    vc->tsc_hz = tsc_hz ? tsc_hz : vc->host_hz;
    vc->lapic_bus_hz = lapic_bus_hz ? lapic_bus_hz : vc->host_hz;
    vc->arm_cntfrq_hz = arm_cntfrq_hz ? arm_cntfrq_hz : UINT64_C(62500000);
    vc->read_host_ticks = read_host_ticks;
    vc->opaque = opaque;
    vc->last_ns = 0;
    vc->virtual_ns = 0;
    vc->boot_host_ticks = (read_host_ticks != 0) ? read_host_ticks(opaque) : 0;
}

void loongemu_vclock_advance_ns(LoongemuVClock *vc, uint64_t delta_ns)
{
    if (vc == 0) {
        return;
    }
    vc->virtual_ns += delta_ns;
    if (vc->virtual_ns > vc->last_ns) {
        vc->last_ns = vc->virtual_ns;
    }
}

uint64_t loongemu_host_timer_hz(void)
{
#if defined(__loongarch__) || defined(__loongarch64)
    uint32_t ccfreq = 0, ccinfo = 0;
    uint64_t ccmul, ccdiv, hz;

    __asm__ volatile("cpucfg %0, %1" : "=r"(ccfreq) : "r"(4));
    __asm__ volatile("cpucfg %0, %1" : "=r"(ccinfo) : "r"(5));
    ccmul = (uint64_t)(ccinfo & 0xffffu);
    ccdiv = (uint64_t)((ccinfo >> 16) & 0xffffu);
    if (ccfreq == 0 || ccmul == 0 || ccdiv == 0) {
        return UINT64_C(100000000);
    }
    hz = ((uint64_t)ccfreq * ccmul) / ccdiv;
    return hz ? hz : UINT64_C(100000000);
#else
    return UINT64_C(100000000);
#endif
}

uint64_t loongemu_vclock_now_ns(LoongemuVClock *vc)
{
    uint64_t ns, prev;

    if (vc == 0) {
        return 0;
    }
    if (vc->mode == LOONGEMU_VCLOCK_VIRTUAL_STEP || vc->read_host_ticks == 0) {
        ns = __atomic_load_n(&vc->virtual_ns, __ATOMIC_ACQUIRE);
    } else {
        uint64_t now_ticks = vc->read_host_ticks(vc->opaque);
        uint64_t delta_ticks = (now_ticks >= vc->boot_host_ticks)
                                   ? (now_ticks - vc->boot_host_ticks)
                                   : 0;
        ns = mul_div_u64(delta_ticks, LOONGEMU_VCLOCK_NS_PER_SEC, vc->host_hz);
    }
    /* Cross-core strictly monotonic clamp via lock-free CAS. */
    prev = __atomic_load_n(&vc->last_ns, __ATOMIC_ACQUIRE);
    for (;;) {
        if (ns <= prev) {
            return prev;
        }
        if (__atomic_compare_exchange_n(&vc->last_ns, &prev, ns, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return ns;
        }
    }
}

uint64_t loongemu_vclock_read_tsc(LoongemuVClock *vc)
{
    uint64_t ns = loongemu_vclock_now_ns(vc);
    return mul_div_u64(ns, vc->tsc_hz, LOONGEMU_VCLOCK_NS_PER_SEC);
}

uint32_t loongemu_vclock_read_acpi_pm24(LoongemuVClock *vc)
{
    uint64_t ns = loongemu_vclock_now_ns(vc);
    return (uint32_t)(mul_div_u64(ns, LOONGEMU_VCLOCK_ACPI_PM_HZ,
                                  LOONGEMU_VCLOCK_NS_PER_SEC) &
                      UINT64_C(0x00ffffff));
}

uint16_t loongemu_vclock_read_pit16(LoongemuVClock *vc, uint16_t reload)
{
    uint64_t ns = loongemu_vclock_now_ns(vc);
    uint64_t mod = reload ? (uint64_t)reload : UINT64_C(65536);
    uint64_t ticks = mul_div_u64(ns, LOONGEMU_VCLOCK_PIT_HZ,
                                 LOONGEMU_VCLOCK_NS_PER_SEC);
    return (uint16_t)((mod - (ticks % mod)) & UINT64_C(0xffff));
}

uint64_t loongemu_vclock_read_hpet(LoongemuVClock *vc)
{
    uint64_t ns = loongemu_vclock_now_ns(vc);
    return mul_div_u64(ns, LOONGEMU_VCLOCK_HPET_HZ, LOONGEMU_VCLOCK_NS_PER_SEC);
}

uint64_t loongemu_vclock_read_arm_cntvct(LoongemuVClock *vc)
{
    uint64_t ns = loongemu_vclock_now_ns(vc);
    return mul_div_u64(ns, vc->arm_cntfrq_hz, LOONGEMU_VCLOCK_NS_PER_SEC);
}

uint32_t loongemu_vclock_lapic_current(LoongemuVClock *vc,
                                       uint64_t start_ns,
                                       uint32_t init_count,
                                       uint32_t divider,
                                       int periodic,
                                       int *expired_out)
{
    uint64_t now_ns, elapsed_ns, ticks;

    if (expired_out != 0) {
        *expired_out = 0;
    }
    if (vc == 0 || init_count == 0) {
        return 0;
    }
    if (divider == 0) {
        divider = 1;
    }
    now_ns = loongemu_vclock_now_ns(vc);
    elapsed_ns = (now_ns >= start_ns) ? (now_ns - start_ns) : 0;
    ticks = mul_div_u64(elapsed_ns, vc->lapic_bus_hz,
                        LOONGEMU_VCLOCK_NS_PER_SEC) /
            (uint64_t)divider;
    if (ticks >= (uint64_t)init_count) {
        if (expired_out != 0) {
            *expired_out = 1;
        }
        if (periodic) {
            return init_count - (uint32_t)(ticks % (uint64_t)init_count);
        }
        return 0;
    }
    return init_count - (uint32_t)ticks;
}
