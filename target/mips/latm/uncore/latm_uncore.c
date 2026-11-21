#include "qemu/osdep.h"
#include "latm-uncore.h"

static uint64_t read_reg(uint64_t *reg, uint32_t off, unsigned size)
{
    uint64_t v = *reg;
    unsigned shift = (off & 7u) * 8u;
    if (size >= 8) return v;
    return (v >> shift) & ((UINT64_C(1) << (size * 8u)) - 1u);
}

static void write_reg(uint64_t *reg, uint32_t off, unsigned size, uint64_t value)
{
    unsigned shift = (off & 7u) * 8u;
    uint64_t mask = size >= 8 ? UINT64_MAX :
                    ((UINT64_C(1) << (size * 8u)) - 1u) << shift;
    *reg = (*reg & ~mask) | ((value << shift) & mask);
}

void latm_uncore_init(LatmUncore *u)
{
    if (u) memset(u, 0, sizeof(*u));
}

void latm_uncore_reset(LatmUncore *u)
{
    latm_uncore_init(u);
}

int latm_uncore_mailbox_read(LatmUncore *u, uint32_t gpa, unsigned size,
                             uint64_t *value)
{
    if (!u || !value || size == 0 || size > 8 ||
        gpa < LATM_MAILBOX_BASE ||
        gpa >= LATM_MAILBOX_BASE + LATM_MAX_CPUS * LATM_MAILBOX_CORE_STRIDE) {
        return 0;
    }
    unsigned cpu = (gpa - LATM_MAILBOX_BASE) / LATM_MAILBOX_CORE_STRIDE;
    uint32_t off = (gpa - LATM_MAILBOX_BASE) % LATM_MAILBOX_CORE_STRIDE;
    LatmMailbox *m = &u->mailbox[cpu];
    switch (off) {
    case 0x00: *value = read_reg((uint64_t *)&m->status, 0, size); return 1;
    case 0x04: *value = read_reg((uint64_t *)&m->enable, 0, size); return 1;
    case 0x08: *value = read_reg((uint64_t *)&m->pending_set, 0, size); return 1;
    case 0x0c: *value = read_reg((uint64_t *)&m->pending_clear, 0, size); return 1;
    default:
        if (off >= 0x20 && off < 0x40) {
            unsigned idx = (off - 0x20) / 8;
            *value = read_reg(&m->buf[idx], off & 7, size);
            return 1;
        }
        return 0;
    }
}

int latm_uncore_mailbox_write(LatmUncore *u, uint32_t gpa, unsigned size,
                              uint64_t value)
{
    if (!u || size == 0 || size > 8 ||
        gpa < LATM_MAILBOX_BASE ||
        gpa >= LATM_MAILBOX_BASE + LATM_MAX_CPUS * LATM_MAILBOX_CORE_STRIDE) {
        return 0;
    }
    unsigned cpu = (gpa - LATM_MAILBOX_BASE) / LATM_MAILBOX_CORE_STRIDE;
    uint32_t off = (gpa - LATM_MAILBOX_BASE) % LATM_MAILBOX_CORE_STRIDE;
    LatmMailbox *m = &u->mailbox[cpu];
    switch (off) {
    case 0x00: write_reg((uint64_t *)&m->status, 0, size, value); break;
    case 0x04: write_reg((uint64_t *)&m->enable, 0, size, value); break;
    case 0x08:
        write_reg((uint64_t *)&m->pending_set, 0, size, value);
        __atomic_fetch_or(&m->status, m->pending_set & m->enable,
                          __ATOMIC_SEQ_CST);
        break;
    case 0x0c:
        write_reg((uint64_t *)&m->pending_clear, 0, size, value);
        __atomic_fetch_and(&m->status, ~m->pending_clear, __ATOMIC_SEQ_CST);
        break;
    default:
        if (off >= 0x20 && off < 0x40) {
            unsigned idx = (off - 0x20) / 8;
            write_reg(&m->buf[idx], off & 7, size, value);
            return 1;
        }
        return 0;
    }
    return 1;
}

void latm_uncore_mailbox_send(LatmUncore *u, unsigned cpu, unsigned index,
                              uint64_t value)
{
    if (!u || cpu >= LATM_MAX_CPUS || index >= 4) return;
    __atomic_store_n(&u->mailbox[cpu].buf[index], value, __ATOMIC_RELEASE);
}

void latm_uncore_mailbox_irq(LatmUncore *u, unsigned cpu)
{
    if (!u || cpu >= LATM_MAX_CPUS) return;
    __atomic_fetch_or(&u->mailbox[cpu].status, 1u, __ATOMIC_SEQ_CST);
}

int latm_uncore_pic_read(LatmUncore *u, uint32_t off, unsigned size,
                         uint64_t *value)
{
    if (!u || !value) return 0;
    switch (off & ~3u) {
    case 0x20: *value = read_reg(&u->pic_mask, 0, size); return 1;
    case 0x40: *value = read_reg(&u->pic_htmsi_en, 0, size); return 1;
    case 0x60: *value = read_reg(&u->pic_edge, 0, size); return 1;
    case 0x80: *value = read_reg(&u->pic_ack, 0, size); return 1;
    case 0x200: *value = read_reg(&u->pic_htmsi_en, 0, size); return 1;
    case 0x380: *value = read_reg(&u->pic_isr, 0, size); return 1;
    case 0x3a0: *value = (uint32_t)u->pic_pending; return 1;
    case 0x3a4: *value = (uint32_t)(u->pic_pending >> 32); return 1;
    case 0x3e0: *value = read_reg(&u->pic_edge, 0, size); return 1;
    default: return 0;
    }
}

int latm_uncore_pic_write(LatmUncore *u, uint32_t off, unsigned size,
                          uint64_t value)
{
    if (!u) return 0;
    switch (off & ~3u) {
    case 0x20:
        write_reg(&u->pic_mask, 0, size, value);
        return 1;
    case 0x40:
        write_reg(&u->pic_htmsi_en, 0, size, value);
        return 1;
    case 0x60:
        write_reg(&u->pic_edge, 0, size, value);
        return 1;
    case 0x80:
        write_reg(&u->pic_ack, 0, size, value);
        __atomic_fetch_and(&u->pic_pending, ~value, __ATOMIC_SEQ_CST);
        __atomic_fetch_and(&u->pic_isr, ~value, __ATOMIC_SEQ_CST);
        return 1;
    case 0x200:
        write_reg(&u->pic_htmsi_en, 0, size, value);
        return 1;
    default:
        if ((off & ~3u) >= 0x100 && (off & ~3u) < 0x140) {
            /* ROUTE_ENTRY: retain the last written routing byte. */
            return 1;
        }
        return 0;
    }
}

void latm_uncore_pic_raise(LatmUncore *u, unsigned line, int level)
{
    if (!u || line >= LATM_PCH_PIC_LINES) return;
    if (level) {
        __atomic_fetch_or(&u->pic_pending, UINT64_C(1) << line,
                          __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&u->pic_isr, UINT64_C(1) << line, __ATOMIC_SEQ_CST);
        latm_uncore_extioi_raise(u, line);
    } else {
        __atomic_fetch_and(&u->pic_pending, ~(UINT64_C(1) << line),
                           __ATOMIC_SEQ_CST);
        __atomic_fetch_and(&u->pic_isr, ~(UINT64_C(1) << line),
                           __ATOMIC_SEQ_CST);
        latm_uncore_extioi_eoi(u, line);
    }
}

void latm_uncore_pic_eoi(LatmUncore *u, unsigned line)
{
    if (!u || line >= LATM_PCH_PIC_LINES) return;
    __atomic_fetch_and(&u->pic_pending, ~(UINT64_C(1) << line), __ATOMIC_SEQ_CST);
    __atomic_fetch_and(&u->pic_isr, ~(UINT64_C(1) << line), __ATOMIC_SEQ_CST);
    __atomic_fetch_and(&u->pic_ack, ~(UINT64_C(1) << line), __ATOMIC_SEQ_CST);
    latm_uncore_extioi_eoi(u, line);
}

int latm_uncore_msi_write(LatmUncore *u, uint32_t gpa, unsigned size,
                          uint64_t value)
{
    if (!u || gpa < LATM_PCH_MSI_BASE || gpa >= LATM_PCH_MSI_BASE + 0x100000u ||
        size == 0 || size > 8) {
        return 0;
    }
    unsigned vector = (unsigned)(value & 0xffu);
    if (vector < LATM_EXTIOI_VECTORS) {
        latm_uncore_extioi_raise(u, vector);
    }
    return 1;
}

void latm_uncore_extioi_route(LatmUncore *u, unsigned vector, unsigned cpu,
                              unsigned ip, int level)
{
    if (!u || vector >= LATM_EXTIOI_VECTORS || cpu >= LATM_MAX_CPUS ||
        ip < LATM_EXTIOI_IP_FIRST || ip > LATM_EXTIOI_IP_LAST) return;
    u->route[vector].cpu = (uint8_t)cpu;
    u->route[vector].ip = (uint8_t)ip;
    u->route[vector].enabled = 1;
    u->route[vector].level = level ? 1 : 0;
    u->extioi_sw_coremap[vector] = (uint8_t)cpu;
    u->extioi_ipmap[vector / 32] |= (uint8_t)(1u << (ip - LATM_EXTIOI_IP_FIRST));
    u->extioi_coremap[vector / 4] |= (uint32_t)cpu << (8 * (vector % 4));
    u->extioi_enable[vector / 32] |= 1u << (vector % 32);
}

static void latm_uncore_extioi_recompute(LatmUncore *u)
{
    uint32_t ip[LATM_MAX_CPUS] = { 0 };

    for (unsigned v = 0; v < LATM_EXTIOI_VECTORS; v++) {
        if (!__atomic_load_n(&u->extioi_pending[v], __ATOMIC_ACQUIRE) ||
            !u->route[v].enabled) {
            continue;
        }
        unsigned cpu = u->route[v].cpu;
        unsigned line = u->route[v].ip;
        if (cpu < LATM_MAX_CPUS && line >= LATM_EXTIOI_IP_FIRST &&
            line <= LATM_EXTIOI_IP_LAST) {
            ip[cpu] |= 1u << (8u + line);
        }
    }
    for (unsigned c = 0; c < LATM_MAX_CPUS; c++) {
        __atomic_store_n(&u->cpu_ip[c], ip[c], __ATOMIC_RELEASE);
    }
}

void latm_uncore_extioi_raise(LatmUncore *u, unsigned vector)
{
    if (!u || vector >= LATM_EXTIOI_VECTORS || !u->route[vector].enabled) return;
    __atomic_store_n(&u->extioi_pending[vector], 1, __ATOMIC_RELEASE);
    __atomic_fetch_or(&u->extioi_isr[vector / 32], 1u << (vector % 32),
                      __ATOMIC_SEQ_CST);
    unsigned cpu = u->route[vector].cpu;
    if (cpu < LATM_MAX_CPUS) {
        __atomic_fetch_or(&u->extioi_coreisr[cpu][vector / 32],
                          1u << (vector % 32), __ATOMIC_SEQ_CST);
    }
    latm_uncore_extioi_recompute(u);
}

void latm_uncore_extioi_eoi(LatmUncore *u, unsigned vector)
{
    if (!u || vector >= LATM_EXTIOI_VECTORS) return;
    __atomic_store_n(&u->extioi_pending[vector], 0, __ATOMIC_RELEASE);
    __atomic_fetch_and(&u->extioi_isr[vector / 32], ~(1u << (vector % 32)),
                       __ATOMIC_SEQ_CST);
    unsigned cpu = u->route[vector].cpu;
    if (cpu < LATM_MAX_CPUS) {
        __atomic_fetch_and(&u->extioi_coreisr[cpu][vector / 32],
                           ~(1u << (vector % 32)), __ATOMIC_SEQ_CST);
    }
    latm_uncore_extioi_recompute(u);
}

uint32_t latm_uncore_cpu_ip(LatmUncore *u, unsigned cpu)
{
    return (u && cpu < LATM_MAX_CPUS) ? u->cpu_ip[cpu] : 0;
}


static unsigned extioi_ip_from_byte(uint8_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        if (v & (1u << i)) {
            return LATM_EXTIOI_IP_FIRST + i;
        }
    }
    return LATM_EXTIOI_IP_FIRST;
}

static void extioi_apply_coremap(LatmUncore *u, unsigned irq, uint32_t val)
{
    for (unsigned i = 0; i < 4 && irq + i < LATM_EXTIOI_VECTORS; i++) {
        unsigned cpu = (val >> (8 * i)) & 0xffu;
        if (cpu >= LATM_MAX_CPUS) cpu = 0;
        u->extioi_sw_coremap[irq + i] = (uint8_t)cpu;
        u->route[irq + i].cpu = (uint8_t)cpu;
        u->route[irq + i].ip =
            (uint8_t)extioi_ip_from_byte(u->extioi_ipmap[(irq + i) / 32]);
        u->route[irq + i].enabled =
            (u->extioi_enable[(irq + i) / 32] >> ((irq + i) % 32)) & 1u;
    }
}

static uint64_t iocsr_ipi_read(LatmUncore *u, unsigned cpu, uint32_t off,
                               unsigned size)
{
    LatmMailbox *m = &u->mailbox[cpu];
    if (off == 0x00) return read_reg((uint64_t *)&m->status, 0, size);
    if (off == 0x04) return read_reg((uint64_t *)&m->enable, 0, size);
    if (off >= 0x20 && off < 0x40) {
        unsigned idx = (off - 0x20) / 8;
        return read_reg(&m->buf[idx], off & 7, size);
    }
    return 0;
}

static void iocsr_ipi_write(LatmUncore *u, unsigned cpu, uint32_t off,
                            uint64_t value, unsigned size)
{
    LatmMailbox *m = &u->mailbox[cpu];
    if (off == 0x04) {
        write_reg((uint64_t *)&m->enable, 0, size, value);
    } else if (off == 0x08) {
        __atomic_fetch_or(&m->status, (uint32_t)value, __ATOMIC_SEQ_CST);
    } else if (off == 0x0c) {
        __atomic_fetch_and(&m->status, ~(uint32_t)value, __ATOMIC_SEQ_CST);
    } else if (off >= 0x20 && off < 0x40) {
        unsigned idx = (off - 0x20) / 8;
        write_reg(&m->buf[idx], off & 7, size, value);
    } else if (off == 0x40) {
        unsigned target = (unsigned)((value >> 16) & 0x3ff);
        unsigned vector = (unsigned)(value & 0x1f);
        if (target < LATM_MAX_CPUS) {
            __atomic_fetch_or(&u->mailbox[target].status, 1u << vector,
                              __ATOMIC_SEQ_CST);
        }
    } else if (off == 0x48) {
        unsigned target = (unsigned)((value >> 16) & 0x3ff);
        uint32_t boff = 0x20 + (uint32_t)(value & 0x1c);
        if (target < LATM_MAX_CPUS) {
            LatmMailbox *t = &u->mailbox[target];
            write_reg(&t->buf[(boff - 0x20) / 8], boff & 7, 4,
                      value >> 32);
        }
    }
}

uint64_t latm_uncore_iocsr_read_cpu(LatmUncore *u, uint64_t addr,
                                    unsigned size, unsigned cpu)
{
    if (!u || size == 0 || size > 4) return 0;
    if (cpu >= LATM_MAX_CPUS) cpu = 0;
    if (addr >= 0x1000 && addr < 0x1100) {
        return iocsr_ipi_read(u, cpu, (uint32_t)(addr - 0x1000), size);
    }
    if (addr < 0x1400 || addr >= 0x1d00) {
        return 0;
    }
    unsigned off = (unsigned)(addr - 0x1400);
    uint32_t v = 0;
    if (off >= 0xa0 && off < 0xc0) {
        v = u->extioi_nodetype[(off - 0xa0) / 4];
    } else if (off >= 0xc0 && off < 0xc8) {
        unsigned n = (off - 0xc0) / 4;
        v = u->extioi_ipmap[n * 4] | ((uint32_t)u->extioi_ipmap[n * 4 + 1] << 8) |
            ((uint32_t)u->extioi_ipmap[n * 4 + 2] << 16) |
            ((uint32_t)u->extioi_ipmap[n * 4 + 3] << 24);
    } else if (off >= 0x200 && off < 0x220) {
        v = u->extioi_enable[(off - 0x200) / 4];
    } else if (off >= 0x280 && off < 0x2a0) {
        v = u->extioi_bounce[(off - 0x280) / 4];
    } else if (off >= 0x300 && off < 0x320) {
        v = u->extioi_isr[(off - 0x300) / 4];
    } else if (off >= 0x400 &&
               off < 0x400 + (sizeof(u->extioi_coreisr[0]) /
                              sizeof(u->extioi_coreisr[0][0])) * 4) {
        v = u->extioi_coreisr[cpu][(off - 0x400) / 4];
    } else if (off >= 0x800 && off < 0x900) {
        v = u->extioi_coremap[(off - 0x800) / 4];
    }
    return v;
}

uint64_t latm_uncore_iocsr_read(LatmUncore *u, uint64_t addr, unsigned size)
{
    return latm_uncore_iocsr_read_cpu(u, addr, size, 0);
}

void latm_uncore_iocsr_write_cpu(LatmUncore *u, uint64_t addr, uint64_t value,
                                 unsigned size, unsigned cpu)
{
    if (!u || size == 0 || size > 4) return;
    if (cpu >= LATM_MAX_CPUS) cpu = 0;
    if (addr >= 0x1000 && addr < 0x1100) {
        iocsr_ipi_write(u, cpu, (uint32_t)(addr - 0x1000), value, size);
        return;
    }
    if (addr < 0x1400 || addr >= 0x1d00) {
        return;
    }
    unsigned off = (unsigned)(addr - 0x1400);
    uint32_t val = (uint32_t)value;
    if (off >= 0xa0 && off < 0xc0) {
        u->extioi_nodetype[(off - 0xa0) / 4] = val;
    } else if (off >= 0xc0 && off < 0xc8) {
        unsigned n = (off - 0xc0) / 4;
        for (unsigned i = 0; i < 4; i++) {
            u->extioi_ipmap[n * 4 + i] = (uint8_t)(val >> (8 * i));
        }
        for (unsigned v = n * 32; v < n * 32 + 32; v++) {
            u->route[v].ip = (uint8_t)extioi_ip_from_byte(
                u->extioi_ipmap[v / 32]);
        }
    } else if (off >= 0x200 && off < 0x220) {
        unsigned n = (off - 0x200) / 4;
        uint32_t old = u->extioi_enable[n];
        u->extioi_enable[n] = val;
        for (unsigned i = 0; i < 32; i++) {
            unsigned v = n * 32 + i;
            u->route[v].enabled = (val >> i) & 1u;
            u->route[v].level = (val >> i) & 1u;
        }
        (void)old;
        latm_uncore_extioi_recompute(u);
    } else if (off >= 0x280 && off < 0x2a0) {
        u->extioi_bounce[(off - 0x280) / 4] = val;
    } else if (off >= 0x300 && off < 0x320) {
        unsigned n = (off - 0x300) / 4;
        u->extioi_isr[n] = val;
        for (unsigned i = 0; i < 32; i++) {
            __atomic_store_n(&u->extioi_pending[n * 32 + i],
                             (val >> i) & 1u, __ATOMIC_RELEASE);
        }
        latm_uncore_extioi_recompute(u);
    } else if (off >= 0x400 &&
               off < 0x400 + (sizeof(u->extioi_coreisr[0]) /
                              sizeof(u->extioi_coreisr[0][0])) * 4) {
        unsigned n = (off - 0x400) / 4;
        /* Write one to clear the pending core ISR bits. */
        u->extioi_coreisr[cpu][n] &= ~val;
        u->extioi_isr[n] &= ~val;
        for (unsigned i = 0; i < 32; i++) {
            if ((val >> i) & 1u) {
                __atomic_store_n(&u->extioi_pending[n * 32 + i], 0,
                                 __ATOMIC_RELEASE);
            }
        }
        latm_uncore_extioi_recompute(u);
    } else if (off >= 0x800 && off < 0x900) {
        unsigned irq = off - 0x800;
        u->extioi_coremap[irq / 4] = val;
        extioi_apply_coremap(u, irq, val);
        latm_uncore_extioi_recompute(u);
    }
}

void latm_uncore_iocsr_write(LatmUncore *u, uint64_t addr, uint64_t value,
                             unsigned size)
{
    latm_uncore_iocsr_write_cpu(u, addr, value, size, 0);
}
