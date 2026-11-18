#ifndef QEMU_LOG_H
#define QEMU_LOG_H
#include "qemu/osdep.h"

#define LOG_NONE          0
#define LOG_GUEST_ERROR   (1 << 0)
#define LOG_STRICT        (1 << 8)
#define LOG_IN_ASAN       (1 << 17)
#define LOG_TRACE         (1 << 20)
#define LOG_DEBUG         (1 << 22)
#define LOG_UNIMP         (1 << 1)
#define LAT_LOG_DT        (1 << 21)
#define LAT_IR2_SCHED     (1 << 23)
#define LAT_IMM_REG       (1 << 27)
#define CPU_LOG_TB_OUT_ASM (1 << 1)
#define CPU_LOG_TB_IN_ASM  (1 << 0)
#define CPU_LOG_TB_NOCHAIN (1 << 4)

extern int qemu_loglevel;
void qemu_log(const char *fmt, ...);

static inline int qemu_loglevel_mask(int mask)
{
    return 0;
}
#define qemu_log_mask(mask, ...) do { (void)(mask); } while (0)
#define qemu_log_mask_and_addr(mask, addr, ...) do { (void)(mask); } while (0)
#endif
