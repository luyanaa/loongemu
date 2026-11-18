#ifndef LATX_BARE_CPU_H
#define LATX_BARE_CPU_H
/* Freestanding extraction of the CPUX86State field set the retained LATX
 * pipeline touches.  The field block below is copied from the upstream header
 * by tools (no upstream edits); only the surrounding QEMU type closure is
 * replaced with freestanding placeholders. */
#include "qemu/osdep.h"
#include "fpu/softfloat-types.h"

#define CPU_NB_REGS 16
#define NB_OPMASK_REGS 8
#define MAX_FIXED_COUNTERS 3
#define MAX_GP_COUNTERS 4
#define MCE_BANKS_DEF 10
#define MSR_MTRRcap_VCNT 8

typedef uint64_t target_ulong;
typedef int64_t  target_long;
typedef uint64_t FeatureWordArray[18];
typedef struct MTRRVar { uint64_t base; uint64_t mask; } MTRRVar;
typedef enum TPRAccess { TPR_ACCESS_READ, TPR_ACCESS_WRITE } TPRAccess;
typedef struct CPUBreakpoint CPUBreakpoint;
typedef struct CPUWatchpoint CPUWatchpoint;
typedef struct latx_ucontext ucontext_t;
typedef struct CPUCacheInfo { int unused; } CPUCacheInfo;
typedef struct CPUCaches { CPUCacheInfo *l1d_cache, *l1i_cache, *l2_cache, *l3_cache; } CPUCaches;
typedef struct HVFX86LazyFlags { target_ulong result; target_ulong auxbits; } HVFX86LazyFlags;

enum {
    R_EAX = 0, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI,
    R_R8, R_R9, R_R10, R_R11, R_R12, R_R13, R_R14, R_R15,
    R_AL = 0, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH,
};

typedef enum X86Seg {
    R_ES = 0, R_CS, R_SS, R_DS, R_FS, R_GS, R_LDTR, R_TR,
} X86Seg;

typedef struct SegmentCache {
    uint32_t selector;
    target_ulong base;
    uint32_t limit;
    uint32_t flags;
} SegmentCache;

#define MMREG_UNION(n, bits)        \
    union n {                       \
        uint8_t  _b_##n[(bits)/8];  \
        uint16_t _w_##n[(bits)/16]; \
        uint32_t _l_##n[(bits)/32]; \
        uint64_t _q_##n[(bits)/64]; \
        float16  _h_##n[(bits)/16]; \
        float32  _s_##n[(bits)/32]; \
        float64  _d_##n[(bits)/64]; \
    }

typedef union {
    uint8_t _b[16];
    uint16_t _w[8];
    uint32_t _l[4];
    uint64_t _q[2];
} XMMReg;

typedef union {
    uint8_t _b[32];
    uint16_t _w[16];
    uint32_t _l[8];
    uint64_t _q[4];
} YMMReg;

typedef MMREG_UNION(ZMMReg, 512) ZMMReg;
typedef MMREG_UNION(MMXReg, 64)  MMXReg;

/* Little-endian host layout, same mapping as the reference header. */
#define ZMM_B(n) _b_ZMMReg[n]
#define ZMM_W(n) _w_ZMMReg[n]
#define ZMM_L(n) _l_ZMMReg[n]
#define ZMM_H(n) _h_ZMMReg[n]
#define ZMM_S(n) _s_ZMMReg[n]
#define ZMM_Q(n) _q_ZMMReg[n]
#define ZMM_D(n) _d_ZMMReg[n]
#define MMX_B(n) _b_MMXReg[n]
#define MMX_W(n) _w_MMXReg[n]
#define MMX_L(n) _l_MMXReg[n]
#define MMX_S(n) _s_MMXReg[n]
#define MMX_Q(n) _q_MMXReg[n]

typedef union {
    floatx80 d __attribute__((aligned(16)));
    MMXReg mmx;
} FPReg;

typedef union CPU_LDoubleU {
    floatx80 d;
    struct { uint64_t lower; uint16_t upper; } l;
} CPU_LDoubleU;

typedef struct BNDReg { uint64_t lb; uint64_t ub; } BNDReg;
typedef struct BNDCSReg { uint64_t cfgu; uint64_t sts; } BNDCSReg;

#define CC_C    0x0001
#define CC_P    0x0004
#define CC_A    0x0010
#define CC_Z    0x0040
#define CC_S    0x0080
#define CC_O    0x0800
#define TF_MASK                 0x00000100
#define IF_MASK                 0x00000200
#define DF_MASK                 0x00000400
#define IOPL_MASK               0x00003000
#define NT_MASK                 0x00004000
#define RF_MASK                 0x00010000
#define VM_MASK                 0x00020000
#define AC_MASK                 0x00040000
#define VIF_MASK                0x00080000
#define VIP_MASK                0x00100000
#define ID_MASK                 0x00200000
#define HF_CPL_SHIFT         0
#define HF_INHIBIT_IRQ_SHIFT 3
#define HF_CS32_SHIFT        4
#define HF_SS32_SHIFT        5
#define HF_ADDSEG_SHIFT      6
#define HF_PE_SHIFT          7
#define HF_TF_SHIFT          8 /* must be same as eflags */
#define HF_MP_SHIFT          9 /* the order must be MP, EM, TS */
#define HF_EM_SHIFT         10
#define HF_TS_SHIFT         11
#define HF_IOPL_SHIFT       12 /* must be same as eflags */
#define HF_LMA_SHIFT        14 /* only used on x86_64: long mode active */
#define HF_CS64_SHIFT       15 /* only used on x86_64: 64 bit code segment  */
#define HF_RF_SHIFT         16 /* must be same as eflags */
#define HF_VM_SHIFT         17 /* must be same as eflags */
#define HF_AC_SHIFT         18 /* must be same as eflags */
#define HF_SMM_SHIFT        19 /* CPU in SMM mode */
#define HF_SVME_SHIFT       20 /* SVME enabled (copy of EFER.SVME) */
#define HF_GUEST_SHIFT      21 /* SVM intercepts are active */
#define HF_OSFXSR_SHIFT     22 /* CR4.OSFXSR */
#define HF_SMAP_SHIFT       23 /* CR4.SMAP */
#define HF_IOBPT_SHIFT      24 /* an io breakpoint enabled */
#define HF_MPX_EN_SHIFT     25 /* MPX Enabled (CR4+XCR0+BNDCFGx) */
#define HF_MPX_IU_SHIFT     26 /* BND registers in-use */
#define HF_UMIP_SHIFT       27 /* CR4.UMIP */
#define HF_AVX_EN_SHIFT     28 /* AVX Enabled (CR4+XCR0) */
#define HF_CPL_MASK          (3 << HF_CPL_SHIFT)
#define HF_INHIBIT_IRQ_MASK  (1 << HF_INHIBIT_IRQ_SHIFT)
#define HF_CS32_MASK         (1 << HF_CS32_SHIFT)
#define HF_SS32_MASK         (1 << HF_SS32_SHIFT)
#define HF_ADDSEG_MASK       (1 << HF_ADDSEG_SHIFT)
#define HF_PE_MASK           (1 << HF_PE_SHIFT)
#define HF_TF_MASK           (1 << HF_TF_SHIFT)
#define HF_MP_MASK           (1 << HF_MP_SHIFT)
#define HF_EM_MASK           (1 << HF_EM_SHIFT)
#define HF_TS_MASK           (1 << HF_TS_SHIFT)
#define HF_IOPL_MASK         (3 << HF_IOPL_SHIFT)
#define HF_LMA_MASK          (1 << HF_LMA_SHIFT)
#define HF_CS64_MASK         (1 << HF_CS64_SHIFT)
#define HF_RF_MASK           (1 << HF_RF_SHIFT)
#define HF_VM_MASK           (1 << HF_VM_SHIFT)
#define HF_AC_MASK           (1 << HF_AC_SHIFT)
#define HF_SMM_MASK          (1 << HF_SMM_SHIFT)
#define HF_SVME_MASK         (1 << HF_SVME_SHIFT)
#define HF_GUEST_MASK        (1 << HF_GUEST_SHIFT)
#define HF_OSFXSR_MASK       (1 << HF_OSFXSR_SHIFT)
#define HF_SMAP_MASK         (1 << HF_SMAP_SHIFT)
#define HF_IOBPT_MASK        (1 << HF_IOBPT_SHIFT)
#define HF_MPX_EN_MASK       (1 << HF_MPX_EN_SHIFT)
#define HF_MPX_IU_MASK       (1 << HF_MPX_IU_SHIFT)
#define HF_UMIP_MASK         (1 << HF_UMIP_SHIFT)
#define HF_AVX_EN_MASK       (1 << HF_AVX_EN_SHIFT)
#define HF2_GIF_SHIFT            0 /* if set CPU takes interrupts */
#define HF2_HIF_SHIFT            1 /* value of IF_MASK when entering SVM */
#define HF2_NMI_SHIFT            2 /* CPU serving NMI */
#define HF2_VINTR_SHIFT          3 /* value of V_INTR_MASKING bit */
#define HF2_SMM_INSIDE_NMI_SHIFT 4 /* CPU serving SMI nested inside NMI */
#define HF2_MPX_PR_SHIFT         5 /* BNDCFGx.BNDPRESERVE */
#define HF2_NPT_SHIFT            6 /* Nested Paging enabled */
#define HF2_IGNNE_SHIFT          7 /* Ignore CR0.NE=0 */
#define HF2_GIF_MASK            (1 << HF2_GIF_SHIFT)
#define HF2_HIF_MASK            (1 << HF2_HIF_SHIFT)
#define HF2_NMI_MASK            (1 << HF2_NMI_SHIFT)
#define HF2_VINTR_MASK          (1 << HF2_VINTR_SHIFT)
#define HF2_SMM_INSIDE_NMI_MASK (1 << HF2_SMM_INSIDE_NMI_SHIFT)
#define HF2_MPX_PR_MASK         (1 << HF2_MPX_PR_SHIFT)
#define HF2_NPT_MASK            (1 << HF2_NPT_SHIFT)
#define HF2_IGNNE_MASK          (1 << HF2_IGNNE_SHIFT)
#define CR0_PE_SHIFT 0
#define CR0_MP_SHIFT 1
#define CR0_PE_MASK  (1U << 0)
#define CR0_MP_MASK  (1U << 1)
#define CR0_EM_MASK  (1U << 2)
#define CR0_TS_MASK  (1U << 3)
#define CR0_ET_MASK  (1U << 4)
#define CR0_NE_MASK  (1U << 5)
#define CR0_WP_MASK  (1U << 16)
#define CR0_AM_MASK  (1U << 18)
#define CR0_PG_MASK  (1U << 31)
#define CR4_VME_MASK  (1U << 0)
#define CR4_PVI_MASK  (1U << 1)
#define CR4_TSD_MASK  (1U << 2)
#define CR4_DE_MASK   (1U << 3)
#define CR4_PSE_MASK  (1U << 4)
#define CR4_PAE_MASK  (1U << 5)
#define CR4_MCE_MASK  (1U << 6)
#define CR4_PGE_MASK  (1U << 7)
#define CR4_PCE_MASK  (1U << 8)
#define CR4_OSFXSR_SHIFT 9
#define CR4_OSFXSR_MASK (1U << CR4_OSFXSR_SHIFT)
#define CR4_OSXMMEXCPT_MASK  (1U << 10)
#define CR4_LA57_MASK   (1U << 12)
#define CR4_VMXE_MASK   (1U << 13)
#define CR4_SMXE_MASK   (1U << 14)
#define CR4_FSGSBASE_MASK (1U << 16)
#define CR4_PCIDE_MASK  (1U << 17)
#define CR4_OSXSAVE_MASK (1U << 18)
#define CR4_SMEP_MASK   (1U << 20)
#define CR4_SMAP_MASK   (1U << 21)
#define CR4_PKE_MASK   (1U << 22)
#define CR4_PKS_MASK   (1U << 24)
#define CC_DST  (env->cc_dst)
#define CC_SRC  (env->cc_src)
#define CC_SRC2 (env->cc_src2)
#define CC_OP   (env->cc_op)

typedef enum {
    CC_OP_DYNAMIC, /* must use dynamic code to get cc_op */
    CC_OP_EFLAGS,  /* all cc are explicitly computed, CC_SRC = flags */

    CC_OP_MULB, /* modify all flags, C, O = (CC_SRC != 0) */
    CC_OP_MULW,
    CC_OP_MULL,
    CC_OP_MULQ,

    CC_OP_ADDB, /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_ADDW,
    CC_OP_ADDL,
    CC_OP_ADDQ,

    CC_OP_ADCB, /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_ADCW,
    CC_OP_ADCL,
    CC_OP_ADCQ,

    CC_OP_SUBB, /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_SUBW,
    CC_OP_SUBL,
    CC_OP_SUBQ,

    CC_OP_SBBB, /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_SBBW,
    CC_OP_SBBL,
    CC_OP_SBBQ,

    CC_OP_LOGICB, /* modify all flags, CC_DST = res */
    CC_OP_LOGICW,
    CC_OP_LOGICL,
    CC_OP_LOGICQ,

    CC_OP_INCB, /* modify all flags except, CC_DST = res, CC_SRC = C */
    CC_OP_INCW,
    CC_OP_INCL,
    CC_OP_INCQ,

    CC_OP_DECB, /* modify all flags except, CC_DST = res, CC_SRC = C  */
    CC_OP_DECW,
    CC_OP_DECL,
    CC_OP_DECQ,

    CC_OP_SHLB, /* modify all flags, CC_DST = res, CC_SRC.msb = C */
    CC_OP_SHLW,
    CC_OP_SHLL,
    CC_OP_SHLQ,

    CC_OP_SARB, /* modify all flags, CC_DST = res, CC_SRC.lsb = C */
    CC_OP_SARW,
    CC_OP_SARL,
    CC_OP_SARQ,

    CC_OP_BMILGB, /* Z,S via CC_DST, C = SRC==0; O=0; P,A undefined */
    CC_OP_BMILGW,
    CC_OP_BMILGL,
    CC_OP_BMILGQ,

    CC_OP_ADCX, /* CC_DST = C, CC_SRC = rest.  */
    CC_OP_ADOX, /* CC_DST = O, CC_SRC = rest.  */
    CC_OP_ADCOX, /* CC_DST = C, CC_SRC2 = O, CC_SRC = rest.  */

    CC_OP_CLR, /* Z set, all other flags clear.  */
    CC_OP_POPCNT, /* Z via CC_SRC, all other flags clear.  */

    CC_OP_NB,
} CCOp;

enum {
    LATX_FPU_MODE_MMX,
    LATX_FPU_MODE_X87,
    LATX_FPU_MODE_SIMD,
    LATX_FPU_MODE_RESTORED,
};

typedef struct CPUX86State {
#ifdef CONFIG_LATX
    void*  checksum_fail_tb;
    void *ibtc_table_p;
    uint64_t vregs[5];
    /* for debug,can be removed later */
    uint64_t mips_regs[32];
    int fcsr;
    /* TODO: why? in new qemu has no next_eip member */
    target_ulong exception_next_eip;
    void *tb_jmp_cache_ptr; /* struct TranslationBlock ** */
    struct __sys {
        int codemode;
    } sys;
 #ifdef CONFIG_LATX_DEBUG
    uint64_t last_store_insn;
    uint64_t tb_exec_count;
#define FUNC_DEPTH 20
    char **call_func;
    int func_index;
    int last_func_index;
 #endif
    uint8_t mode_fpu;
    bool fpu_clobber;
#endif
    /* standard registers */
    uint64_t regs[CPU_NB_REGS];
    target_ulong eip;
    target_ulong eflags; /* eflags register. During CPU emulation, CC
                        flags and DF are set to zero because they are
                        stored elsewhere */

    /* emulator internal eflags handling */
    target_ulong cc_dst;
    target_ulong cc_src;
    target_ulong cc_src2;
    uint32_t cc_op;
    int32_t df; /* D flag : 1 if D = 0, -1 if D = 1 */
    uint32_t hflags; /* TB flags, see HF_xxx constants. These flags
                        are known at translation time. */
    uint32_t hflags2; /* various other flags, see HF2_xxx constants. */

    /* segments */
    SegmentCache segs[6]; /* selector values */
    SegmentCache ldt;
    SegmentCache tr;
    SegmentCache gdt; /* only base and limit are used */
    SegmentCache idt; /* only base and limit are used */

    target_ulong cr[5]; /* NOTE: cr1 is unused */
    int32_t a20_mask;

    BNDReg bnd_regs[4];
    BNDCSReg bndcs_regs;
    uint64_t msr_bndcfgs;
    uint64_t efer;

    /* Beginning of state preserved by INIT (dummy marker).  */
    struct {} start_init_save;

    /* FPU state */
    unsigned int fpstt; /* top of stack index */
    uint16_t fpus;
    uint16_t fpuc;
    uint8_t fptags[8];   /* 0 = valid, 1 = empty */
    FPReg fpregs[8];
    /* KVM-only so far */
    uint16_t fpop;
    uint64_t fpip;
    uint64_t fpdp;
    /* TODO imm_cached_value size should correspond
        to IMM_CACHE_CAPACITY */
    int64_t imm_cached_value[4];
    /* emulator internal variables */
    float_status fp_status;
    floatx80 ft0;

    float_status mmx_status; /* for 3DNow! float ops */
    float_status sse_status;
    uint32_t mxcsr;
    ZMMReg xmm_regs[CPU_NB_REGS == 8 ? 8 : 32];
    ZMMReg xmm_t0;
    MMXReg mmx_t0;

    XMMReg ymmh_regs[CPU_NB_REGS];

    uint64_t opmask_regs[NB_OPMASK_REGS];
    YMMReg zmmh_regs[CPU_NB_REGS];
    ZMMReg hi16_zmm_regs[CPU_NB_REGS];

    /* sysenter registers */
    uint32_t sysenter_cs;
    target_ulong sysenter_esp;
    target_ulong sysenter_eip;
    uint64_t star;

    uint64_t vm_hsave;

#ifdef TARGET_X86_64
    target_ulong lstar;
    target_ulong cstar;
    target_ulong fmask;
    target_ulong kernelgsbase;
#endif

    uint64_t tsc;
    uint64_t tsc_adjust;
    uint64_t tsc_deadline;
    uint64_t tsc_aux;

    uint64_t xcr0;

    uint64_t mcg_status;
    uint64_t msr_ia32_misc_enable;
    uint64_t msr_ia32_feature_control;

    uint64_t msr_fixed_ctr_ctrl;
    uint64_t msr_global_ctrl;
    uint64_t msr_global_status;
    uint64_t msr_global_ovf_ctrl;
    uint64_t msr_fixed_counters[MAX_FIXED_COUNTERS];
    uint64_t msr_gp_counters[MAX_GP_COUNTERS];
    uint64_t msr_gp_evtsel[MAX_GP_COUNTERS];

    uint64_t pat;
    uint32_t smbase;
    uint64_t msr_smi_count;

    uint32_t pkru;
    uint32_t pkrs;
    uint32_t tsx_ctrl;

    uint64_t spec_ctrl;
    uint64_t virt_ssbd;

    /* End of state preserved by INIT (dummy marker).  */
    struct {} end_init_save;

    uint64_t system_time_msr;
    uint64_t wall_clock_msr;
    uint64_t steal_time_msr;
    uint64_t async_pf_en_msr;
    uint64_t async_pf_int_msr;
    uint64_t pv_eoi_en_msr;
    uint64_t poll_control_msr;

    /* exception/interrupt handling */
    int error_code;
    int exception_index;
    int exception_is_int;
#ifndef CONFIG_LATX
    target_ulong exception_next_eip;
#endif
    target_ulong dr[8]; /* debug registers; note dr4 and dr5 are unused */
    union {
        struct CPUBreakpoint *cpu_breakpoint[4];
        struct CPUWatchpoint *cpu_watchpoint[4];
    }; /* break/watchpoints for dr[0..3] */
    int old_exception;  /* exception in flight */

    uint64_t vm_vmcb;
    uint64_t tsc_offset;
    uint64_t intercept;
    uint16_t intercept_cr_read;
    uint16_t intercept_cr_write;
    uint16_t intercept_dr_read;
    uint16_t intercept_dr_write;
    uint32_t intercept_exceptions;
    uint64_t nested_cr3;
    uint32_t nested_pg_mode;
    uint8_t v_tpr;

    /* KVM states, automatically cleared on reset */
    uint8_t nmi_injected;
    uint8_t nmi_pending;

    uintptr_t retaddr;

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    /* Fields after this point are preserved across CPU reset. */

    /* processor features (e.g. for CPUID insn) */
    /* Minimum cpuid leaf 7 value */
    uint32_t cpuid_level_func7;
    /* Actual cpuid leaf 7 value */
    uint32_t cpuid_min_level_func7;
    /* Minimum level/xlevel/xlevel2, based on CPU model + features */
    uint32_t cpuid_min_level, cpuid_min_xlevel, cpuid_min_xlevel2;
    /* Maximum level/xlevel/xlevel2 value for auto-assignment: */
    uint32_t cpuid_max_level, cpuid_max_xlevel, cpuid_max_xlevel2;
    /* Actual level/xlevel/xlevel2 value: */
    uint32_t cpuid_level, cpuid_xlevel, cpuid_xlevel2;
    uint32_t cpuid_vendor1;
    uint32_t cpuid_vendor2;
    uint32_t cpuid_vendor3;
    uint32_t cpuid_version;
    FeatureWordArray features;
    /* Features that were explicitly enabled/disabled */
    FeatureWordArray user_features;
    uint32_t cpuid_model[12];
    /* Cache information for CPUID.  When legacy-cache=on, the cache data
     * on each CPUID leaf will be different, because we keep compatibility
     * with old QEMU versions.
     */
    CPUCaches cache_info_cpuid2, cache_info_cpuid4, cache_info_amd;

    /* MTRRs */
    uint64_t mtrr_fixed[11];
    uint64_t mtrr_deftype;
    MTRRVar mtrr_var[MSR_MTRRcap_VCNT];

    /* For KVM */
    uint32_t mp_state;
    int32_t exception_nr;
    int32_t interrupt_injected;
    uint8_t soft_interrupt;
    uint8_t exception_pending;
    uint8_t exception_injected;
    uint8_t has_error_code;
    uint8_t exception_has_payload;
    uint64_t exception_payload;
    uint32_t ins_len;
    uint32_t sipi_vector;
    bool tsc_valid;
    int64_t tsc_khz;
    int64_t user_tsc_khz; /* for sanity check only */
    uint64_t apic_bus_freq;
#if defined(CONFIG_KVM) || defined(CONFIG_HVF)
    void *xsave_buf;
#endif
#if defined(CONFIG_KVM)
    struct kvm_nested_state *nested_state;
#endif
#if defined(CONFIG_HVF)
    HVFX86LazyFlags hvf_lflags;
    void *hvf_mmio_buf;
#endif

    uint64_t mcg_cap;
    uint64_t mcg_ctl;
    uint64_t mcg_ext_ctl;
    uint64_t mce_banks[MCE_BANKS_DEF*4];
    uint64_t xstate_bv;

    /* vmstate */
    uint16_t fpus_vmstate;
    uint16_t fptag_vmstate;
    uint16_t fpregs_format_vmstate;

    uint64_t xss;
    uint32_t umwait;

    TPRAccess tpr_access_type;

    unsigned nr_dies;
#ifdef CONFIG_LATX
    ucontext_t *puc;
    uintptr_t insn_save[2];
    target_ulong kzt_guest_stack_base;
#ifdef CONFIG_LATX_KZT
    void *kzt_guest_tls_allocation;
    void *kzt_guest_thread_state;
    void *kzt_guest_tls_parent_snapshot;
    void *kzt_libc_semantic_state;
#endif
#endif
    uint64_t mapping_range[2];
    uint64_t *pc_map_cache;
} CPUX86State;

typedef union X86LegacyXSaveArea {
    struct {
        uint16_t fcw;
        uint16_t fsw;
        uint8_t ftw;
        uint8_t reserved;
        uint16_t fpop;
        uint64_t fpip;
        uint64_t fpdp;
        uint32_t mxcsr;
        uint32_t mxcsr_mask;
        FPReg fpregs[8];
        uint8_t xmm_regs[16][16];
    };
    uint8_t data[512];
} X86LegacyXSaveArea;

typedef struct X86XSaveHeader {
    uint64_t xstate_bv;
    uint64_t xcomp_bv;
    uint64_t reserve0;
    uint8_t reserved[40];
} X86XSaveHeader;

/* Ext. save area 2: AVX State */
typedef struct XSaveAVX {
    uint8_t ymmh[16][16];
} XSaveAVX;

/* Ext. save area 3: BNDREG */
typedef struct XSaveBNDREG {
    BNDReg bnd_regs[4];
} XSaveBNDREG;

/* Ext. save area 4: BNDCSR */
typedef union XSaveBNDCSR {
    BNDCSReg bndcsr;
    uint8_t data[64];
} XSaveBNDCSR;

/* Ext. save area 5: Opmask */
typedef struct XSaveOpmask {
    uint64_t opmask_regs[NB_OPMASK_REGS];
} XSaveOpmask;

/* Ext. save area 6: ZMM_Hi256 */
typedef struct XSaveZMM_Hi256 {
    uint8_t zmm_hi256[16][32];
} XSaveZMM_Hi256;

/* Ext. save area 7: Hi16_ZMM */
typedef struct XSaveHi16_ZMM {
    uint8_t hi16_zmm[16][64];
} XSaveHi16_ZMM;

/* Ext. save area 9: PKRU state */
typedef struct XSavePKRU {
    uint32_t pkru;
    uint32_t padding;
} XSavePKRU;

typedef struct X86XSaveArea {
    X86LegacyXSaveArea legacy;
    X86XSaveHeader header;

    /* Extended save areas: */

    /* AVX State: */
    XSaveAVX avx_state;
    uint8_t padding[960 - 576 - sizeof(XSaveAVX)];
    /* MPX State: */
    XSaveBNDREG bndreg_state;
    XSaveBNDCSR bndcsr_state;
    /* AVX-512 State: */
    XSaveOpmask opmask_state;
    XSaveZMM_Hi256 zmm_hi256_state;
    XSaveHi16_ZMM hi16_zmm_state;
    /* PKRU State: */
    XSavePKRU pkru_state;
} X86XSaveArea;

#define MSR_IA32_TSC                    0x10
#define MSR_IA32_APICBASE               0x1b
#define MSR_IA32_APICBASE_BSP           (1<<8)
#define MSR_IA32_APICBASE_ENABLE        (1<<11)
#define MSR_IA32_APICBASE_EXTD          (1 << 10)
#define MSR_IA32_APICBASE_BASE          (0xfffffU<<12)
#define MSR_IA32_FEATURE_CONTROL        0x0000003a
#define MSR_TSC_ADJUST                  0x0000003b
#define MSR_IA32_SPEC_CTRL              0x48
#define MSR_VIRT_SSBD                   0xc001011f
#define MSR_IA32_PRED_CMD               0x49
#define MSR_IA32_UCODE_REV              0x8b
#define MSR_IA32_CORE_CAPABILITY        0xcf
#define MSR_IA32_ARCH_CAPABILITIES      0x10a
#define MSR_IA32_PERF_CAPABILITIES      0x345
#define MSR_IA32_TSX_CTRL		0x122
#define MSR_IA32_TSCDEADLINE            0x6e0
#define MSR_IA32_PKRS                   0x6e1
#define MSR_P6_PERFCTR0                 0xc1
#define MSR_IA32_SMBASE                 0x9e
#define MSR_SMI_COUNT                   0x34
#define MSR_CORE_THREAD_COUNT           0x35
#define MSR_IA32_SYSENTER_CS            0x174
#define MSR_IA32_SYSENTER_ESP           0x175
#define MSR_IA32_SYSENTER_EIP           0x176
#define MSR_MCG_CAP                     0x179
#define MSR_MCG_STATUS                  0x17a
#define MSR_MCG_CTL                     0x17b
#define MSR_MCG_EXT_CTL                 0x4d0
#define MSR_P6_EVNTSEL0                 0x186
#define MSR_IA32_PERF_STATUS            0x198
#define MSR_IA32_MISC_ENABLE            0x1a0
#define MSR_IA32_MISC_ENABLE_DEFAULT    1
#define MSR_IA32_MISC_ENABLE_MWAIT      (1ULL << 18)
#define MSR_PAT                         0x277
#define MSR_CORE_PERF_FIXED_CTR0        0x309
#define MSR_CORE_PERF_FIXED_CTR1        0x30a
#define MSR_CORE_PERF_FIXED_CTR2        0x30b
#define MSR_CORE_PERF_FIXED_CTR_CTRL    0x38d
#define MSR_CORE_PERF_GLOBAL_STATUS     0x38e
#define MSR_CORE_PERF_GLOBAL_CTRL       0x38f
#define MSR_CORE_PERF_GLOBAL_OVF_CTRL   0x390
#define MSR_MC0_CTL                     0x400
#define MSR_MC0_STATUS                  0x401
#define MSR_MC0_ADDR                    0x402
#define MSR_MC0_MISC                    0x403
#define MSR_IA32_RTIT_OUTPUT_BASE       0x560
#define MSR_IA32_RTIT_OUTPUT_MASK       0x561
#define MSR_IA32_RTIT_CTL               0x570
#define MSR_IA32_RTIT_STATUS            0x571
#define MSR_IA32_RTIT_CR3_MATCH         0x572
#define MSR_IA32_RTIT_ADDR0_A           0x580
#define MSR_IA32_RTIT_ADDR0_B           0x581
#define MSR_IA32_RTIT_ADDR1_A           0x582
#define MSR_IA32_RTIT_ADDR1_B           0x583
#define MSR_IA32_RTIT_ADDR2_A           0x584
#define MSR_IA32_RTIT_ADDR2_B           0x585
#define MSR_IA32_RTIT_ADDR3_A           0x586
#define MSR_IA32_RTIT_ADDR3_B           0x587
#define MSR_EFER                        0xc0000080
#define MSR_EFER_SCE   (1 << 0)
#define MSR_EFER_LME   (1 << 8)
#define MSR_EFER_LMA   (1 << 10)
#define MSR_EFER_NXE   (1 << 11)
#define MSR_EFER_SVME  (1 << 12)
#define MSR_EFER_FFXSR (1 << 14)
#define MSR_STAR                        0xc0000081
#define MSR_LSTAR                       0xc0000082
#define MSR_CSTAR                       0xc0000083
#define MSR_FMASK                       0xc0000084
#define MSR_FSBASE                      0xc0000100
#define MSR_GSBASE                      0xc0000101
#define MSR_KERNELGSBASE                0xc0000102
#define MSR_TSC_AUX                     0xc0000103
#define MSR_VM_HSAVE_PA                 0xc0010117
#define MSR_IA32_BNDCFGS                0x00000d90
#define MSR_IA32_XSS                    0x00000da0
#define MSR_IA32_UMWAIT_CONTROL         0xe1
#define MSR_IA32_VMX_BASIC              0x00000480
#define MSR_IA32_VMX_PINBASED_CTLS      0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS     0x00000482
#define MSR_IA32_VMX_EXIT_CTLS          0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS         0x00000484
#define MSR_IA32_VMX_MISC               0x00000485
#define MSR_IA32_VMX_CR0_FIXED0         0x00000486
#define MSR_IA32_VMX_CR0_FIXED1         0x00000487
#define MSR_IA32_VMX_CR4_FIXED0         0x00000488
#define MSR_IA32_VMX_CR4_FIXED1         0x00000489
#define MSR_IA32_VMX_VMCS_ENUM          0x0000048a
#define MSR_IA32_VMX_PROCBASED_CTLS2    0x0000048b
#define MSR_IA32_VMX_EPT_VPID_CAP       0x0000048c
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS  0x0000048d
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x0000048e
#define MSR_IA32_VMX_TRUE_EXIT_CTLS      0x0000048f
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS     0x00000490
#define MSR_IA32_VMX_VMFUNC             0x00000491
#define MSR_ARCH_CAP_RDCL_NO            (1U << 0)
#define MSR_ARCH_CAP_IBRS_ALL           (1U << 1)
#define MSR_ARCH_CAP_RSBA               (1U << 2)
#define MSR_ARCH_CAP_SKIP_L1DFL_VMENTRY (1U << 3)
#define MSR_ARCH_CAP_SSB_NO             (1U << 4)
#define MSR_ARCH_CAP_MDS_NO             (1U << 5)
#define MSR_ARCH_CAP_PSCHANGE_MC_NO     (1U << 6)
#define MSR_ARCH_CAP_TSX_CTRL_MSR       (1U << 7)
#define MSR_ARCH_CAP_TAA_NO             (1U << 8)
#define MSR_CORE_CAP_SPLIT_LOCK_DETECT  (1U << 5)
#define MSR_VMX_BASIC_VMCS_REVISION_MASK             0x7FFFFFFFull
#define MSR_VMX_BASIC_VMXON_REGION_SIZE_MASK         (0x00001FFFull << 32)
#define MSR_VMX_BASIC_VMCS_MEM_TYPE_MASK             (0x003C0000ull << 32)
#define MSR_VMX_BASIC_DUAL_MONITOR                   (1ULL << 49)
#define MSR_VMX_BASIC_INS_OUTS                       (1ULL << 54)
#define MSR_VMX_BASIC_TRUE_CTLS                      (1ULL << 55)
#define MSR_VMX_MISC_PREEMPTION_TIMER_SHIFT_MASK     0x1Full
#define MSR_VMX_MISC_STORE_LMA                       (1ULL << 5)
#define MSR_VMX_MISC_ACTIVITY_HLT                    (1ULL << 6)
#define MSR_VMX_MISC_ACTIVITY_SHUTDOWN               (1ULL << 7)
#define MSR_VMX_MISC_ACTIVITY_WAIT_SIPI              (1ULL << 8)
#define MSR_VMX_MISC_MAX_MSR_LIST_SIZE_MASK          0x0E000000ull
#define MSR_VMX_MISC_VMWRITE_VMEXIT                  (1ULL << 29)
#define MSR_VMX_MISC_ZERO_LEN_INJECT                 (1ULL << 30)
#define MSR_VMX_EPT_EXECONLY                         (1ULL << 0)
#define MSR_VMX_EPT_PAGE_WALK_LENGTH_4               (1ULL << 6)
#define MSR_VMX_EPT_PAGE_WALK_LENGTH_5               (1ULL << 7)
#define MSR_VMX_EPT_UC                               (1ULL << 8)
#define MSR_VMX_EPT_WB                               (1ULL << 14)
#define MSR_VMX_EPT_2MB                              (1ULL << 16)
#define MSR_VMX_EPT_1GB                              (1ULL << 17)
#define MSR_VMX_EPT_INVEPT                           (1ULL << 20)
#define MSR_VMX_EPT_AD_BITS                          (1ULL << 21)
#define MSR_VMX_EPT_ADVANCED_VMEXIT_INFO             (1ULL << 22)
#define MSR_VMX_EPT_INVEPT_SINGLE_CONTEXT            (1ULL << 25)
#define MSR_VMX_EPT_INVEPT_ALL_CONTEXT               (1ULL << 26)
#define MSR_VMX_EPT_INVVPID                          (1ULL << 32)
#define MSR_VMX_EPT_INVVPID_SINGLE_ADDR              (1ULL << 40)
#define MSR_VMX_EPT_INVVPID_SINGLE_CONTEXT           (1ULL << 41)
#define MSR_VMX_EPT_INVVPID_ALL_CONTEXT              (1ULL << 42)
#define MSR_VMX_EPT_INVVPID_SINGLE_CONTEXT_NOGLOBALS (1ULL << 43)
#define MSR_VMX_VMFUNC_EPT_SWITCHING                 (1ULL << 0)

#define XSTATE_FP_BIT                   0
#define XSTATE_SSE_BIT                  1
#define XSTATE_YMM_BIT                  2
#define XSTATE_BNDREGS_BIT              3
#define XSTATE_BNDCSR_BIT               4
#define XSTATE_OPMASK_BIT               5
#define XSTATE_PKRU_BIT                 9
#define XSTATE_FP_MASK                  (1ULL << XSTATE_FP_BIT)
#define XSTATE_SSE_MASK                 (1ULL << XSTATE_SSE_BIT)
#define XSTATE_YMM_MASK                 (1ULL << XSTATE_YMM_BIT)
#define XSTATE_BNDREGS_MASK             (1ULL << XSTATE_BNDREGS_BIT)
#define XSTATE_BNDCSR_MASK              (1ULL << XSTATE_BNDCSR_BIT)
#define XSTATE_OPMASK_MASK              (1ULL << XSTATE_OPMASK_BIT)
#define XSTATE_PKRU_MASK                (1ULL << XSTATE_PKRU_BIT)
#define CPUID_FP87 (1U << 0)
#define CPUID_VME  (1U << 1)
#define CPUID_DE   (1U << 2)
#define CPUID_PSE  (1U << 3)
#define CPUID_TSC  (1U << 4)
#define CPUID_MSR  (1U << 5)
#define CPUID_PAE  (1U << 6)
#define CPUID_MCE  (1U << 7)
#define CPUID_CX8  (1U << 8)
#define CPUID_APIC (1U << 9)
#define CPUID_SEP  (1U << 11) /* sysenter/sysexit */
#define CPUID_MTRR (1U << 12)
#define CPUID_PGE  (1U << 13)
#define CPUID_MCA  (1U << 14)
#define CPUID_CMOV (1U << 15)
#define CPUID_PAT  (1U << 16)
#define CPUID_PSE36   (1U << 17)
#define CPUID_PN   (1U << 18)
#define CPUID_CLFLUSH (1U << 19)
#define CPUID_DTS (1U << 21)
#define CPUID_ACPI (1U << 22)
#define CPUID_MMX  (1U << 23)
#define CPUID_FXSR (1U << 24)
#define CPUID_SSE  (1U << 25)
#define CPUID_SSE2 (1U << 26)
#define CPUID_SS (1U << 27)
#define CPUID_HT (1U << 28)
#define CPUID_TM (1U << 29)
#define CPUID_IA64 (1U << 30)
#define CPUID_PBE (1U << 31)
#define CPUID_EXT_SSE3     (1U << 0)
#define CPUID_EXT_PCLMULQDQ (1U << 1)
#define CPUID_EXT_DTES64   (1U << 2)
#define CPUID_EXT_MONITOR  (1U << 3)
#define CPUID_EXT_DSCPL    (1U << 4)
#define CPUID_EXT_VMX      (1U << 5)
#define CPUID_EXT_SMX      (1U << 6)
#define CPUID_EXT_EST      (1U << 7)
#define CPUID_EXT_TM2      (1U << 8)
#define CPUID_EXT_SSSE3    (1U << 9)
#define CPUID_EXT_CID      (1U << 10)
#define CPUID_EXT_FMA      (1U << 12)
#define CPUID_EXT_CX16     (1U << 13)
#define CPUID_EXT_XTPR     (1U << 14)
#define CPUID_EXT_PDCM     (1U << 15)
#define CPUID_EXT_PCID     (1U << 17)
#define CPUID_EXT_DCA      (1U << 18)
#define CPUID_EXT_SSE41    (1U << 19)
#define CPUID_EXT_SSE42    (1U << 20)
#define CPUID_EXT_X2APIC   (1U << 21)
#define CPUID_EXT_MOVBE    (1U << 22)
#define CPUID_EXT_POPCNT   (1U << 23)
#define CPUID_EXT_TSC_DEADLINE_TIMER (1U << 24)
#define CPUID_EXT_AES      (1U << 25)
#define CPUID_EXT_XSAVE    (1U << 26)
#define CPUID_EXT_OSXSAVE  (1U << 27)
#define CPUID_EXT_AVX      (1U << 28)
#define CPUID_EXT_F16C     (1U << 29)
#define CPUID_EXT_RDRAND   (1U << 30)
#define CPUID_EXT_HYPERVISOR  (1U << 31)
#define CPUID_EXT2_FPU     (1U << 0)
#define CPUID_EXT2_VME     (1U << 1)
#define CPUID_EXT2_DE      (1U << 2)
#define CPUID_EXT2_PSE     (1U << 3)
#define CPUID_EXT2_TSC     (1U << 4)
#define CPUID_EXT2_MSR     (1U << 5)
#define CPUID_EXT2_PAE     (1U << 6)
#define CPUID_EXT2_MCE     (1U << 7)
#define CPUID_EXT2_CX8     (1U << 8)
#define CPUID_EXT2_APIC    (1U << 9)
#define CPUID_EXT2_SYSCALL (1U << 11)
#define CPUID_EXT2_MTRR    (1U << 12)
#define CPUID_EXT2_PGE     (1U << 13)
#define CPUID_EXT2_MCA     (1U << 14)
#define CPUID_EXT2_CMOV    (1U << 15)
#define CPUID_EXT2_PAT     (1U << 16)
#define CPUID_EXT2_PSE36   (1U << 17)
#define CPUID_EXT2_MP      (1U << 19)
#define CPUID_EXT2_NX      (1U << 20)
#define CPUID_EXT2_MMXEXT  (1U << 22)
#define CPUID_EXT2_MMX     (1U << 23)
#define CPUID_EXT2_FXSR    (1U << 24)
#define CPUID_EXT2_FFXSR   (1U << 25)
#define CPUID_EXT2_PDPE1GB (1U << 26)
#define CPUID_EXT2_RDTSCP  (1U << 27)
#define CPUID_EXT2_LM      (1U << 29)
#define CPUID_EXT2_3DNOWEXT (1U << 30)
#define CPUID_EXT2_3DNOW   (1U << 31)
#define CPUID_EXT2_AMD_ALIASES (CPUID_EXT2_FPU | CPUID_EXT2_VME | \
#define CPUID_EXT3_LAHF_LM (1U << 0)
#define CPUID_EXT3_CMP_LEG (1U << 1)
#define CPUID_EXT3_SVM     (1U << 2)
#define CPUID_EXT3_EXTAPIC (1U << 3)
#define CPUID_EXT3_CR8LEG  (1U << 4)
#define CPUID_EXT3_ABM     (1U << 5)
#define CPUID_EXT3_SSE4A   (1U << 6)
#define CPUID_EXT3_MISALIGNSSE (1U << 7)
#define CPUID_EXT3_3DNOWPREFETCH (1U << 8)
#define CPUID_EXT3_OSVW    (1U << 9)
#define CPUID_EXT3_IBS     (1U << 10)
#define CPUID_EXT3_XOP     (1U << 11)
#define CPUID_EXT3_SKINIT  (1U << 12)
#define CPUID_EXT3_WDT     (1U << 13)
#define CPUID_EXT3_LWP     (1U << 15)
#define CPUID_EXT3_FMA4    (1U << 16)
#define CPUID_EXT3_TCE     (1U << 17)
#define CPUID_EXT3_NODEID  (1U << 19)
#define CPUID_EXT3_TBM     (1U << 21)
#define CPUID_EXT3_TOPOEXT (1U << 22)
#define CPUID_EXT3_PERFCORE (1U << 23)
#define CPUID_EXT3_PERFNB  (1U << 24)
#define CPUID_SVM_NPT             (1U << 0)
#define CPUID_SVM_LBRV            (1U << 1)
#define CPUID_SVM_SVMLOCK         (1U << 2)
#define CPUID_SVM_NRIPSAVE        (1U << 3)
#define CPUID_SVM_TSCSCALE        (1U << 4)
#define CPUID_SVM_VMCBCLEAN       (1U << 5)
#define CPUID_SVM_FLUSHASID       (1U << 6)
#define CPUID_SVM_DECODEASSIST    (1U << 7)
#define CPUID_SVM_PAUSEFILTER     (1U << 10)
#define CPUID_SVM_PFTHRESHOLD     (1U << 12)
#define CPUID_SVM_AVIC            (1U << 13)
#define CPUID_SVM_V_VMSAVE_VMLOAD (1U << 15)
#define CPUID_SVM_VGIF            (1U << 16)
#define CPUID_SVM_SVME_ADDR_CHK   (1U << 28)
#define CPUID_7_0_EBX_FSGSBASE          (1U << 0)
#define CPUID_7_0_EBX_BMI1              (1U << 3)
#define CPUID_7_0_EBX_HLE               (1U << 4)
#define CPUID_7_0_EBX_AVX2              (1U << 5)
#define CPUID_7_0_EBX_SMEP              (1U << 7)
#define CPUID_7_0_EBX_BMI2              (1U << 8)
#define CPUID_7_0_EBX_ERMS              (1U << 9)
#define CPUID_7_0_EBX_INVPCID           (1U << 10)
#define CPUID_7_0_EBX_RTM               (1U << 11)
#define CPUID_7_0_EBX_MPX               (1U << 14)
#define CPUID_7_0_EBX_AVX512F           (1U << 16)
#define CPUID_7_0_EBX_AVX512DQ          (1U << 17)
#define CPUID_7_0_EBX_RDSEED            (1U << 18)
#define CPUID_7_0_EBX_ADX               (1U << 19)
#define CPUID_7_0_EBX_SMAP              (1U << 20)
#define CPUID_7_0_EBX_AVX512IFMA        (1U << 21)
#define CPUID_7_0_EBX_PCOMMIT           (1U << 22)
#define CPUID_7_0_EBX_CLFLUSHOPT        (1U << 23)
#define CPUID_7_0_EBX_CLWB              (1U << 24)
#define CPUID_7_0_EBX_INTEL_PT          (1U << 25)
#define CPUID_7_0_EBX_AVX512PF          (1U << 26)
#define CPUID_7_0_EBX_AVX512ER          (1U << 27)
#define CPUID_7_0_EBX_AVX512CD          (1U << 28)
#define CPUID_7_0_EBX_SHA_NI            (1U << 29)
#define CPUID_7_0_EBX_AVX512BW          (1U << 30)
#define CPUID_7_0_EBX_AVX512VL          (1U << 31)
#define CPUID_7_0_ECX_AVX512_VBMI       (1U << 1)
#define CPUID_7_0_ECX_UMIP              (1U << 2)
#define CPUID_7_0_ECX_PKU               (1U << 3)
#define CPUID_7_0_ECX_OSPKE             (1U << 4)
#define CPUID_7_0_ECX_WAITPKG           (1U << 5)
#define CPUID_7_0_ECX_AVX512_VBMI2      (1U << 6)
#define CPUID_7_0_ECX_GFNI              (1U << 8)
#define CPUID_7_0_ECX_VAES              (1U << 9)
#define CPUID_7_0_ECX_VPCLMULQDQ        (1U << 10)
#define CPUID_7_0_ECX_AVX512VNNI        (1U << 11)
#define CPUID_7_0_ECX_AVX512BITALG      (1U << 12)
#define CPUID_7_0_ECX_AVX512_VPOPCNTDQ  (1U << 14)
#define CPUID_7_0_ECX_LA57              (1U << 16)
#define CPUID_7_0_ECX_RDPID             (1U << 22)
#define CPUID_7_0_ECX_BUS_LOCK_DETECT   (1U << 24)
#define CPUID_7_0_ECX_CLDEMOTE          (1U << 25)
#define CPUID_7_0_ECX_MOVDIRI           (1U << 27)
#define CPUID_7_0_ECX_MOVDIR64B         (1U << 28)
#define CPUID_7_0_ECX_PKS               (1U << 31)
#define CPUID_7_0_EDX_AVX512_4VNNIW     (1U << 2)
#define CPUID_7_0_EDX_AVX512_4FMAPS     (1U << 3)
#define CPUID_7_0_EDX_FSRM              (1U << 4)
#define CPUID_7_0_EDX_AVX512_VP2INTERSECT (1U << 8)
#define CPUID_7_0_EDX_SERIALIZE         (1U << 14)
#define CPUID_7_0_EDX_TSX_LDTRK         (1U << 16)
#define CPUID_7_0_EDX_AVX512_FP16       (1U << 23)
#define CPUID_7_0_EDX_SPEC_CTRL         (1U << 26)
#define CPUID_7_0_EDX_STIBP             (1U << 27)
#define CPUID_7_0_EDX_ARCH_CAPABILITIES (1U << 29)
#define CPUID_7_0_EDX_CORE_CAPABILITY   (1U << 30)
#define CPUID_7_0_EDX_SPEC_CTRL_SSBD    (1U << 31)
#define CPUID_7_1_EAX_AVX512_BF16       (1U << 5)
#define CPUID_14_0_ECX_LIP              (1U << 31)
#define CPUID_8000_0008_EBX_CLZERO      (1U << 0)
#define CPUID_8000_0008_EBX_XSAVEERPTR  (1U << 2)
#define CPUID_8000_0008_EBX_WBNOINVD    (1U << 9)
#define CPUID_8000_0008_EBX_IBPB        (1U << 12)
#define CPUID_8000_0008_EBX_IBRS        (1U << 14)
#define CPUID_8000_0008_EBX_STIBP       (1U << 15)
#define CPUID_8000_0008_EBX_AMD_SSBD    (1U << 24)
#define CPUID_XSAVE_XSAVEOPT   (1U << 0)
#define CPUID_XSAVE_XSAVEC     (1U << 1)
#define CPUID_XSAVE_XGETBV1    (1U << 2)
#define CPUID_XSAVE_XSAVES     (1U << 3)
#define CPUID_6_EAX_ARAT       (1U << 2)
#define CPUID_APM_INVTSC       (1U << 8)
#define CPUID_VENDOR_SZ      12
#define CPUID_VENDOR_INTEL_1 0x756e6547 /* "Genu" */
#define CPUID_VENDOR_INTEL_2 0x49656e69 /* "ineI" */
#define CPUID_VENDOR_INTEL_3 0x6c65746e /* "ntel" */
#define CPUID_VENDOR_INTEL "GenuineIntel"
#define CPUID_VENDOR_AMD_1   0x68747541 /* "Auth" */
#define CPUID_VENDOR_AMD_2   0x69746e65 /* "enti" */
#define CPUID_VENDOR_AMD_3   0x444d4163 /* "cAMD" */
#define CPUID_VENDOR_AMD   "AuthenticAMD"
#define CPUID_VENDOR_VIA   "CentaurHauls"
#define CPUID_VENDOR_HYGON    "HygonGenuine"
#define CPUID_MWAIT_IBE     (1U << 1) /* Interrupts can exit capability */
#define CPUID_MWAIT_EMX     (1U << 0) /* enumeration supported */
#define CPUID_TOPOLOGY_LEVEL_INVALID  (0U << 8)
#define CPUID_TOPOLOGY_LEVEL_SMT      (1U << 8)
#define CPUID_TOPOLOGY_LEVEL_CORE     (2U << 8)
#define CPUID_TOPOLOGY_LEVEL_DIE      (5U << 8)
typedef enum FeatureWord {
    FEAT_1_EDX,         /* CPUID[1].EDX */
    FEAT_1_ECX,         /* CPUID[1].ECX */
    FEAT_7_0_EBX,       /* CPUID[EAX=7,ECX=0].EBX */
    FEAT_7_0_ECX,       /* CPUID[EAX=7,ECX=0].ECX */
    FEAT_7_0_EDX,       /* CPUID[EAX=7,ECX=0].EDX */
    FEAT_7_1_EAX,       /* CPUID[EAX=7,ECX=1].EAX */
    FEAT_8000_0001_EDX, /* CPUID[8000_0001].EDX */
    FEAT_8000_0001_ECX, /* CPUID[8000_0001].ECX */
    FEAT_8000_0007_EDX, /* CPUID[8000_0007].EDX */
    FEAT_8000_0008_EBX, /* CPUID[8000_0008].EBX */
    FEAT_C000_0001_EDX, /* CPUID[C000_0001].EDX */
    FEAT_KVM,           /* CPUID[4000_0001].EAX (KVM_CPUID_FEATURES) */
    FEAT_KVM_HINTS,     /* CPUID[4000_0001].EDX */
    FEAT_HYPERV_EAX,    /* CPUID[4000_0003].EAX */
    FEAT_HYPERV_EBX,    /* CPUID[4000_0003].EBX */
    FEAT_HYPERV_EDX,    /* CPUID[4000_0003].EDX */
    FEAT_HV_RECOMM_EAX, /* CPUID[4000_0004].EAX */
    FEAT_HV_NESTED_EAX, /* CPUID[4000_000A].EAX */
    FEAT_SVM,           /* CPUID[8000_000A].EDX */
    FEAT_XSAVE,         /* CPUID[EAX=0xd,ECX=1].EAX */
    FEAT_6_EAX,         /* CPUID[6].EAX */
    FEAT_XSAVE_COMP_LO, /* CPUID[EAX=0xd,ECX=0].EAX */
    FEAT_XSAVE_COMP_HI, /* CPUID[EAX=0xd,ECX=0].EDX */
    FEAT_ARCH_CAPABILITIES,
    FEAT_CORE_CAPABILITY,
    FEAT_PERF_CAPABILITIES,
    FEAT_VMX_PROCBASED_CTLS,
    FEAT_VMX_SECONDARY_CTLS,
    FEAT_VMX_PINBASED_CTLS,
    FEAT_VMX_EXIT_CTLS,
    FEAT_VMX_ENTRY_CTLS,
    FEAT_VMX_MISC,
    FEAT_VMX_EPT_VPID_CAPS,
    FEAT_VMX_BASIC,
    FEAT_VMX_VMFUNC,
    FEAT_14_0_ECX,
    FEATURE_WORDS,
} FeatureWord;

#define MSR_MTRRphysBase(reg)           (0x200 + 2 * (reg))
#define MSR_MTRRphysMask(reg)           (0x200 + 2 * (reg) + 1)
#define MSR_MTRRphysIndex(addr)         ((((addr) & ~1u) - 0x200) / 2)

#define MSR_MTRRfix64K_00000            0x250
#define MSR_MTRRfix16K_80000            0x258
#define MSR_MTRRfix16K_A0000            0x259
#define MSR_MTRRfix4K_C0000             0x268
#define MSR_MTRRfix4K_C8000             0x269
#define MSR_MTRRfix4K_D0000             0x26a
#define MSR_MTRRfix4K_D8000             0x26b
#define MSR_MTRRfix4K_E0000             0x26c
#define MSR_MTRRfix4K_E8000             0x26d
#define MSR_MTRRfix4K_F0000             0x26e
#define MSR_MTRRfix4K_F8000             0x26f

#define MCG_CTL_P       (1ULL<<8)   /* MCG_CAP register available */

#define MCG_SER_P       (1ULL<<24) /* MCA recovery/new status bits */

#define MCG_LMCE_P      (1ULL<<27) /* Local Machine Check Supported */

#define MCG_CAP_BANKS_MASK 0xff

#define MCG_STATUS_RIPV (1ULL<<0)   /* restart ip valid */

#define MCG_STATUS_EIPV (1ULL<<1)   /* ip points to correct instruction */

#define MCG_STATUS_MCIP (1ULL<<2)   /* machine check in progress */

#define MCG_STATUS_LMCE (1ULL<<3)   /* Local MCE signaled */

#define MCG_EXT_CTL_LMCE_EN (1ULL<<0) /* Local MCE enabled */

#define MSR_MTRRcap_FIXRANGE_SUPPORT    (1 << 8)

#define MSR_MTRRcap_WC_SUPPORTED        (1 << 10)

#define MSR_MTRRdefType                 0x2ff

#define MSR_MTRRcap                     0xfe

typedef CPUX86State CPUArchState;
typedef struct MemTxAttrs { uint64_t opaque; } MemTxAttrs;
static inline MemTxAttrs cpu_get_mem_attrs(CPUArchState *env)
{
    MemTxAttrs a = { 0 };
    (void)env;
    return a;
}
struct X86CPU { CPUX86State env; void *apic_state; uint64_t ucode_rev; };
typedef struct X86CPU X86CPU;
#define X86_CPU(cpu) ((X86CPU *)(cpu))
#define env_archcpu(env) ((X86CPU *)(env))
#define CPU(cs) ((CPUState *)(cs))
#define CPU_NEXT(cs) ((CPUState *)0)
typedef struct CPUState {
    void *env_ptr;
    int cpu_index;
    bool running;
    int exception_index;
    uint32_t tcg_cflags;
    sigjmp_buf jmp_env;
    bool can_do_io;
    bool halted;
    bool singlestep_enabled;
} CPUState;
#define env_cpu(env) ((CPUState *)(env))
#endif
