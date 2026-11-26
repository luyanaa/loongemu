#ifndef LATM_HELPERS_H
#define LATM_HELPERS_H

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"
#include "ir1.h"

#ifndef TARGET_PAGE_BITS
#define TARGET_PAGE_BITS 12
#endif
#ifndef TARGET_PAGE_SIZE
#define TARGET_PAGE_SIZE (1u << TARGET_PAGE_BITS)
#endif
#ifndef TARGET_PAGE_MASK
#define TARGET_PAGE_MASK (~(target_ulong)(TARGET_PAGE_SIZE - 1))
#endif

static inline unsigned latm_fcc_bit(unsigned cc)
{
    return cc == 0 ? 23 : 24 + cc;
}

void do_raise_exception(CPUMIPSState *env, uint32_t exception, uintptr_t pc);
void latm_check_interrupts(CPUMIPSState *env);
uint32_t cpu_mips_get_count(CPUMIPSState *env);
void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc);

/* FPU dispatch helpers (helpers/fpu.c). */
void latm_fpu_arith(CPUMIPSState *env, uint32_t fmt, uint32_t funct,
                    uint32_t fd, uint32_t fs, uint32_t ft);
void latm_fpu_cvt(CPUMIPSState *env, uint32_t fmt, uint32_t funct,
                  uint32_t fd, uint32_t fs);
void latm_fpu_cmp(CPUMIPSState *env, uint32_t fmt, uint32_t cond,
                  uint32_t fs, uint32_t ft, uint32_t cc);
void latm_fpu_movc(CPUMIPSState *env, uint32_t fmt, uint32_t fd, uint32_t fs,
                   uint32_t cc, uint32_t tf);
void latm_fpu_move(CPUMIPSState *env, uint32_t fmt, uint32_t op,
                   uint32_t fd, uint32_t fs, uint32_t rt);
void latm_fpu_madd(CPUMIPSState *env, uint32_t fmt, uint32_t op,
                   uint32_t fd, uint32_t fs, uint32_t ft, uint32_t fr);
void latm_fpu_gpr(CPUMIPSState *env, uint32_t op, uint32_t rt, uint32_t fs);
void latm_fpu_ldst(CPUMIPSState *env, uint32_t op, uint32_t ft,
                   uint32_t fs, uint64_t addr);

/* Integer helpers (helpers/int.c). */
uint64_t latm_helper_arith(CPUMIPSState *env, uint32_t op,
                           uint64_t a, uint64_t b);
void latm_helper_div(CPUMIPSState *env, uint32_t op, uint64_t a, uint64_t b);
void latm_helper_madd(CPUMIPSState *env, uint32_t op, uint64_t a, uint64_t b);
uint64_t latm_helper_fcc(CPUMIPSState *env, uint32_t cc);
void latm_helper_sync(CPUMIPSState *env, uint32_t stype);
void latm_helper_synci(CPUMIPSState *env, uint64_t addr);
void latm_helper_cache(CPUMIPSState *env, uint32_t op, uint64_t addr);
void latm_helper_pref(CPUMIPSState *env, uint32_t hint, uint64_t addr);
uint64_t latm_helper_rdhwr(CPUMIPSState *env, uint32_t rd, uint32_t sel);
uint64_t latm_helper_ll(CPUMIPSState *env, uint32_t op, uint64_t addr);
uint64_t latm_helper_sc(CPUMIPSState *env, uint32_t op, uint64_t addr,
                        uint64_t val);
/* Helper-call counters for the boot harness (see helpers/memory.c). */
void latm_regcache_release_temp(int reg, int temp);
/* Exception tap (see helpers/exception.c): enable from the harness, read back the
 * first eight raises with PC, EPC, BadVAddr and the cause code. */
extern unsigned latm_exc_tap_enable;
void latm_exc_tap_get(unsigned *n, uint64_t *pc, uint64_t *epc, uint64_t *bad,
                      uint64_t *code, unsigned cap);
void latm_helper_stats_get(uint64_t *loads, uint64_t *stores,
                           uint64_t *unaligned);
uint64_t latm_helper_load(CPUMIPSState *env, uint32_t op, uint64_t addr);
void latm_helper_store(CPUMIPSState *env, uint32_t op, uint64_t addr,
                       uint64_t val);
uint64_t latm_helper_unaligned_load(CPUMIPSState *env, uint32_t op,
                                    uint64_t addr, uint64_t old);
void latm_helper_unaligned_store(CPUMIPSState *env, uint32_t op,
                                 uint64_t addr, uint64_t value);
uint64_t latm_helper_cp0_read(CPUMIPSState *env, uint32_t reg, uint32_t sel);
void latm_helper_cp0_write(CPUMIPSState *env, uint32_t reg, uint32_t sel,
                           uint64_t val);
void latm_tlb_init(CPUMIPSState *env, CPUMIPSTLBContext *tlb, int nb_tlb);
void latm_helper_tlb(CPUMIPSState *env, uint32_t op);
void latm_jmp_cache_flush(void *cpu);
uint64_t latm_helper_shadow_read(CPUMIPSState *env, uint32_t rd);
uint64_t latm_helper_loongson_arith(CPUMIPSState *env, uint64_t op,
                                   uint64_t a, uint64_t b);
void latm_helper_loongson_mem(CPUMIPSState *env, uint32_t op,
                              uint64_t base, uint64_t index, int32_t offset,
                              uint32_t rt, uint32_t rt2);
uint64_t latm_helper_iocsr_read(CPUMIPSState *env, uint64_t addr, unsigned size);
void latm_helper_iocsr_write(CPUMIPSState *env, uint64_t addr, uint64_t val,
                             unsigned size);
uint64_t latm_helper_cpucfg(CPUMIPSState *env, uint32_t sel);
uint64_t latm_helper_crc(CPUMIPSState *env, uint32_t op, uint64_t data,
                         uint64_t crc);
void latm_helper_shadow_write(CPUMIPSState *env, uint32_t rd, uint64_t val);
void latm_helper_eret(CPUMIPSState *env);
void latm_helper_wait(CPUMIPSState *env);
uint64_t latm_helper_di(CPUMIPSState *env);
uint64_t latm_helper_ei(CPUMIPSState *env);
void latm_helper_trap(CPUMIPSState *env, uint32_t op, uint64_t a, uint64_t b,
                      uint32_t code);
void latm_helper_syscall(CPUMIPSState *env);
void latm_helper_break(CPUMIPSState *env, uint32_t code);
void latm_helper_overflow(CPUMIPSState *env);

#endif
