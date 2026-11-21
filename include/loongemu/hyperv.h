#ifndef LOONGEMU_HYPERV_H
#define LOONGEMU_HYPERV_H

#include <stdint.h>

/* Hyper-V x64 hypercalls. Flush callbacks must complete before returning. */
typedef struct LoongemuHypervOps {
    void *opaque;
    uint32_t vp_count;
    int (*read_guest)(void *opaque, uint64_t gpa, uint64_t *value);
    uint64_t (*read_xmm_qword)(void *opaque, unsigned index);
    void (*flush)(void *opaque, uint64_t vp_mask, uint64_t address_space,
                  uint64_t flags);
    void (*send_ipi)(void *opaque, uint64_t vp_mask, uint8_t vector);
} LoongemuHypervOps;

/* RCX is control; RDX and R8 are input GPA or fast-call input words. */
uint64_t loongemu_hyperv_call(const LoongemuHypervOps *ops, uint64_t control,
                              uint64_t rdx, uint64_t r8);

#endif
