#ifndef LATX_BARE_EXEC_ALL_H
#define LATX_BARE_EXEC_ALL_H
#include "qemu/osdep.h"
#include "cpu.h"

/* Translation-block vocabulary mirrored from the LATX-upgraded upstream
 * header.  Configuration-only variants are folded in unconditionally: the
 * bare build keeps one shape so the stripped modules agree on offsets. */

#define TCG_MAX_INSNS 512
#define TB_JMP_RESET_OFFSET_INVALID 0xffff
#define TB_JMP_CACHE_BITS 12

#define PAGE_READ      0x0001
#define PAGE_WRITE     0x0002
#define PAGE_EXEC      0x0004
#define PAGE_VALID     0x0008
#define PAGE_WRITE_ORG 0x0010
#define PAGE_WRITE_INV 0x0020
#define PAGE_RESET     0x0040
#define PAGE_ANON      0x0080

#define CF_COUNT_MASK  0x00007fff
#define CF_LAST_IO     0x00008000
#define CF_MEMI_ONLY   0x00010000
#define CF_USE_ICOUNT  0x00020000
#define CF_INVALID     0x00040000
#define CF_PARALLEL    0x00080000
#define CF_CLUSTER_MASK 0xff000000
#define CF_CLUSTER_SHIFT 24

/* bool_flags bits */
#define TARGET1_ELIMINATE 0x01
#define OPT_BCC           0x02
#define IS_ENABLE_JRRA    0x04
#define IS_AOT_TB         0x08
#define IS_TUNNEL_LIB     0x10
#define IS_TU_TB          0x20
#define IS_TU_JMP         0x40
#define IS_INDIRECT_JMP   0x80
#define SIGNAL_UNLINK0    0x100
#define SIGNAL_RELINK0    0x200
#define SIGNAL_UNLINK1    0x400
#define SIGNAL_RELINK1    0x800
#define IS_CODE64         0x1000
#define IS_TU_SPLIT       0x2000
#define TBSMC_OPTED       0x4000
#define AOT_STATS_COUNTED 0x8000

#define TBSMC_COUNT_MASK        0xffff
#define TBSMC_OPT_THRESHOLD     0xff
#define TBSMC_OPTED_MASK        (0x1 << 16)

/* Host exception numbers referenced by the translator's helper glue. */
#define EXCP00_DIV0    0
#define EXCP01_DB      1
#define EXCP03_INT3    3
#define EXCP04_INTO    4
#define EXCP05_BOUND   5
#define EXCP06_ILLOP   6
#define EXCP0D_GPF     13
#define EXCP00_DIVZ    0
#define EXCP10_COPR    16
#define EXCP_DEBUG     0x10001
#define EXCP_HLT       0x10002
#define EXCP_INTERRUPT 0x10000
#define EXCP_SYSCALL   0x100
#define EXCP_VSYSCALL  0x101
#define EXCP_MASK      0x1ff

typedef uint64_t tb_page_addr_t;


typedef struct IntervalTreeNode { void *unused; } IntervalTreeNode;
typedef struct IntervalTreeRoot { IntervalTreeNode *rb_root; } IntervalTreeRoot;
typedef struct RBRoot { void *rb_node; } RBRoot;
typedef struct TCGContext TCGContext;
typedef struct TBProfile { uint64_t count[2]; } TBProfile;

struct tb_tc {
    uint8_t *ptr;    /* pointer to the translated code */
    size_t size;
    size_t offset_in_tu;
};

struct tu_unlink {
    uint32_t stub_offset;
    uint32_t ins;
    uint8_t  rel_num;
};

typedef struct TranslationBlock TranslationBlock;

typedef enum tu_tb_mode_type {
    TU_TB_MODE_NONE = 0,
    TU_TB_MODE_SWITCH_TO_TB,
    TU_TB_MODE_BROKEN,
    TU_TB_MODE_STATIC,
    TB_GEN_CODE,
    BAD_TB
} tu_tb_mode_type;

struct separated_data {
    uint8_t eflag_out;
    uint8_t _top_in;
    uint8_t _top_out;
    TranslationBlock *next_tb[2];
    void     *ir1;
    int32_t rel_start;
    int32_t rel_end;
    uint8_t last_ir1_type;
    union {
        int is_first_tb;
        int tu_size;
    };
    tu_tb_mode_type tu_tb_mode;
    size_t offset_in_tu;
    target_ulong tu_id;
    target_ulong next_pc;
    target_ulong target_pc;
    uint32_t tu_unlink_stub_offset;
    uint32_t tu_link_ins;
    uint8_t *tu_search_addr;
    uint32_t codesize;
    target_ulong cs_base;
    void *env;
    bool isplit;
    uint32_t xmm_in;
    uint32_t xmm_out;
    uint32_t xmm_use;
    uint32_t xmm_def;
    uint32_t gpr_in;
    uint32_t gpr_out;
    uint32_t gpr_use;
    uint32_t gpr_def;
    uint8_t shbr_type;
};

struct TranslationBlock {
    uintptr_t jmp_list_head;
    uintptr_t jmp_list_next[2];
    uintptr_t jmp_dest[2];

    union {
        uintptr_t jmp_target_arg[2];
        struct tu_unlink tu_unlink;
    };
    union {
        uintptr_t jmp_stub_target_arg[2];
        uintptr_t jmp_indirect;
    };

    struct tb_tc tc;

    uintptr_t page_next[2];
    tb_page_addr_t page_addr[2];

    target_ulong pc;
    QemuSpin jmp_lock;

    uint32_t flags;
    uint32_t cflags;

    union {
        uint16_t jmp_reset_offset[2];
        uint16_t tu_jmp[2];
    };
    uint16_t jmp_stub_reset_offset[2];
    uint16_t first_jmp_align;
    uint16_t size;
    uint16_t icount;
    uint16_t jmp_insn_offset[2];

    uint32_t tu_search_addr_offset;
    uint16_t bool_flags;
    uint8_t  eflag_use;
    uint16_t eflags_target_arg[3];
    bool has_jcc_end_ptn;
    TBProfile profile;
    unsigned long *return_target_ptr;
    unsigned long next_86_pc;
    unsigned long checksum;
    unsigned long smc_data;
    int64_t lazypc[2];
    int8_t canlink[2];
    int8_t lazylink[2];
    uint64_t lazylinkinst[2];
    struct separated_data *s_data;
    void *ir1;
    struct TranslationBlock *next_tb[2];
    uint8_t last_ir1_type;
    target_ulong next_pc;
    target_ulong target_pc;
    uint32_t tu_unlink_stub_offset;
    uint32_t tu_link_ins;
    uint8_t *tu_search_addr;
    uint32_t codesize;
    target_ulong cs_base;
    void *env;
    bool isplit;
};

/* Code-cache and TB bookkeeping used by the retained optimiser. */
void  tcg_tb_insert(TranslationBlock *tb);
void *tcg_tb_alloc(TCGContext *s);
void *tcg_tb_alloc_full(TCGContext *s);
TranslationBlock *tcg_tb_lookup(uintptr_t pc);
void *tcg_splitwx_to_rx(void *p);
int   tcg_gen_code(TCGContext *s, uint8_t *search_pc);
void  tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr);
void  qemu_thread_jit_write(void);
void  qemu_spin_init(QemuSpin *s);
int   cpu_lduw_code(CPUArchState *env, uint64_t addr);
uint32_t cpu_cc_compute_all(CPUArchState *env, int op);
void  tcg_debug_assert_(int cond);
int tr_translate_tb(TranslationBlock *tb);
int encode_search(TranslationBlock *tb, void *p);
void mmap_unlock(void);
#define tcg_debug_assert(x) tcg_debug_assert_(!!(x))

typedef enum IR1_TYPE {
    IR1_TYPE_NORMAL = 0,
    IR1_TYPE_BRANCH,
    IR1_TYPE_JUMP,
    IR1_TYPE_CALL,
    IR1_TYPE_RET,
    IR1_TYPE_JUMPIN,
    IR1_TYPE_CALLIN,
    IR1_TYPE_SYSCALL,
    IR1_TYPE_COUNT,
    IR1_TYPE_JMP = IR1_TYPE_JUMP,
    IR1_TYPE_JMPIN = IR1_TYPE_JUMPIN,
} IR1_TYPE;
#define TB_JMP_OFFSET_INVALID 0xffff
#define TU_UNLINK_STUB_INVALID 0xffffffffu
#define CODE_GEN_ALIGN 16
#define ROUND_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_read(p) (*(p))
#define TU_TB_INDEX_NEXT 0
#define TU_TB_INDEX_TARGET 1
#define LATA_PC_LOW_BIT 2
#define TB_JMP_CACHE_SIZE 64
extern int option_fam_jmp_cache;

/* Thunk table + user-mode page flags (linux-user boundary). */
int ht_pc_thunk_lookup(uint64_t target);
int page_get_flags(uint64_t address);
void cpu_svm_check_intercept_param(CPUX86State *env, uint32_t type,
                                   uint64_t param, uintptr_t retaddr);
int cpu_get_apic_tpr(CPUX86State *env);
void cpu_set_apic_tpr(CPUX86State *env, uint8_t val);
void cpu_set_apic_base(CPUX86State *env, uint64_t val);
uint64_t cpu_get_apic_base(CPUX86State *env);
void cpu_set_mxcsr(CPUX86State *env, uint32_t val);
void cpu_load_efer(CPUX86State *env, uint64_t val);
void tlb_flush_page(CPUState *cpu, uint64_t addr);
void cpu_sync_avx_hflag(CPUX86State *env);
void cpu_sync_bndcs_hflags(CPUX86State *env);
void tlb_flush(CPUState *cpu);
void cpu_set_fpuc(CPUX86State *env, uint16_t fpuc);
uint64_t cpu_get_tsc(CPUX86State *env);
void update_mxcsr_from_sse_status(CPUX86State *env);
void update_mxcsr_status(CPUX86State *env);
void cpu_x86_update_cr0(CPUX86State *env, uint32_t new_cr0);
void cpu_x86_update_cr3(CPUX86State *env, target_ulong new_cr3);
void cpu_x86_update_cr4(CPUX86State *env, uint32_t new_cr4);
void cpu_x86_cpuid(CPUX86State *env, uint32_t index, uint32_t count,
                   uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
#define SVM_EXIT_CPUID 0x072
#define SVM_EXIT_HLT 0x078
#define SVM_EXIT_INVLPG 0x079
#define SVM_EXIT_MONITOR 0x08a
#define SVM_EXIT_MSR 0x07c
#define SVM_EXIT_MWAIT 0x08b
#define SVM_EXIT_PAUSE 0x077
#define SVM_EXIT_RDPMC 0x06f
#define SVM_EXIT_RDTSC 0x06e
#define SVM_EXIT_READ_CR0 0x000
#define SVM_EXIT_WRITE_CR0 0x010
#endif
