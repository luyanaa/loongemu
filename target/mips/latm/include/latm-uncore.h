#ifndef LATM_UNCORE_H
#define LATM_UNCORE_H

#include <stdint.h>

#define LATM_MAX_CPUS 4u
#define LATM_MAILBOX_CORE_STRIDE 0x100u
#define LATM_MAILBOX_BASE 0x3ff01000u
#define LATM_PCH_PIC_BASE 0x10000000u
#define LATM_PCH_PIC_LINES 64u
#define LATM_PCH_MSI_BASE 0x2ff00000u
#define LATM_EXTIOI_VECTORS 256u
#define LATM_EXTIOI_IP_FIRST 3u
#define LATM_EXTIOI_IP_LAST 6u

typedef struct LatmMailbox {
    uint64_t status;
    uint64_t enable;
    uint64_t pending_set;
    uint64_t pending_clear;
    uint64_t buf[4];
} LatmMailbox;

typedef struct LatmExtioiRoute {
    uint8_t cpu;
    uint8_t ip;
    uint8_t enabled;
    uint8_t level;
} LatmExtioiRoute;

typedef struct LatmUncore {
    LatmMailbox mailbox[LATM_MAX_CPUS];
    uint64_t pic_pending;
    uint64_t pic_mask;
    uint64_t pic_ack;
    uint64_t pic_htmsi_en;
    uint64_t pic_edge;
    uint64_t pic_isr;
    LatmExtioiRoute route[LATM_EXTIOI_VECTORS];
    uint8_t msi_route[LATM_EXTIOI_VECTORS];
    uint8_t extioi_pending[LATM_EXTIOI_VECTORS];
    uint32_t extioi_nodetype[8];
    uint32_t extioi_enable[8];
    uint32_t extioi_bounce[8];
    uint32_t extioi_isr[8];
    uint32_t extioi_coreisr[LATM_MAX_CPUS][8];
    uint32_t extioi_coremap[64];
    uint8_t extioi_ipmap[8];
    uint8_t extioi_sw_coremap[LATM_EXTIOI_VECTORS];
    uint32_t cpu_ip[LATM_MAX_CPUS]; /* CP0 Cause IP bit mask */
} LatmUncore;

void latm_uncore_init(LatmUncore *u);
void latm_uncore_reset(LatmUncore *u);
int latm_uncore_mailbox_read(LatmUncore *u, uint32_t gpa, unsigned size,
                             uint64_t *value);
int latm_uncore_mailbox_write(LatmUncore *u, uint32_t gpa, unsigned size,
                              uint64_t value);
void latm_uncore_mailbox_send(LatmUncore *u, unsigned cpu, unsigned index,
                              uint64_t value);
void latm_uncore_mailbox_irq(LatmUncore *u, unsigned cpu);
int latm_uncore_pic_read(LatmUncore *u, uint32_t off, unsigned size,
                         uint64_t *value);
int latm_uncore_pic_write(LatmUncore *u, uint32_t off, unsigned size,
                          uint64_t value);
void latm_uncore_pic_raise(LatmUncore *u, unsigned line, int level);
void latm_uncore_pic_eoi(LatmUncore *u, unsigned line);
int latm_uncore_msi_write(LatmUncore *u, uint32_t gpa, unsigned size,
                          uint64_t value);
void latm_uncore_extioi_route(LatmUncore *u, unsigned vector, unsigned cpu,
                              unsigned ip, int level);
void latm_uncore_extioi_raise(LatmUncore *u, unsigned vector);
void latm_uncore_extioi_eoi(LatmUncore *u, unsigned vector);
uint32_t latm_uncore_cpu_ip(LatmUncore *u, unsigned cpu);
uint64_t latm_uncore_iocsr_read(LatmUncore *u, uint64_t addr, unsigned size);
void latm_uncore_iocsr_write(LatmUncore *u, uint64_t addr, uint64_t value,
                             unsigned size);
uint64_t latm_uncore_iocsr_read_cpu(LatmUncore *u, uint64_t addr,
                                    unsigned size, unsigned cpu);
void latm_uncore_iocsr_write_cpu(LatmUncore *u, uint64_t addr, uint64_t value,
                                 unsigned size, unsigned cpu);

#endif
