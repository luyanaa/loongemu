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
        if (off >= 0x10 && off < 0x30) {
            unsigned idx = (off - 0x10) / 8;
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
        m->status |= m->pending_set & m->enable;
        break;
    case 0x0c:
        write_reg((uint64_t *)&m->pending_clear, 0, size, value);
        m->status &= ~m->pending_clear;
        break;
    default:
        if (off >= 0x10 && off < 0x30) {
            unsigned idx = (off - 0x10) / 8;
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
    u->mailbox[cpu].buf[index] = value;
}

void latm_uncore_mailbox_irq(LatmUncore *u, unsigned cpu)
{
    if (!u || cpu >= LATM_MAX_CPUS) return;
    u->mailbox[cpu].status |= 1u;
}

int latm_uncore_pic_read(LatmUncore *u, uint32_t off, unsigned size,
                         uint64_t *value)
{
    if (!u || !value) return 0;
    switch (off & ~3u) {
    case 0x00: *value = read_reg(&u->pic_pending, 0, size); return 1;
    case 0x04: *value = read_reg(&u->pic_mask, 0, size); return 1;
    case 0x08: *value = read_reg(&u->pic_ack, 0, size); return 1;
    default: return 0;
    }
}

int latm_uncore_pic_write(LatmUncore *u, uint32_t off, unsigned size,
                          uint64_t value)
{
    if (!u) return 0;
    switch (off & ~3u) {
    case 0x04:
        write_reg(&u->pic_mask, 0, size, value);
        return 1;
    case 0x08:
        write_reg(&u->pic_ack, 0, size, value);
        return 1;
    default:
        return 0;
    }
}

void latm_uncore_pic_raise(LatmUncore *u, unsigned line, int level)
{
    if (!u || line >= LATM_PCH_PIC_LINES) return;
    if (level) u->pic_pending |= UINT64_C(1) << line;
    else u->pic_pending &= ~(UINT64_C(1) << line);
}

void latm_uncore_pic_eoi(LatmUncore *u, unsigned line)
{
    if (!u || line >= LATM_PCH_PIC_LINES) return;
    u->pic_pending &= ~(UINT64_C(1) << line);
    u->pic_ack &= ~(UINT64_C(1) << line);
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
}

void latm_uncore_extioi_raise(LatmUncore *u, unsigned vector)
{
    if (!u || vector >= LATM_EXTIOI_VECTORS || !u->route[vector].enabled) return;
    unsigned cpu = u->route[vector].cpu;
    unsigned ip = u->route[vector].ip;
    u->cpu_ip[cpu] |= 1u << (8u + ip);
}

void latm_uncore_extioi_eoi(LatmUncore *u, unsigned vector)
{
    if (!u || vector >= LATM_EXTIOI_VECTORS || !u->route[vector].enabled) return;
    unsigned cpu = u->route[vector].cpu;
    unsigned ip = u->route[vector].ip;
    u->cpu_ip[cpu] &= ~(1u << (8u + ip));
}

uint32_t latm_uncore_cpu_ip(LatmUncore *u, unsigned cpu)
{
    return (u && cpu < LATM_MAX_CPUS) ? u->cpu_ip[cpu] : 0;
}
