#ifndef LATX_BARE_PRELUDE_H
#define LATX_BARE_PRELUDE_H
/* Force-included with -include.  Neutralises the headers that belonged to the
 * stripped QEMU surfaces (AOT cache, host signal handling, KZT wrapper, Zydis
 * tracer) without editing the upstream submodule.  Each guard below matches
 * the header we drop; anything they used to declare is declared here instead.
 */
#define __AOT_H_
#define __TU_H_
#define __AOT_LIB_H_
#define _AOT_LINK_SEG_H_
#define __AOT_PAGE_H_
#define LATX_AOT_READER_H
#define _AOT_MERGE_H_
#define _AOT_RECOVER_TB_H_
#define __AOT_SMC_H_
#define LATX_AOT_EXIT_H
#define LATX_AOT_VERSION_H
#define __SEGMENT_H_
#define __TS_H_
#define LATX_SMC_RELOAD_H
#define __PATHCOLL_H_
#define AOT_FILE_CTX_H
#define _LATX_CONFIG_H_
#define _LATX_SIGNAL_H_

#include "capStone-defs.h"
#include "qemu/osdep.h"
#include "latx-types.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "latx_bare.h"
#include "cpu.h"
#include "exec/exec-all.h"

/* --- options that used to live in latx-options.h / latx-config.h --- */
extern int option_mem_test;
extern int option_native_sigsegv;
extern int option_latx_debug;
extern int option_latx_aot;
extern int option_latx_tu;
extern int option_latx_kzt;
extern int option_smc_opt;
extern int option_latx_o0;
extern int option_latx_o1;
extern int option_latx_o2;
extern int option_latx_o3;

/* --- aot rel-kind constants (aot.h is stripped, the names are the ABI) --- */
enum aot_rel_kind {

    B_PROLOGUE,
    B_EPILOGUE,
    B_EPILOGUE_RET_ID_3,
    B_EPILOGUE_RET_ID_1,
    B_EPILOGUE_RET_ID_0,
    JIRL_EPILOGUE_RET_ID_1,
    JIRL_EPILOGUE_RET_ID_0,
    B_EPILOGUE_RET_0,
    B_NATIVE_JMP_GLUE2,

    LOAD_TB_ADDR,
    LOAD_RIP_OFF,
    LOAD_CALL_TARGET,
    LOAD_SYSCALL_OPTIMIZE_CONFIRM,

    /* Helper functions relocation. Assume we need to call a helper in our tb,
     * we'll load its address into a tmp reg and then jmp to that address, but
     * if latx has changed its code and all helper address has changed, all aot
     * tb will need to be relocated. */
    LOAD_HELPER_BEGIN,

    LOAD_VPAES_ENC_TABLES,
    LOAD_VPAES_DEC_TABLES,
    LOAD_VPAES_KEYGEN_TABLES,
    LOAD_VPAES_ENC_TABLES_XV,
    LOAD_VPAES_DEC_TABLES_XV,

    LOAD_HELPER_TRACE_SESSION_BEGIN,
    LOAD_HELPER_UPDATE_MXCSR_STATUS,
    LOAD_HELPER_FPATAN,
    LOAD_HELPER_FPTAN,
    LOAD_HELPER_FPREM,
    LOAD_HELPER_FPREM1,
    LOAD_HELPER_FRNDINT,
    LOAD_HELPER_F2XM1,
    LOAD_HELPER_FXTRACT,
    LOAD_HELPER_FYL2X,
    LOAD_HELPER_FYL2XP1,
    LOAD_HELPER_FSINCOS,
    LOAD_HELPER_FSIN,
    LOAD_HELPER_FCOS,
    LOAD_HELPER_FBLD_ST0,
    LOAD_HELPER_FBST_ST0,
    LOAD_HELPER_FXSAVE,
    LOAD_HELPER_FXRSTOR,
    LOAD_HELPER_CONVERT_FPREGS_X80_TO_64,
    LOAD_HELPER_CONVERT_FPREGS_64_TO_X80,
    LOAD_HELPER_UPDATE_FP_STATUS,
    LOAD_HELPER_CPUID,
    LOAD_HELPER_RAISE_INT,
    LOAD_HELPER_RAISE_ILLOP,
    LOAD_HELPER_RAISE_TRAPOP,
    LOAD_HELPER_RAISE_GPF,
    LOAD_HELPER_RAISE_INTO,
    LOAD_HELPER_RAISE_BOUND,
    LOAD_HELPER_RAISE_SYSCALL,
    LOAD_HELPER_WRITE_CR,
    LOAD_HELPER_READ_CR,
    LOAD_HELPER_RDMSR,
    LOAD_HELPER_WRMSR,
    LOAD_HELPER_IN,
    LOAD_HELPER_CHECK_ACCESS,
    LOAD_HELPER_DELIVER_FAULT,
    LOAD_HELPER_OUT,
    LOAD_HELPER_XGETBV,
    LOAD_HELPER_EFLAGTF,
    LOAD_HELPER_SMC_ST,
    LOAD_HELPER_SMC_VST,
    LOAD_HELPER_SMC_VST_X4,
    LOAD_HELPER_KZT_GET_ALTERNATE,

    LOAD_HOST_POW,
    LOAD_HOST_SIN,
    LOAD_HOST_COS,
    LOAD_HOST_ATAN2,
    LOAD_HOST_LOGB,
    LOAD_HOST_LOG2,
    LOAD_HOST_SINCOS,
    LOAD_HOST_PFTABLE,
    LOAD_HOST_LATLOCK,
    LOAD_HOST_RAISE_EX,
    LOAD_PAGEFLAGS_ROOT,
    LOAD_HELPER_PCMPESTRI_XMM,
    LOAD_HELPER_PCMPESTRM_XMM,
    LOAD_HELPER_PCMPISTRI_XMM,
    LOAD_HELPER_PCMPISTRM_XMM,
    LOAD_HELPER_AESIMC_XMM,
    LOAD_HELPER_AESKEYGENASSIST_XMM,
    LOAD_HELPER_AESDEC_XMM,
    LOAD_HELPER_AESDECLAST_XMM,
    LOAD_HOST_AES_TE_LATX,
    LOAD_HOST_AES_SBOX_LATX,
    LOAD_HOST_AES_VPAES_LASX,
    LOAD_HELPER_SHA1NEXTE,
    LOAD_HELPER_SHA1MSG1,
    LOAD_HELPER_SHA1MSG2,
    LOAD_HELPER_SHA256MSG1,
    LOAD_HELPER_SHA256MSG2,
    LOAD_HELPER_SHA1RNDS4_F0,
    LOAD_HELPER_SHA1RNDS4_F1,
    LOAD_HELPER_SHA1RNDS4_F2,
    LOAD_HELPER_SHA1RNDS4_F3,
    LOAD_HELPER_SHA256RNDS2_XMM0,
    LOAD_HELPER_XSETBV,
    LOAD_HELPER_XSAVE,
    LOAD_HELPER_XSAVEOPT,
    LOAD_HELPER_XRSTOR,
    LOAD_HELPER_VPCLMULQDQ_YMM,
    LOAD_HELPER_VPCLMULQDQ_XMM,
    LOAD_HELPER_VAESDEC_YMM,
    LOAD_HELPER_VAESDEC_XMM,
    LOAD_HELPER_VAESDECLAST_YMM,
    LOAD_HELPER_VAESDECLAST_XMM,
    LOAD_HELPER_VAESENC_YMM,
    LOAD_HELPER_VAESENC_XMM,
    LOAD_HELPER_VAESENCLAST_YMM,
    LOAD_HELPER_VAESENCLAST_XMM,
    LOAD_HELPER_CVTPH2PS_YMM,
    LOAD_HELPER_CVTPH2PS_XMM,
    LOAD_HELPER_CVTPS2PH_YMM,
    LOAD_HELPER_CVTPS2PH_XMM,

    LOAD_HELPER_END,

    LOAD_TUNNEL_ADDR_BEGIN,
};

/* --- x87 / SIMD helpers called through addresses or directly --- */
void get_last_info(TranslationBlock *tb, void *pir1);
int  use_indirect_jmp(TranslationBlock *tb);
int  use_tu_jmp(TranslationBlock *tb);
void ht_pc_thunk_insert(uint32_t thunk_addr, int reg_index);
unsigned long long aot_get_call_offset(unsigned long long addr);
void helper_vaesdec_xmm();
void helper_vaesdec_ymm();
void helper_vaesdeclast_xmm();
void helper_vaesdeclast_ymm();
void helper_cvtph2ps_xmm();
void helper_cvtph2ps_ymm();
void helper_cvtps2ph_xmm();
void helper_cvtps2ph_ymm();
void helper_pcmpestri_xmm();
void helper_pcmpestrm_xmm();
void helper_pcmpistri_xmm();
void helper_pcmpistrm_xmm();
void helper_aesdec_xmm();
void helper_aesdeclast_xmm();
void helper_aesimc_xmm();
void helper_aeskeygenassist_xmm();
void helper_sha1msg1();
void helper_sha1msg2();
void helper_sha1nexte();
void helper_sha1rnds4_f0();
void helper_sha1rnds4_f1();
void helper_sha1rnds4_f2();
void helper_sha1rnds4_f3();
void helper_sha256msg1();
void helper_sha256msg2();
void helper_sha256rnds2_xmm0();

void *helper_tb_lookup_ptr(CPUArchState *env);
void update_fp_status(CPUX86State *env);
void update_mxcsr_status(CPUX86State *env);





void latx_runtime_init(void);
/*
 * Fill CPUX86State.features[] from what cpu_x86_cpuid() advertises.  The MSR
 * helpers mask writes with those words (EFER.LME is only writable when long
 * mode is advertised), so an environment that was merely zeroed silently
 * swallows wrmsr.
 */
void latx_x86_env_init_features(CPUX86State *env);
int latx_gen_prologue(void *code_buf);
int latx_gen_epilogue(void *code_buf);
extern ADDR context_switch_bt_to_native;
extern ADDR context_switch_native_to_bt;
extern ADDR context_switch_native_to_bt_ret_0;
extern ADDR context_switch_native_to_bt_ret_id_3;
extern ADDR context_switch_native_to_bt_ret_id_1;
extern ADDR context_switch_native_to_bt_ret_id_0;

/* --- signal/backtrace surfaces referenced by error.h --- */
#define latx_backtrace(...) do { } while (0)

#endif
