#ifndef LATR_RISCV_ENV_H
#define LATR_RISCV_ENV_H

#include <stddef.h>
#include <stdint.h>

#define LATR_RISCV_CSR_COUNT 4096
#define LATR_RISCV_PRIV_U 0
#define LATR_RISCV_PRIV_S 1
#define LATR_RISCV_PRIV_M 3
#define LATR_RISCV_CSR_STIMECMP 0x14d
#define LATR_RISCV_CSR_MIP      0x344
#define LATR_RISCV_MIP_STIP     (UINT64_C(1) << 5)
#define LATR_RISCV_TIMEBASE_HZ  UINT64_C(1000000000)

/* RVV baseline configuration for LATR. */
#define LATR_RVV_VLEN  128u
#define LATR_RVV_VLENB (LATR_RVV_VLEN / 8u)
#define LATR_RVV_ELEN  64u
#define LATR_RVV_VSTART_MASK ((UINT64_C(1) << 7) - 1) /* lg2(VLEN) bits */
#define RVV_VTYPE_VLMUL(vt) ((unsigned)((vt) & 7u))
#define RVV_VTYPE_VSEW(vt)  ((unsigned)(((vt) >> 3) & 7u))
#define RVV_VTYPE_VTA(vt)   ((unsigned)(((vt) >> 6) & 1u))
#define RVV_VTYPE_VMA(vt)   ((unsigned)(((vt) >> 7) & 1u))

typedef struct LoongemuVClock LoongemuVClock;
typedef struct LoongemuMemMap LoongemuMemMap;
typedef struct LoongemuPlic LoongemuPlic;
typedef struct LoongemuMmioBus LoongemuMmioBus;


#define LATR_TLB_ENTRIES 64u

/*
 * Software translation cache. An entry is only used while the leaf PTE still
 * holds the recorded value, so guest page-table edits are observed even
 * without an SFENCE; SFENCE.VMA/FENCE.I still flush entries explicitly.
 */
typedef struct LatrTlbEntry {
    uint64_t va_page;   /* VA >> 12 */
    uint64_t satp;      /* exact satp value (mode/ASID/root) */
    uint64_t pte;       /* validated leaf PTE contents */
    uint64_t pte_host;  /* host address of the leaf PTE */
    uint64_t pa_page;   /* leaf PPN << 12 */
    uint64_t page_mask; /* VA/PA offset mask for the leaf level */
    uint32_t valid;
} LatrTlbEntry;

typedef struct CPUArchState CPUArchState;

struct CPUArchState {
    uint64_t gpr[32];
    uint64_t fpr[32];
    uint64_t pc;
    uint64_t fcsr;
    uint64_t exception;
    uint64_t badaddr;
    uint64_t csr[LATR_RISCV_CSR_COUNT];

    /* Architectural privilege (U=0, S=1, M=3); zero-initialized state is U. */
    uint64_t privilege;

    /* LR/SC reservation tracked independently of the host LLbit. */
    uint64_t reservation_addr;
    uint64_t reservation_size;
    uint64_t reservation_epoch;
    uint64_t reservation_valid;

    /* Host call / runtime integration state; appended to preserve IR offsets. */
    uint64_t host_return_address;
    /* Phase 3 sets the VM-shared clock before guest entry; CSR_TIME reads it. */
    LoongemuVClock *vclock;
    /* Phase 3 consumes this after TB exit for local instruction-cache invalidation. */
    uint64_t fence_i_pending;
    /*
     * Sstc contract for phase 3: csr[STIMECMP] is in vclock nanoseconds
     * (TIMEBASE_HZ); writes set this request bit. Runtime consumes/clears it
     * after TB exit and updates MIP.STIP as virtual time crosses the deadline.
     */
    uint64_t stimecmp_update_pending;

    /* Result of the last latr_resolve_guest_address() call (0 = success). */
    uint64_t mem_fault_cause;
    /* Spill slots that survive a runtime-helper call (temps are caller-saved). */
    uint64_t mem_va;
    uint64_t mem_value;
    uint64_t mem_phys;
    /* Set when an instruction fetch walks into an unmapped/forbidden page. */
    uint64_t fetch_faulted;
    uint64_t fetch_fault_va;
    uint64_t fetch_fault_cause;

    /* Hart identifier exposed through mhartid. */
    uint64_t hartid;

    /* Optional VMM physical map; NULL means guest PA is host memory. */
    LoongemuMemMap *mem_map;

    /* Optional external interrupt controller (PLIC) and this hart's context. */
    LoongemuPlic *plic;
    uint64_t plic_context;
    /* WFI executed; the runtime may advance the clock until an IRQ arrives. */
    uint64_t wfi_pending;

    /* SFENCE.VMA request for the runtime's TB-cache shootdown. */
    uint64_t sfence_pending;
    uint64_t sfence_asid; /* UINT32_MAX means "all ASIDs" */

    /* Set by latr_translate() when the access must go through the MMIO bus. */
    uint64_t mem_mmio;
    LoongemuMmioBus *mmio;

    /* RVV state (VLEN=128, ELEN=64). vtype holds bits [9:0]; vill is separate
     * and is reported in bit 63 of the vtype CSR. */
    uint64_t vtype;
    uint64_t vill;
    uint64_t vl;
    uint64_t vstart;
    uint64_t vxrm;
    uint64_t vxsat;
    uint8_t vreg[32][LATR_RVV_VLENB] __attribute__((aligned(32)));
    /* Scratch for direct LSX/LASX lowering (up to LMUL=8 = 128 bytes). */
    uint8_t v_expanded_mask[LATR_RVV_VLENB * 8] __attribute__((aligned(32)));
    uint8_t v_inactive_fill[LATR_RVV_VLENB * 8] __attribute__((aligned(32)));
    uint8_t v_staging[LATR_RVV_VLENB * 8] __attribute__((aligned(32)));
    /* Scratch for helper calls and the packed active-lane mask words. */
    uint64_t rvv_args[8];
    uint64_t v_mask_words[(LATR_RVV_VLEN + 63) / 64];
    uint64_t v_mask_version;
    /* PVIR emission scratch and observations. */
    uint64_t rvv_scalar;
    uint64_t rvv_imm;
    uint64_t pvir_status;
    uint64_t pvir_count;
    uint64_t pvir_cache_hits;
    uint64_t pvir_direct_count;
    uint64_t pvir_lsx_chunks;
    uint64_t pvir_lasx_chunks;

    /* Software TLB (shared by data and instruction translations). */
    LatrTlbEntry tlb[LATR_TLB_ENTRIES];
    uint64_t tlb_hits;
    uint64_t tlb_flush_seq;  /* bumped by SFENCE.VMA/FENCE.I */
    uint64_t tlb_flush_seen; /* last sequence applied to the TLB */
};

static inline int env_offset_gpr(int reg)
{
    return (int)offsetof(CPUArchState, gpr) + reg * (int)sizeof(uint64_t);
}

static inline int env_offset_fpr(int reg)
{
    return (int)offsetof(CPUArchState, fpr) + reg * (int)sizeof(uint64_t);
}

static inline int env_offset_pc(void)
{
    return (int)offsetof(CPUArchState, pc);
}

static inline int env_offset_fcsr(void)
{
    return (int)offsetof(CPUArchState, fcsr);
}

static inline int env_offset_csr(int csr)
{
    return (int)offsetof(CPUArchState, csr) +
           (csr & (LATR_RISCV_CSR_COUNT - 1)) * (int)sizeof(uint64_t);
}

static inline int env_offset_exception(void)
{
    return (int)offsetof(CPUArchState, exception);
}

static inline int env_offset_badaddr(void)
{
    return (int)offsetof(CPUArchState, badaddr);
}
static inline int env_offset_privilege(void)
{
    return (int)offsetof(CPUArchState, privilege);
}

static inline int env_offset_reservation_addr(void)
{
    return (int)offsetof(CPUArchState, reservation_addr);
}

static inline int env_offset_reservation_size(void)
{
    return (int)offsetof(CPUArchState, reservation_size);
}

static inline int env_offset_reservation_epoch(void)
{
    return (int)offsetof(CPUArchState, reservation_epoch);
}

static inline int env_offset_reservation_valid(void)
{
    return (int)offsetof(CPUArchState, reservation_valid);
}

static inline int env_offset_host_return_address(void)
{
    return (int)offsetof(CPUArchState, host_return_address);
}

static inline int env_offset_vclock(void)
{
    return (int)offsetof(CPUArchState, vclock);
}

static inline int env_offset_fence_i_pending(void)
{
    return (int)offsetof(CPUArchState, fence_i_pending);
}

static inline int env_offset_stimecmp_update_pending(void)
{
    return (int)offsetof(CPUArchState, stimecmp_update_pending);
}

static inline int env_offset_mem_fault_cause(void)
{
    return (int)offsetof(CPUArchState, mem_fault_cause);
}

static inline int env_offset_mem_va(void)
{
    return (int)offsetof(CPUArchState, mem_va);
}

static inline int env_offset_mem_value(void)
{
    return (int)offsetof(CPUArchState, mem_value);
}

static inline int env_offset_mem_phys(void)
{
    return (int)offsetof(CPUArchState, mem_phys);
}

static inline int env_offset_fetch_faulted(void)
{
    return (int)offsetof(CPUArchState, fetch_faulted);
}

static inline int env_offset_fetch_fault_va(void)
{
    return (int)offsetof(CPUArchState, fetch_fault_va);
}

static inline int env_offset_mem_map(void)
{
    return (int)offsetof(CPUArchState, mem_map);
}

static inline int env_offset_plic(void)
{
    return (int)offsetof(CPUArchState, plic);
}

static inline int env_offset_plic_context(void)
{
    return (int)offsetof(CPUArchState, plic_context);
}

static inline int env_offset_wfi_pending(void)
{
    return (int)offsetof(CPUArchState, wfi_pending);
}

static inline int env_offset_sfence_pending(void)
{
    return (int)offsetof(CPUArchState, sfence_pending);
}

static inline int env_offset_tlb_flush_seq(void)
{
    return (int)offsetof(CPUArchState, tlb_flush_seq);
}

static inline int env_offset_sfence_asid(void)
{
    return (int)offsetof(CPUArchState, sfence_asid);
}

static inline int env_offset_mem_mmio(void)
{
    return (int)offsetof(CPUArchState, mem_mmio);
}

static inline int env_offset_mmio_bus(void)
{
    return (int)offsetof(CPUArchState, mmio);
}

static inline int env_offset_vtype(void)
{
    return (int)offsetof(CPUArchState, vtype);
}

static inline int env_offset_vill(void)
{
    return (int)offsetof(CPUArchState, vill);
}

static inline int env_offset_vl(void)
{
    return (int)offsetof(CPUArchState, vl);
}

static inline int env_offset_vstart(void)
{
    return (int)offsetof(CPUArchState, vstart);
}

static inline int env_offset_vxrm(void)
{
    return (int)offsetof(CPUArchState, vxrm);
}

static inline int env_offset_vxsat(void)
{
    return (int)offsetof(CPUArchState, vxsat);
}

static inline int env_offset_vreg(int reg)
{
    return (int)offsetof(CPUArchState, vreg) +
           reg * (int)sizeof(((CPUArchState *)0)->vreg[0]);
}

static inline int env_offset_v_expanded_mask(int byte_off)
{
    return (int)offsetof(CPUArchState, v_expanded_mask) + byte_off;
}

static inline int env_offset_v_inactive_fill(int byte_off)
{
    return (int)offsetof(CPUArchState, v_inactive_fill) + byte_off;
}

static inline int env_offset_v_staging(int byte_off)
{
    return (int)offsetof(CPUArchState, v_staging) + byte_off;
}

static inline int env_offset_pvir_cache_hits(void)
{
    return (int)offsetof(CPUArchState, pvir_cache_hits);
}

static inline int env_offset_pvir_direct_count(void)
{
    return (int)offsetof(CPUArchState, pvir_direct_count);
}

static inline int env_offset_pvir_lsx_chunks(void)
{
    return (int)offsetof(CPUArchState, pvir_lsx_chunks);
}

static inline int env_offset_pvir_lasx_chunks(void)
{
    return (int)offsetof(CPUArchState, pvir_lasx_chunks);
}

static inline int env_offset_rvv_arg(int index)
{
    return (int)offsetof(CPUArchState, rvv_args) +
           index * (int)sizeof(uint64_t);
}

static inline int env_offset_v_mask_words(void)
{
    return (int)offsetof(CPUArchState, v_mask_words);
}

static inline int env_offset_v_mask_version(void)
{
    return (int)offsetof(CPUArchState, v_mask_version);
}

static inline int env_offset_hartid(void)
{
    return (int)offsetof(CPUArchState, hartid);
}

#endif
