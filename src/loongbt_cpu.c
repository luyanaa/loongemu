#include "loongemu/loongbt.h"

static uint32_t g_cpu_flags;
static bool g_hw_available;

void loongbt_set_cpu_features(uint32_t flags)
{
    g_cpu_flags = flags;
}

uint32_t loongbt_cpu_features(void)
{
    return g_cpu_flags;
}

void loongbt_set_hw_available(bool available)
{
    g_hw_available = available;
}

bool loongbt_hw_available(void)
{
    return g_hw_available;
}

uint64_t loongbt_exec_soft(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                           uint64_t a, uint64_t b);

uint64_t loongbt_exec(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                      uint64_t a, uint64_t b)
{
    /* cpu_flag selects the back end: cores without LBT (2K1000LA/2K2000) take
     * the integer fallback, cores with LBT_X86 run the real instructions. */
    if (g_hw_available && (g_cpu_flags & LOONGEMU_CPU_LBT_X86) && loongbt_hw_probe()) {
        return loongbt_hw_exec(st, op, width, a, b);
    }
    return loongbt_exec_soft(st, op, width, a, b);
}

uint64_t loongbt_pack_flags(uint32_t eflags, uint8_t mask)
{
    uint64_t dst = 0;

    if (mask & (1u << 0)) dst |= (uint64_t)((eflags >> 0) & 1) << 0;
    if (mask & (1u << 1)) dst |= (uint64_t)((eflags >> 2) & 1) << 2;
    if (mask & (1u << 2)) dst |= (uint64_t)((eflags >> 4) & 1) << 4;
    if (mask & (1u << 3)) dst |= (uint64_t)((eflags >> 6) & 1) << 6;
    if (mask & (1u << 4)) dst |= (uint64_t)((eflags >> 7) & 1) << 7;
    if (mask & (1u << 5)) dst |= (uint64_t)((eflags >> 11) & 1) << 11;
    return dst;
}

void loongbt_unpack_flags(uint32_t *eflags, uint64_t value, uint8_t mask)
{
    uint32_t f = *eflags;

    if (mask & (1u << 0)) f = (f & ~LOONGBT_EF_CF) | (uint32_t)(((value >> 0) & 1) << 0);
    if (mask & (1u << 1)) f = (f & ~LOONGBT_EF_PF) | (uint32_t)(((value >> 2) & 1) << 2);
    if (mask & (1u << 2)) f = (f & ~LOONGBT_EF_AF) | (uint32_t)(((value >> 4) & 1) << 4);
    if (mask & (1u << 3)) f = (f & ~LOONGBT_EF_ZF) | (uint32_t)(((value >> 6) & 1) << 6);
    if (mask & (1u << 4)) f = (f & ~LOONGBT_EF_SF) | (uint32_t)(((value >> 7) & 1) << 7);
    if (mask & (1u << 5)) f = (f & ~LOONGBT_EF_OF) | (uint32_t)(((value >> 11) & 1) << 11);
    *eflags = f;
}
