/*
 * Tree-owned runtime state for the bare translator.
 *
 * These replace LATX-side globals whose definitions lived in files that are
 * not part of this build (options loader, linux-user runtime, JRRA/imm-cache
 * implementations).  Behaviour is deliberately minimal: the translation path
 * only needs the storage, and the optimisations that read them are disabled
 * by their option flags.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "latx-options.h"
#include "mem.h"
#include "jrra.h"
#include "jrra-translator.h"
#include "imm-cache.h"
#include "latx-smc.h"
#include "latx-types.h"
#include "ir2.h"

/* --- translation environment --- */
ENV *lsenv;

/* --- counters and tracing knobs declared in latx-options.h --- */

/* --- user-mode page flag tree root referenced through its address --- */
IntervalTreeRoot pageflags_root;

/* --- host CPU model --- */
CPUState *first_cpu;
FILE *stderr;

/* --- TCG bookkeeping --- */
TCGContext *tcg_ctx;

void *tcg_tb_alloc(TCGContext *s)
{
    (void)s;
    return calloc(1, sizeof(TranslationBlock));
}

void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{
    /* Direct jump patching belongs to the code-cache layer; the engine links
     * blocks through the TB hash until that layer lands. */
    (void)tb;
    (void)n;
    (void)addr;
}

__attribute__((weak)) void latx_platform_cpu_loop_exit(CPUState *cs)
{
    (void)cs;
    for (;;) {
    }
}

void cpu_loop_exit(CPUState *cs)
{
    latx_platform_cpu_loop_exit(cs);
    for (;;) {
    }
}

void siglongjmp(sigjmp_buf env, int val)
{
    (void)env;
    (void)val;
    latx_platform_cpu_loop_exit(NULL);
    for (;;) {
    }
}

/* --- allocator shims used by the translator --- */
void *mm_malloc(int size)
{
    return malloc((size_t)size);
}

void *mm_calloc(int num, int size)
{
    return calloc((size_t)num, (size_t)size);
}

void *mm_realloc(void *ptr, int size)
{
    return realloc(ptr, (size_t)size);
}

void mm_free(void *ptr)
{
    free(ptr);
}

/* --- PC thunk table used by the call/return shortcut --- */
#define THUNK_SLOTS 64
static uint32_t thunk_pc[THUNK_SLOTS];
static int thunk_reg[THUNK_SLOTS];
static int thunk_used;

void ht_pc_thunk_insert(uint32_t thunk_addr, int reg_index)
{
    int i;

    for (i = 0; i < thunk_used; i++) {
        if (thunk_pc[i] == thunk_addr) {
            thunk_reg[i] = reg_index;
            return;
        }
    }
    if (thunk_used < THUNK_SLOTS) {
        thunk_pc[thunk_used] = thunk_addr;
        thunk_reg[thunk_used] = reg_index;
        thunk_used++;
    }
}

int ht_pc_thunk_lookup(uint64_t target)
{
    int i;

    for (i = 0; i < thunk_used; i++) {
        if (thunk_pc[i] == (uint32_t)target) {
            return thunk_reg[i];
        }
    }
    return -1;
}

/* --- user-mode page flags: no protection tracking yet --- */
int page_get_flags(uint64_t address)
{
    (void)address;
    return 0;
}

/* --- optimisation predicates: keep the conservative paths --- */
int use_indirect_jmp(TranslationBlock *tb)
{
    (void)tb;
    return 0;
}

int use_tu_jmp(TranslationBlock *tb)
{
    (void)tb;
    return 0;
}

unsigned long long aot_get_call_offset(unsigned long long addr)
{
    (void)addr;
    return 0;
}

int get_insn_len_readable(uint64_t pc)
{
    (void)pc;
    return 16; /* x86 architectural maximum */
}

/* --- immediate cache: no cache is built in this configuration --- */
void imm_cache_free_all(IMM_CACHE *cache)
{
    (void)cache;
}

void imm_cache_free_itemp(IMM_CACHE *cache, int itemp_num)
{
    (void)cache;
    (void)itemp_num;
}

/* --- JRRA (call/return shortcut) is disabled --- */
void jrra_context_switch_bt_to_native(void)
{
}

void jrra_emit_call_metadata(TranslationBlock *tb, target_ulong next_pc,
                             JrraCallType call_type)
{
    (void)tb;
    (void)next_pc;
    (void)call_type;
}

bool jrra_translate_ret(TranslationBlock *tb, IR2_OPND return_addr_opnd,
                        IR1_INST *pir1)
{
    (void)tb;
    (void)return_addr_opnd;
    (void)pir1;
    return false;
}

/* --- SMC fast store is disabled --- */
int smc_store_helper_vst_x4(void *env, uint64_t address,
                            int r0, int r1, int r2, int r3)
{
    (void)env;
    (void)address;
    (void)r0;
    (void)r1;
    (void)r2;
    (void)r3;
    return 0;
}

/* --- capstone entry points: decoding is wired up with the capstone build --- */

/*
 * SoftRAS platform hooks (strong overrides live in PLV0 VMM runtime, e.g.
 * baremetal/bare/tb_sys.c).  Because translated TBs keep live guest GPRs in
 * caller-saved LoongArch registers ($a2..$a5, $t5..$t8), EFLAGS in $a6, SW-LBT
 * CC in $tp, and DBT scratch/link registers in $r21/$fp/$a7, the ABI veneers
 * save and restore all non-$a0 caller-saved/DBT registers around C calls.
 */
__attribute__((weak)) void latx_platform_soft_ras_push(uint64_t ret_x86_pc)
{
    (void)ret_x86_pc;
}

__attribute__((weak)) uint64_t latx_platform_soft_ras_lookup(uint64_t target_x86_pc)
{
    (void)target_x86_pc;
    return 0;
}

#ifdef __loongarch__
__asm__(
".text\n"
".balign 4\n"
".globl latx_soft_ras_push_abi\n"
"latx_soft_ras_push_abi:\n"
"  addi.d $sp, $sp, -176\n"
"  st.d   $ra,  $sp, 0\n"
"  st.d   $tp,  $sp, 8\n"
"  st.d   $a0,  $sp, 16\n"
"  st.d   $a1,  $sp, 24\n"
"  st.d   $a2,  $sp, 32\n"
"  st.d   $a3,  $sp, 40\n"
"  st.d   $a4,  $sp, 48\n"
"  st.d   $a5,  $sp, 56\n"
"  st.d   $a6,  $sp, 64\n"
"  st.d   $a7,  $sp, 72\n"
"  st.d   $t0,  $sp, 80\n"
"  st.d   $t1,  $sp, 88\n"
"  st.d   $t2,  $sp, 96\n"
"  st.d   $t3,  $sp, 104\n"
"  st.d   $t4,  $sp, 112\n"
"  st.d   $t5,  $sp, 120\n"
"  st.d   $t6,  $sp, 128\n"
"  st.d   $t7,  $sp, 136\n"
"  st.d   $t8,  $sp, 144\n"
"  st.d   $r21, $sp, 152\n"
"  st.d   $fp,  $sp, 160\n"
"  bl     latx_platform_soft_ras_push\n"
"  ld.d   $ra,  $sp, 0\n"
"  ld.d   $tp,  $sp, 8\n"
"  ld.d   $a0,  $sp, 16\n"
"  ld.d   $a1,  $sp, 24\n"
"  ld.d   $a2,  $sp, 32\n"
"  ld.d   $a3,  $sp, 40\n"
"  ld.d   $a4,  $sp, 48\n"
"  ld.d   $a5,  $sp, 56\n"
"  ld.d   $a6,  $sp, 64\n"
"  ld.d   $a7,  $sp, 72\n"
"  ld.d   $t0,  $sp, 80\n"
"  ld.d   $t1,  $sp, 88\n"
"  ld.d   $t2,  $sp, 96\n"
"  ld.d   $t3,  $sp, 104\n"
"  ld.d   $t4,  $sp, 112\n"
"  ld.d   $t5,  $sp, 120\n"
"  ld.d   $t6,  $sp, 128\n"
"  ld.d   $t7,  $sp, 136\n"
"  ld.d   $t8,  $sp, 144\n"
"  ld.d   $r21, $sp, 152\n"
"  ld.d   $fp,  $sp, 160\n"
"  addi.d $sp,  $sp, 176\n"
"  jirl   $zero, $ra, 0\n"
"\n"
".balign 4\n"
".globl latx_soft_ras_lookup_abi\n"
"latx_soft_ras_lookup_abi:\n"
"  addi.d $sp, $sp, -176\n"
"  st.d   $ra,  $sp, 0\n"
"  st.d   $tp,  $sp, 8\n"
"  st.d   $a1,  $sp, 24\n"
"  st.d   $a2,  $sp, 32\n"
"  st.d   $a3,  $sp, 40\n"
"  st.d   $a4,  $sp, 48\n"
"  st.d   $a5,  $sp, 56\n"
"  st.d   $a6,  $sp, 64\n"
"  st.d   $a7,  $sp, 72\n"
"  st.d   $t0,  $sp, 80\n"
"  st.d   $t1,  $sp, 88\n"
"  st.d   $t2,  $sp, 96\n"
"  st.d   $t3,  $sp, 104\n"
"  st.d   $t4,  $sp, 112\n"
"  st.d   $t5,  $sp, 120\n"
"  st.d   $t6,  $sp, 128\n"
"  st.d   $t7,  $sp, 136\n"
"  st.d   $t8,  $sp, 144\n"
"  st.d   $r21, $sp, 152\n"
"  st.d   $fp,  $sp, 160\n"
"  bl     latx_platform_soft_ras_lookup\n"
"  ld.d   $ra,  $sp, 0\n"
"  ld.d   $tp,  $sp, 8\n"
"  ld.d   $a1,  $sp, 24\n"
"  ld.d   $a2,  $sp, 32\n"
"  ld.d   $a3,  $sp, 40\n"
"  ld.d   $a4,  $sp, 48\n"
"  ld.d   $a5,  $sp, 56\n"
"  ld.d   $a6,  $sp, 64\n"
"  ld.d   $a7,  $sp, 72\n"
"  ld.d   $t0,  $sp, 80\n"
"  ld.d   $t1,  $sp, 88\n"
"  ld.d   $t2,  $sp, 96\n"
"  ld.d   $t3,  $sp, 104\n"
"  ld.d   $t4,  $sp, 112\n"
"  ld.d   $t5,  $sp, 120\n"
"  ld.d   $t6,  $sp, 128\n"
"  ld.d   $t7,  $sp, 136\n"
"  ld.d   $t8,  $sp, 144\n"
"  ld.d   $r21, $sp, 152\n"
"  ld.d   $fp,  $sp, 160\n"
"  addi.d $sp,  $sp, 176\n"
"  jirl   $zero, $ra, 0\n"
);
#else
void latx_soft_ras_push_abi(uint64_t ret_x86_pc)
{
    latx_platform_soft_ras_push(ret_x86_pc);
}

uint64_t latx_soft_ras_lookup_abi(uint64_t target_x86_pc)
{
    return latx_platform_soft_ras_lookup(target_x86_pc);
}
#endif
