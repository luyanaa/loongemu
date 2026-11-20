/*
 * Loongson EXT/EXT2 helpers.
 *
 * The paired and indexed loads/stores are deliberately implemented through
 * the same system-memory helpers as ordinary loads/stores.  That keeps TLB
 * faults and MMIO access widths exact instead of letting a 64-bit pair load
 * degrade into host pointer dereferences.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "latm-helpers.h"
#include "latm-platform.h"

static uint64_t bswap_bytes(uint64_t v, unsigned bytes)
{
    switch (bytes) {
    case 1: return v;
    case 2: return __builtin_bswap16((uint16_t)v);
    case 4: return __builtin_bswap32((uint32_t)v);
    default: return __builtin_bswap64(v);
    }
}

static uint64_t lg_div32(uint64_t a, uint64_t b, int is_signed, int rem)
{
    if (is_signed) {
        int32_t x = (int32_t)a;
        int32_t y = (int32_t)b;
        if (y == 0) return 0;
        if (x == INT32_MIN && y == -1) return (uint64_t)(int64_t)x;
        return (uint64_t)(int64_t)(rem ? x % y : x / y);
    } else {
        uint32_t x = (uint32_t)a;
        uint32_t y = (uint32_t)b;
        if (y == 0) return 0;
        return (uint64_t)(int64_t)(rem ? x % y : x / y);
    }
}

static uint64_t lg_div64(uint64_t a, uint64_t b, int is_signed, int rem)
{
    if (is_signed) {
        int64_t x = (int64_t)a;
        int64_t y = (int64_t)b;
        if (y == 0) return 0;
        if (x == INT64_MIN && y == -1) return (uint64_t)x;
        return (uint64_t)(rem ? x % y : x / y);
    } else {
        if (b == 0) return 0;
        return rem ? a % b : a / b;
    }
}

uint64_t latm_helper_loongson_arith(CPUMIPSState *env, uint64_t op,
                                   uint64_t a, uint64_t b)
{
    (void)env;
    switch (op) {
    case MIPS_LG_MULT:  return (uint64_t)(int64_t)(int32_t)a *
                               (uint64_t)(int64_t)(int32_t)b;
    case MIPS_LG_MULTU: return (uint64_t)(int64_t)((uint32_t)a * (uint32_t)b);
    case MIPS_LG_DMULT: return a * b;
    case MIPS_LG_DMULTU:return a * b;
    case MIPS_LG_DIV:   return lg_div32(a, b, 1, 0);
    case MIPS_LG_DIVU:  return lg_div32(a, b, 0, 0);
    case MIPS_LG_DDIV:  return lg_div64(a, b, 1, 0);
    case MIPS_LG_DDIVU: return lg_div64(a, b, 0, 0);
    case MIPS_LG_MOD:   return lg_div32(a, b, 1, 1);
    case MIPS_LG_MODU:  return lg_div32(a, b, 0, 1);
    case MIPS_LG_DMOD:  return lg_div64(a, b, 1, 1);
    case MIPS_LG_DMODU: return lg_div64(a, b, 0, 1);
    case MIPS_LG_LE:    return (int64_t)a <= (int64_t)b;
    case MIPS_LG_GT:    return (int64_t)a > (int64_t)b;
    default:            return 0;
    }
}

static int lg_mem_size(uint16_t op)
{
    switch (op) {
    case MIPS_LM_GSLBX: case MIPS_LM_GSSBX:
    case MIPS_LM_GSLBLE: case MIPS_LM_GSLBGT:
    case MIPS_LM_GSSBLE: case MIPS_LM_GSSBGT: return 1;
    case MIPS_LM_GSLHX: case MIPS_LM_GSSHX:
    case MIPS_LM_GSLHLE: case MIPS_LM_GSLHGT:
    case MIPS_LM_GSSHLE: case MIPS_LM_GSSHGT: return 2;
    case MIPS_LM_GSLWX: case MIPS_LM_GSSWX:
    case MIPS_LM_GSLWLE: case MIPS_LM_GSLWGT:
    case MIPS_LM_GSSWLE: case MIPS_LM_GSSWGT: return 4;
    default: return 8;
    }
}

static int lg_is_store(uint16_t op)
{
    return op == MIPS_LM_GSSQ || op == MIPS_LM_GSSBX ||
           op == MIPS_LM_GSSHX || op == MIPS_LM_GSSWX ||
           op == MIPS_LM_GSSDX || op == MIPS_LM_GSSBLE ||
           op == MIPS_LM_GSSBGT || op == MIPS_LM_GSSHLE ||
           op == MIPS_LM_GSSHGT || op == MIPS_LM_GSSWLE ||
           op == MIPS_LM_GSSWGT || op == MIPS_LM_GSSDLE ||
           op == MIPS_LM_GSSDGT;
}

static uint64_t lg_load(CPUMIPSState *env, uint16_t op, uint64_t addr)
{
    unsigned size = lg_mem_size(op);
    uint32_t kind;

    switch (size) {
    case 1: kind = MIPS_MEM_LB; break;
    case 2: kind = MIPS_MEM_LH; break;
    case 4: kind = MIPS_MEM_LW; break;
    default: kind = MIPS_MEM_LD; break;
    }
    uint64_t v = latm_helper_load(env, kind, addr);
    if (op == MIPS_LM_GSLBLE || op == MIPS_LM_GSLHLE ||
        op == MIPS_LM_GSLWLE || op == MIPS_LM_GSLDLE ||
        op == MIPS_LM_GSLBGT || op == MIPS_LM_GSLHGT ||
        op == MIPS_LM_GSLWGT || op == MIPS_LM_GSLDGT) {
        int gt = op == MIPS_LM_GSLBGT || op == MIPS_LM_GSLHGT ||
                 op == MIPS_LM_GSLWGT || op == MIPS_LM_GSLDGT;
        if (gt) {
            v = bswap_bytes(v, size);
        }
    }
    return v;
}

static void lg_store(CPUMIPSState *env, uint16_t op, uint64_t addr, uint64_t v)
{
    unsigned size = lg_mem_size(op);
    uint32_t kind;

    if (op == MIPS_LM_GSSBGT || op == MIPS_LM_GSSHGT ||
        op == MIPS_LM_GSSWGT || op == MIPS_LM_GSSDGT) {
        v = bswap_bytes(v, size);
    }
    switch (size) {
    case 1: kind = MIPS_MEM_SB; break;
    case 2: kind = MIPS_MEM_SH; break;
    case 4: kind = MIPS_MEM_SW; break;
    default: kind = MIPS_MEM_SD; break;
    }
    latm_helper_store(env, kind, addr, v);
}

/* Pair forms write two guest GPRs; indexed/shifted forms write one. */
void latm_helper_loongson_mem(CPUMIPSState *env, uint32_t op,
                              uint64_t base, uint64_t index, int32_t offset,
                              uint32_t rt, uint32_t rt2)
{
    uint64_t addr = base + (uint64_t)(int64_t)offset + index;

    if (op == MIPS_LM_GSLQ) {
        uint64_t lo = latm_helper_load(env, MIPS_MEM_LD, addr);
        uint64_t hi = latm_helper_load(env, MIPS_MEM_LD, addr + 8);
        if (rt) env->active_tc.gpr[rt] = lo;
        if (rt2) env->active_tc.gpr[rt2] = hi;
        return;
    }
    if (op == MIPS_LM_GSSQ) {
        latm_helper_store(env, MIPS_MEM_SD, addr,
                          rt ? env->active_tc.gpr[rt] : 0);
        latm_helper_store(env, MIPS_MEM_SD, addr + 8,
                          rt2 ? env->active_tc.gpr[rt2] : 0);
        return;
    }
    if (lg_is_store((uint16_t)op)) {
        lg_store(env, (uint16_t)op, addr,
                 rt ? env->active_tc.gpr[rt] : 0);
    } else {
        uint64_t v = lg_load(env, (uint16_t)op, addr);
        if (rt) env->active_tc.gpr[rt] = v;
    }
}

uint64_t latm_helper_iocsr_read(CPUMIPSState *env, uint64_t addr, unsigned size)
{
    const LatmPlatform *p = latm_platform_get();
    if (p && p->iocsr_read) {
        return p->iocsr_read(env, addr, size);
    }
    return 0;
}

void latm_helper_iocsr_write(CPUMIPSState *env, uint64_t addr, uint64_t val,
                             unsigned size)
{
    const LatmPlatform *p = latm_platform_get();
    if (p && p->iocsr_write) {
        p->iocsr_write(env, addr, val, size);
    }
}

uint64_t latm_helper_cpucfg(CPUMIPSState *env, uint32_t sel)
{
    switch (sel) {
    case 0: return (uint32_t)env->CP0_PRid;
    case 1:
        return (1u << CPUCFG1_FP) | (1u << CPUCFG1_MMI) |
               (1u << CPUCFG1_LSLDR0) | (1u << CPUCFG1_LLEXC);
    case 2:
        return (1u << CPUCFG2_LEXT1) | (1u << CPUCFG2_LEXT2) |
               (1u << CPUCFG2_LEXT3) | (1u << CPUCFG2_LSPW) |
               (1u << CPUCFG2_LCSRP);
    default: return 0;
    }
}

uint64_t latm_helper_crc(CPUMIPSState *env, uint32_t op, uint64_t data,
                         uint64_t crc)
{
    (void)env;
    static const uint32_t poly = 0xedb88320u;
    static const uint32_t polyc = 0x82f63b78u;
    unsigned width = 1;
    uint64_t v = data;

    switch (op) {
    case MIPS_CRC32B: case MIPS_CRC32CB: width = 1; break;
    case MIPS_CRC32H: case MIPS_CRC32CH: width = 2; break;
    case MIPS_CRC32W: case MIPS_CRC32CW: width = 4; break;
    default: width = 8; break;
    }
    uint32_t c = (uint32_t)crc;
    int reflected = op >= MIPS_CRC32CB;
    for (unsigned i = 0; i < width; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        c ^= b;
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = reflected ? polyc : poly;
            c = (c >> 1) ^ (mask & (uint32_t)-(int32_t)(c & 1u));
        }
    }
    return c;
}
