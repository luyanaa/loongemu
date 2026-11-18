/* Minimal stand-in for QEMU's target/arm/tcg/translate.h (8.x). */
#ifndef LATA_ARM_TCG_TRANSLATE_H
#define LATA_ARM_TCG_TRANSLATE_H
#include "qemu/osdep.h"
#include <stdio.h>
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "exec/exec-all.h"
#include "target/arm/cpu.h"
#include "ir1-arg.h"
#ifndef G_NORETURN
#define G_NORETURN __attribute__((noreturn))
#endif
#ifndef CF_PARALLEL
#define CF_PARALLEL (1u << 0)
#endif
#define CP_REG_ARM64_SYSREG_CP 0
#define ENCODE_AA64_CP_REG(cp, crn, crm, op0, op1, op2) \
    ((int)(cp) ^ ((crn) << 14) ^ ((crm) << 7) ^ ((op0) << 20) ^ ((op1) << 11) ^ ((op2) << 17))
static inline bool cp_access_ok(int current_el, const ARMCPRegInfo *ri, bool isread)
{ return true; }

typedef unsigned int MemOp;
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_MASK (~(TARGET_PAGE_SIZE - 1))
#define TARGET_INSN_START_WORDS 2
#define CF_SINGLE_STEP (1u << 1)
#define FPROUNDING_ZERO 3
#define FPROUNDING_TIEAWAY 4
#define QEMU_ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
static inline uint32_t simd_desc(unsigned size, unsigned width, unsigned opcode)
{ return (opcode << 8) | (width << 4) | size; }
#define TRANS_FEAT_LDST_TAG(NAME, FEAT, DO, ZERO, PAIR) \
static bool trans_##NAME(DisasContext *s) { \
    return DO(s, &s->arg.f_ldst_tag, ZERO, PAIR); \
}
typedef enum {
    DISAS_NEXT, DISAS_TOO_MANY, DISAS_UPDATE_EXIT, DISAS_EXIT,
    DISAS_NORETURN, DISAS_WFI, DISAS_WFE, DISAS_YIELD, DISAS_SWI
} DisasJumpType;

typedef struct CPUARMTBFlags { uint32_t flags[2]; } CPUARMTBFlags;

typedef struct DisasContextBase {
    TranslationBlock *tb;
    uint64_t pc_first;
    uint64_t pc_next;
    int is_jmp;
    int num_insns;
    int max_insns;
    bool singlestep_enabled;
    void *host_addr[2];
} DisasContextBase;

typedef struct DisasContext {
    DisasContextBase *base;
    uint64_t pc_curr;
    uint64_t pc_save;
    uint32_t insn;
    int insn_type;
    union a64_u arg;
    uint64_t tbid;
    void *cp_regs;
    void *isar;
    uint64_t *features;
    uint32_t dcz_blocksize;
    int user, aarch64, thumb, sctlr_b, be_data;
    int condjmp, condexec_mask, condexec_cond;
    int current_el, mmu_idx;
    int fp_excp_el, sve_excp_el, sme_excp_el;
    int vl, svl;
    bool fp_access_checked, sve_access_checked, is_nonstreaming;
    bool align_mem, pstate_za;
    bool ss_active, pstate_sm, pstate_ss, is_ldex, sme_trap_nonstreaming;
    bool mte_active[2], tbii, tcma, pauth_active, unpriv, ata, pstate_il;
    bool fgt_active, fgt_svc, fgt_eret;
    int bt, btype, vec_len, vec_stride, naa, lse2;
} DisasContext;

#define LOG2_TAG_GRANULE 4
#define EXCP_UDEF 1
extern int option_fam_jmp_cache;
extern CPUState *current_cpu;
static inline CPUARMState *arm_env(CPUState *cpu) { return (CPUARMState *)cpu->env_ptr; }
static inline uint32_t syn_uncategorized(void) { return 0; }
static inline uint32_t syn_aa64_sysregtrap(int op0, int op1, int op2, int crn,
                                           int crm, int rt, bool isread) { return 0; }
static inline uint32_t syn_aa64_svc(int imm16) { return 0; }
#define ARM_CP_SUPPRESS_TB_END (1U << 12)
#define EXCP_SWI 2
enum { MO_8, MO_16, MO_32, MO_64, MO_SIZE = 3, MO_SIGN = 1 << 3,
       MO_BSWAP = 1 << 4, MO_LE = 0, MO_BE = MO_BSWAP };
static inline uint64_t dup_const(unsigned size, uint64_t imm)
{
    unsigned bits = 8u << size;
    unsigned shift = bits;
    uint64_t v = imm & ((bits == 64) ? ~0ull : ((1ull << bits) - 1));
    while (shift < 64) { v |= v << shift; shift <<= 1; }
    return v;
}
#define shl_12(ctx, x) ((int64_t)(x) << 12)
#define times_2(ctx, x) ((int64_t)(x) * 2)
#define times_4(ctx, x) ((int64_t)(x) * 4)
#define plus_1(ctx, x) ((int64_t)(x) + 1)
#define plus_2(ctx, x) ((int64_t)(x) + 2)
#define EX_TBFLAG_ANY(tb, f) 0
#define EX_TBFLAG_A64(tb, f) 0
#define dc_isar_feature(name, s) 0

static inline int core_to_aa64_mmu_idx(int idx) { return idx; }
int arm_mmu_idx_to_el(int idx);
CPUARMTBFlags arm_tbflags_from_tb(TranslationBlock *tb);
int arm_rmode_to_sf(int rmode);
int tb_cflags(const TranslationBlock *tb);
int qemu_log_in_addr_range(uint64_t addr);
FILE *qemu_log_trylock(void);
void qemu_log_unlock(FILE *f);
const char *lookup_symbol(uint64_t addr);
void target_disas(FILE *f, CPUState *cpu, uint64_t pc, size_t size);

#endif
