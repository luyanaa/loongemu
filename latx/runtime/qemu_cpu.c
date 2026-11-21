/*
 * Tree-owned replacements for the QEMU CPU/model support the copied helpers
 * call.  Scope is what the translator and a firmware bring-up actually need:
 * CPUID enumeration, CR/EFER mode tracking, MSR-level TSC, APIC accessors,
 * port I/O routing, and the exception entry points.
 *
 * The I/O and guest-memory paths go through weak platform hooks so a board
 * layer (UEFI or a test harness) can supply real backends.
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "exec/address-spaces.h"
#include "exec/cpu_ldst.h"
#include "cpu.h"
#include "latx-options.h"
#include "latx-smc.h"
#include "latx-x86-paging.h"

/* ---------------- platform hooks (weak defaults) ---------------- */
__attribute__((weak)) uint64_t latx_platform_io_read(uint32_t port, int size)
{
    (void)port;
    (void)size;
    return 0xffffffffull;
}

__attribute__((weak)) void latx_platform_io_write(uint32_t port, int size,
                                                 uint64_t value)
{
    (void)port;
    (void)size;
    (void)value;
}

__attribute__((weak)) uint64_t latx_platform_guest_read(uint64_t gpa, int size)
{
    (void)gpa;
    (void)size;
    return 0;
}

__attribute__((weak)) void latx_platform_guest_write(uint64_t gpa, int size,
                                                    uint64_t value)
{
    (void)gpa;
    (void)size;
    (void)value;
}

__attribute__((weak)) uint8_t latx_bare_ldub_code(uint64_t vaddr)
{
    (void)vaddr;
    return 0;
}

/* ---------------- port I/O address space ---------------- */
AddressSpace address_space_io;
AddressSpace address_space_memory;

uint8_t address_space_ldub(AddressSpace *as, uint64_t addr, MemTxAttrs attrs,
                           int *err)
{
    (void)as;
    (void)attrs;
    if (err) {
        *err = 0;
    }
    return (uint8_t)latx_platform_io_read((uint32_t)addr, 1);
}

uint16_t address_space_lduw(AddressSpace *as, uint64_t addr, MemTxAttrs attrs,
                            int *err)
{
    (void)as;
    (void)attrs;
    if (err) {
        *err = 0;
    }
    return (uint16_t)latx_platform_io_read((uint32_t)addr, 2);
}

uint32_t address_space_ldl(AddressSpace *as, uint64_t addr, MemTxAttrs attrs,
                           int *err)
{
    (void)as;
    (void)attrs;
    if (err) {
        *err = 0;
    }
    return (uint32_t)latx_platform_io_read((uint32_t)addr, 4);
}

void address_space_stb(AddressSpace *as, uint64_t addr, uint8_t v,
                       MemTxAttrs attrs, int *err)
{
    (void)as;
    (void)attrs;
    if (err) {
        *err = 0;
    }
    latx_platform_io_write((uint32_t)addr, 1, v);
}

void address_space_stw(AddressSpace *as, uint64_t addr, uint16_t v,
                       MemTxAttrs attrs, int *err)
{
    (void)as;
    (void)attrs;
    if (err) {
        *err = 0;
    }
    latx_platform_io_write((uint32_t)addr, 2, v);
}

void address_space_stl(AddressSpace *as, uint64_t addr, uint32_t v,
                       MemTxAttrs attrs, int *err)
{
    (void)as;
    (void)attrs;
    if (err) {
        *err = 0;
    }
    latx_platform_io_write((uint32_t)addr, 4, v);
}

/* ---------------- guest memory accessors used by helpers ---------------- */
uint8_t cpu_ldub_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra)
{
    (void)env;
    (void)ra;
    return (uint8_t)latx_platform_guest_read(addr, 1);
}

uint16_t cpu_lduw_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra)
{
    (void)env;
    (void)ra;
    return (uint16_t)latx_platform_guest_read(addr, 2);
}

uint32_t cpu_ldl_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra)
{
    (void)env;
    (void)ra;
    return (uint32_t)latx_platform_guest_read(addr, 4);
}

uint64_t cpu_ldq_data_ra(CPUArchState *env, uint64_t addr, uintptr_t ra)
{
    (void)env;
    (void)ra;
    return latx_platform_guest_read(addr, 8);
}

void cpu_stb_data_ra(CPUArchState *env, uint64_t addr, uint8_t v, uintptr_t ra)
{
    (void)env;
    (void)ra;
    latx_platform_guest_write(addr, 1, v);
}

void cpu_stw_data_ra(CPUArchState *env, uint64_t addr, uint16_t v, uintptr_t ra)
{
    (void)env;
    (void)ra;
    latx_platform_guest_write(addr, 2, v);
}

void cpu_stl_data_ra(CPUArchState *env, uint64_t addr, uint32_t v, uintptr_t ra)
{
    (void)env;
    (void)ra;
    latx_platform_guest_write(addr, 4, v);
}

void cpu_stq_data_ra(CPUArchState *env, uint64_t addr, uint64_t v, uintptr_t ra)
{
    (void)env;
    (void)ra;
    latx_platform_guest_write(addr, 8, v);
}

/* ---------------- CR / EFER mode tracking ---------------- */
void cpu_load_efer(CPUX86State *env, uint64_t val)
{
    env->efer = val;
    if ((env->cr[0] & CR0_PG_MASK) && (env->efer & MSR_EFER_LME)) {
        env->efer |= MSR_EFER_LMA;
        env->hflags |= HF_LMA_MASK;
    } else {
        env->efer &= ~MSR_EFER_LMA;
        env->hflags &= ~HF_LMA_MASK;
    }
}

void cpu_x86_update_cr0(CPUX86State *env, uint32_t new_cr0)
{
    env->cr[0] = new_cr0;
    env->hflags = (env->hflags & ~(HF_PE_MASK | HF_MP_MASK | HF_EM_MASK |
                                   HF_TS_MASK))
                  | ((new_cr0 & CR0_PE_MASK) ? HF_PE_MASK : 0);
    if ((new_cr0 & CR0_PG_MASK) && (env->efer & MSR_EFER_LME)) {
        env->efer |= MSR_EFER_LMA;
        env->hflags |= HF_LMA_MASK;
    } else {
        env->efer &= ~MSR_EFER_LMA;
        env->hflags &= ~HF_LMA_MASK;
    }
    tlb_flush(0);
}

void cpu_x86_update_cr3(CPUX86State *env, target_ulong new_cr3)
{
    int noflush = ((env->cr[4] & CR4_PCIDE_MASK) != 0) &&
                  ((new_cr3 & (UINT64_C(1) << 63)) != 0);

    env->cr[3] = new_cr3 & ~(UINT64_C(1) << 63);
    if (!noflush) {
        if (env->cr[4] & CR4_PGE_MASK) {
            latx_x86_tlb_flush_non_global();
            latx_platform_tlb_flush_all();
        } else {
            tlb_flush(0);
        }
    } else {
        latx_platform_tlb_flush_all();
    }
}

void cpu_x86_update_cr4(CPUX86State *env, uint32_t new_cr4)
{
    uint32_t old_cr4 = (uint32_t)env->cr[4];
    env->cr[4] = new_cr4;
    env->hflags = (env->hflags & ~(HF_SMAP_MASK | HF_OSFXSR_MASK))
                  | ((new_cr4 & CR4_SMAP_MASK) ? HF_SMAP_MASK : 0)
                  | ((new_cr4 & CR4_OSFXSR_MASK) ? HF_OSFXSR_MASK : 0);
    if ((old_cr4 ^ new_cr4) &
        (CR4_PGE_MASK | CR4_PSE_MASK | CR4_PAE_MASK | CR4_PCIDE_MASK |
         CR4_SMEP_MASK | CR4_SMAP_MASK | CR4_PKE_MASK)) {
        tlb_flush(0);
    }
}

void cpu_sync_avx_hflag(CPUX86State *env)
{
    (void)env;
}

void cpu_sync_bndcs_hflags(CPUX86State *env)
{
    (void)env;
}

/* Reset vCPU into 16-bit x86 Real Mode (CS=0xF000, CS.base=0xFFFF0000, IP=0xFFF0). */
void latx_x86_reset_real_mode(CPUX86State *env)
{
    env->cr[0] = 0x60000010u; /* CD | NW | ET, PE=0, PG=0 */
    env->cr[3] = 0;
    env->cr[4] = 0;
    env->efer = 0;
    env->hflags = 0;
    env->sys.codemode = 2; /* LATX_DT_X16 */
    for (int i = 0; i < 6; i++) {
        env->segs[i].selector = 0;
        env->segs[i].base = 0;
        env->segs[i].limit = 0xffffu;
        env->segs[i].flags = 0;
    }
    env->segs[R_CS].selector = 0xf000u;
    env->segs[R_CS].base = 0xffff0000ULL;
    env->segs[R_CS].limit = 0xffffu;
    env->eip = 0xfff0ULL;
    env->eflags = 0x2u;
    env->xcr0 = 1ULL;
}

/* Far jump CS/mode transition helper (Real Mode / 32-bit PM / 64-bit LM). */
void helper_ljmp_far(CPUX86State *env, uint32_t selector, uint64_t offset)
{
    uint16_t sel = (uint16_t)(selector & 0xffffu);
    env->segs[R_CS].selector = sel;

    if ((env->cr[0] & CR0_PE_MASK) == 0) {
        env->segs[R_CS].base = ((uint64_t)sel) << 4;
        env->segs[R_CS].limit = 0xffffu;
        env->sys.codemode = 2; /* LATX_DT_X16 */
        env->hflags &= ~(HF_CS32_MASK | HF_CS64_MASK | HF_LMA_MASK);
        env->eip = offset & 0xffffULL;
        return;
    }

    uint64_t dt_base = (sel & 4u) ? env->ldt.base : env->gdt.base;
    uint32_t idx = sel & ~7u;
    uint64_t desc = 0;
    if (dt_base != 0 && idx != 0) {
        desc = latx_platform_guest_read(dt_base + (uint64_t)idx, 8);
    }
    uint64_t base = ((desc >> 16) & 0xffffffULL) | (((desc >> 56) & 0xffULL) << 24);
    int l_bit = (int)((desc >> 53) & 1u);
    int d_bit = (int)((desc >> 54) & 1u);

    env->segs[R_CS].base = base;
    env->segs[R_CS].limit = 0xffffffffu;
    if ((env->efer & MSR_EFER_LMA) && l_bit) {
        env->sys.codemode = 1; /* LATX_DT_X64 */
        env->hflags = (env->hflags & ~HF_CS32_MASK) | HF_CS64_MASK | HF_LMA_MASK;
    } else if (d_bit || desc == 0) {
        env->sys.codemode = 0; /* LATX_DT_X86 (32-bit) */
        env->hflags = (env->hflags & ~HF_CS64_MASK) | HF_CS32_MASK;
    } else {
        env->sys.codemode = 2; /* LATX_DT_X16 (16-bit PM) */
        env->hflags &= ~(HF_CS64_MASK | HF_CS32_MASK);
    }
    env->eip = offset;
}

/*
 * The feature words the MSR helpers consult.  They are derived from CPUID
 * rather than hand-written so the two can never disagree: a wrmsr of EFER.LME
 * is accepted exactly when CPUID.8000_0001:EDX advertises long mode.
 */
void latx_x86_env_init_features(CPUX86State *env)
{
    uint32_t eax, ebx, ecx, edx;

    memset(env->features, 0, sizeof(env->features));
    env->xcr0 = XSTATE_FP_MASK;

    cpu_x86_cpuid(env, 0x1, 0, &eax, &ebx, &ecx, &edx);
    env->features[FEAT_1_EDX] = edx;
    env->features[FEAT_1_ECX] = ecx;

    cpu_x86_cpuid(env, 0x7, 0, &eax, &ebx, &ecx, &edx);
    env->features[FEAT_7_0_EBX] = ebx;
    env->features[FEAT_7_0_ECX] = ecx;
    env->features[FEAT_7_0_EDX] = edx;

    cpu_x86_cpuid(env, 0x80000001u, 0, &eax, &ebx, &ecx, &edx);
    env->features[FEAT_8000_0001_EDX] = edx;
    env->features[FEAT_8000_0001_ECX] = ecx;

    cpu_x86_cpuid(env, 0x80000008u, 0, &eax, &ebx, &ecx, &edx);
    env->features[FEAT_8000_0008_EBX] = ebx;
}

/* ---------------- model registers ---------------- */
/*
 * The timestamp counter is the hardware's, not this file's.
 *
 * RDTSC translates to rdtime.d, so a guest that reads the counter and a
 * helper that reads it through here must see the same number; `env->tsc++`
 * was a second clock running at one tick per call, which made the two
 * disagree the moment anything actually measured elapsed time.  The
 * environment field stays as the guest-visible shadow for state save and
 * restore, updated from the same source.
 */
uint64_t cpu_get_tsc(CPUX86State *env)
{
    uint64_t now;

    __asm__ volatile("rdtime.d %0, $zero" : "=r"(now) :: "memory");
    env->tsc = now;
    return now;
}

void cpu_set_fpuc(CPUX86State *env, uint16_t fpuc)
{
    env->fpuc = fpuc;
}

void cpu_set_mxcsr(CPUX86State *env, uint32_t val)
{
    env->mxcsr = val;
}

uint32_t latx_guest_nr_cpus = 1;
uint32_t latx_guest_apic_id = 0;
static uint8_t apic_tpr_per_cpu[64];
static uint64_t apic_base_per_cpu[64];

__attribute__((weak)) uint32_t latx_platform_apic_id(void)
{
    return latx_guest_apic_id;
}

__attribute__((weak)) int cpu_get_apic_tpr(CPUX86State *env)
{
    (void)env;
    return apic_tpr_per_cpu[latx_platform_apic_id() & 63u];
}

__attribute__((weak)) void cpu_set_apic_tpr(CPUX86State *env, uint8_t val)
{
    (void)env;
    apic_tpr_per_cpu[latx_platform_apic_id() & 63u] = val;
}

uint64_t cpu_get_apic_base(CPUX86State *env)
{
    uint32_t id = latx_platform_apic_id() & 63u;
    (void)env;
    if (apic_base_per_cpu[id] == 0) {
        apic_base_per_cpu[id] = 0xfee00800ULL | (id == 0 ? 0x100ULL : 0ULL);
    }
    return apic_base_per_cpu[id];
}

void cpu_set_apic_base(CPUX86State *env, uint64_t val)
{
    (void)env;
    apic_base_per_cpu[latx_platform_apic_id() & 63u] = val;
}

/* ---------------- SVM / TLB / randomness ---------------- */
void cpu_svm_check_intercept_param(CPUX86State *env, uint32_t type,
                                   uint64_t param, uintptr_t retaddr)
{
    (void)env;
    (void)type;
    (void)param;
    (void)retaddr;
}

/*
 * The QEMU helpers express their invalidation through these two functions --
 * helper_invlpg() calls tlb_flush_page(), a TLB-wide flush calls tlb_flush().
 * Routing them into the guest translation cache is what makes INVLPG visible
 * here: a guest that edits a PTE and invalidates gets the new answer on the
 * next walk, and one that edits without invalidating keeps the old one, which
 * is exactly the architecture's contract.
 */
__attribute__((weak)) void latx_platform_tlb_flush_all(void)
{
}

__attribute__((weak)) void latx_platform_tlb_flush_page(uint64_t addr)
{
    (void)addr;
}

void tlb_flush(CPUState *cpu)
{
    (void)cpu;
    latx_x86_tlb_flush_all();
    latx_platform_tlb_flush_all();
}

void tlb_flush_page(CPUState *cpu, uint64_t addr)
{
    (void)cpu;
    latx_x86_tlb_flush_page(addr);
    latx_platform_tlb_flush_page(addr);
}

uint64_t latx_host_timer_hz = UINT64_C(100000000);
/* 0 = KVM ("KVMKVMKVM\0\0\0"), 1 = Hyper-V ("Microsoft Hv"), 2 = LoongEMU */
uint32_t latx_hv_vendor = 0u;
/* 0 = Skylake (max leaf 0x16) on LASX, 1 = Ivy Bridge (max leaf 0x0D) on LASX */
uint32_t latx_cpuid_lasx_ivybridge = 0u;

/*
 * Cryptographic entropy policy (P0-A): do NOT advertise RDRAND/RDSEED or fake
 * CF=1 using host rdtime.d + deterministic mixing. Platforms with a verified
 * host TRNG/CSPRNG (e.g. EFI_RNG_PROTOCOL / hardware TRNG) must override
 * latx_platform_get_entropy() and explicitly opt in via these trust flags.
 */
uint32_t latx_entropy_trusted = 0u;
uint32_t latx_rdseed_trusted = 0u;

__attribute__((weak)) int latx_platform_get_entropy(uint64_t *out, int is_rdseed)
{
    (void)is_rdseed;
    if (out != NULL) {
        *out = 0;
    }
    return 0;
}

int latx_random_u64_checked(uint64_t *out, int is_rdseed)
{
    uint32_t trusted = is_rdseed ? latx_rdseed_trusted : latx_entropy_trusted;
    if (out != NULL) {
        *out = 0;
    }
    if (!trusted || out == NULL) {
        return 0;
    }
    if (!latx_platform_get_entropy(out, is_rdseed)) {
        *out = 0;
        return 0;
    }
    return 1;
}

__attribute__((weak)) void helper_hypercall(CPUX86State *env)
{
    if (env != NULL) {
        env->regs[R_EAX] = (uint64_t)(int64_t)-1000; /* -KVM_ENOSYS */
    }
}

__attribute__((weak)) void latx_platform_kvm_wrmsr(CPUX86State *env,
                                                   uint32_t msr, uint64_t val)
{
    (void)env;
    (void)msr;
    (void)val;
}

__attribute__((weak)) int latx_platform_kvm_rdmsr(CPUX86State *env,
                                                  uint32_t msr, uint64_t *val)
{
    (void)env;
    (void)msr;
    (void)val;
    return 0;
}

int qemu_guest_getrandom(void *buf, size_t len, void **errp)
{
    uint8_t *out = (uint8_t *)buf;
    size_t i = 0;
    (void)errp;
    while (i < len) {
        uint64_t r = 0;
        if (!latx_random_u64_checked(&r, 0)) {
            return -1;
        }
        for (int b = 0; b < 8 && i < len; b++, i++) {
            out[i] = (uint8_t)(r >> (b * 8));
        }
    }
    return 0;
}

uint8_t options_to_save(void)
{
    return 0;
}

/* ---------------- exception entry ---------------- */
void raise_exception_err_ra(CPUX86State *env, int exception_index,
                            int error_code, uintptr_t retaddr)
{
    (void)retaddr;
    env->exception_index = exception_index;
    env->error_code = error_code;
    cpu_loop_exit((CPUState *)(void *)env);
}

void raise_exception_err(CPUX86State *env, int exception_index, int error_code)
{
    raise_exception_err_ra(env, exception_index, error_code, 0);
}

void raise_exception_ra(CPUX86State *env, int exception_index, uintptr_t retaddr)
{
    raise_exception_err_ra(env, exception_index, 0, retaddr);
}

void raise_interrupt(CPUX86State *env, int intno, int is_int, int error_code,
                     int next_eip_addend)
{
    (void)next_eip_addend;
    env->exception_index = intno;
    env->error_code = error_code;
    env->exception_is_int = is_int;
    cpu_loop_exit((CPUState *)(void *)env);
}

/* ---------------- CPUID ---------------- */
static void cpuid_vendor(uint32_t *ebx, uint32_t *edx, uint32_t *ecx)
{
    *ebx = 0x756e6547; /* Genu */
    *edx = 0x49656e69; /* ineI */
    *ecx = 0x6c65746e; /* ntel */
}

void cpu_x86_cpuid(CPUX86State *env, uint32_t index, uint32_t count,
                   uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
                   uint32_t *edx)
{
    *eax = *ebx = *ecx = *edx = 0;

    uint32_t apic_id = latx_platform_apic_id();
    uint32_t nr_cpus = latx_guest_nr_cpus ? latx_guest_nr_cpus : 1u;
    uint64_t hz = latx_host_timer_hz ? latx_host_timer_hz : UINT64_C(100000000);
    uint32_t tsc_khz = (uint32_t)(hz / 1000ULL);
    int has_lasx = latx_cpu_has_lasx();

    switch (index) {
    case 0x0:
        if (!has_lasx) {
            *eax = 0x08u; /* Nehalem-based max basic leaf on LSX-only path */
        } else {
            *eax = latx_cpuid_lasx_ivybridge ? 0x0du : 0x16u; /* Ivy Bridge / Skylake */
        }
        cpuid_vendor(ebx, edx, ecx);
        break;
    case 0x1: {
        uint32_t f_edx = (1u << 0)  | (1u << 4)  | (1u << 5)  | (1u << 6)
                       | (1u << 8)  | (1u << 9)  | (1u << 11) | (1u << 12)
                       | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16)
                       | (1u << 17) | (1u << 19) | (1u << 23) | (1u << 24)
                       | (1u << 25) | (1u << 26)
                       | (nr_cpus > 1u ? (1u << 28) : 0u); /* HTT */
        uint32_t f_ecx = (latx_entropy_trusted ? (1u << 30) : 0u) /* RDRAND */
                       | (1u << 31); /* HYPERVISOR */

        if (latx_cpu_has_lsx()) {
            f_ecx |= (1u << 0)    /* SSE3        */
                   | (1u << 1)    /* PCLMULQDQ   */
                   | (1u << 9)    /* SSSE3       */
                   | (1u << 13)   /* CMPXCHG16B  */
                   | (1u << 19)   /* SSE4.1      */
                   | (1u << 20)   /* SSE4.2      */
                   | (1u << 23);  /* POPCNT      */
        }

        if (has_lasx) {
            f_ecx |= (1u << 12)   /* FMA      */
                   | (1u << 22)   /* MOVBE    */
                   | (1u << 26)   /* XSAVE    */
                   | (1u << 28)   /* AVX      */
                   | (1u << 29);  /* F16C     */

            if (env != NULL && (env->cr[4] & CR4_OSXSAVE_MASK)) {
                f_ecx |= (1u << 27);  /* OSXSAVE */
            }
        }
        /* Family 6h: Nehalem (0x106a5), Ivy Bridge (0x306a9), Skylake (0x506e3) */
        if (!has_lasx) {
            *eax = 0x000106a5u;
        } else {
            *eax = latx_cpuid_lasx_ivybridge ? 0x000306a9u : 0x000506e3u;
        }
        *ebx = ((apic_id & 0xffu) << 24) | ((nr_cpus & 0xffu) << 16) | (8u << 8);
        *ecx = f_ecx;
        *edx = f_edx;
        break;
    }
    case 0x2:
        /* Compact TLB/cache descriptors (0xff defers cache sizing to leaf 0x04). */
        *eax = 0x76036301u;
        *ebx = 0x00f0b5ffu;
        *ecx = 0x00000000u;
        *edx = 0x00c30000u;
        break;
    case 0x4: {
        uint32_t max_cores = (nr_cpus > 1u) ? (nr_cpus - 1u) : 0u;
        if (count == 0) { /* L1d: 32 KiB, 8-way, 64B line, 64 sets */
            *eax = (max_cores << 26) | (0u << 14) | (1u << 8) | (1u << 5) | 1u;
            *ebx = (7u << 22) | 63u;
            *ecx = 63u;
            *edx = 0;
        } else if (count == 1) { /* L1i: 32 KiB, 8-way, 64B line, 64 sets */
            *eax = (max_cores << 26) | (0u << 14) | (1u << 8) | (1u << 5) | 2u;
            *ebx = (7u << 22) | 63u;
            *ecx = 63u;
            *edx = 0;
        } else if (count == 2) { /* L2 unified: 256 KiB, 8-way, 512 sets */
            *eax = (max_cores << 26) | (0u << 14) | (1u << 8) | (2u << 5) | 3u;
            *ebx = (7u << 22) | 63u;
            *ecx = 511u;
            *edx = 0;
        } else if (count == 3) { /* L3 unified: 4 MiB, 16-way, shared */
            *eax = (max_cores << 26) | (max_cores << 14) | (1u << 8) | (3u << 5) | 3u;
            *ebx = (15u << 22) | 63u;
            *ecx = 4095u;
            *edx = 2u; /* inclusive */
        }
        break;
    }
    case 0xb: {
        uint32_t core_shift = 0;
        while ((1u << core_shift) < nr_cpus) {
            core_shift++;
        }
        if (count == 0) { /* SMT level: 1 thread/core */
            *eax = 0;
            *ebx = 1;
            *ecx = (1u << 8) | 0u;
            *edx = apic_id;
        } else if (count == 1) { /* Core level: nr_cpus cores/pkg */
            *eax = core_shift;
            *ebx = nr_cpus;
            *ecx = (2u << 8) | 1u;
            *edx = apic_id;
        } else {
            *eax = 0;
            *ebx = 0;
            *ecx = count;
            *edx = apic_id;
        }
        break;
    }
    case 0xd:
        if (count == 0) {
            if (has_lasx) {
                *eax = 0x7;      /* x87 | SSE | AVX */
                *ebx = (env != NULL && (env->xcr0 & XSTATE_YMM_MASK))
                           ? 0x340u : 0x240u;
                *ecx = 0x340;
                *edx = 0;
            }
        } else if (count == 1) {
            if (has_lasx) {
                *eax = 1;        /* XSAVEOPT */
                *ebx = 0x340;
                *ecx = 0;
                *edx = 0;
            }
        } else if (count == 2) {
            if (has_lasx) {
                *eax = 0x100;    /* YMM_Hi128 size = 256 bytes */
                *ebx = 0x240;    /* YMM_Hi128 offset = 576 bytes */
                *ecx = 0;
                *edx = 0;
            }
        }
        break;
    case 0x15:
        /* Dynamic TSC / Core Crystal Clock matching host rdtime.d (hz). */
        *eax = 1;
        *ebx = 4;
        *ecx = (uint32_t)(hz / 4ULL);
        *edx = 0;
        break;
    case 0x16: {
        uint32_t base_mhz = (uint32_t)(hz / 1000000ULL);
        if (base_mhz < 100u) {
            base_mhz = 100u;
        }
        *eax = base_mhz;
        *ebx = base_mhz;
        *ecx = 100u;
        *edx = 0;
        break;
    }
    case 0x6:
    case 0xa:
        break;
    case 0x7:
        if (count == 0) {
            *ebx = latx_rdseed_trusted ? (1u << 18) : 0u; /* RDSEED */
            if (has_lasx) {
                *ebx |= (1u << 3)    /* BMI1  */
                      | (1u << 5)    /* AVX2  */
                      | (1u << 8);   /* BMI2  */
            }
        }
        break;
    case 0x40000000u:
        if (latx_hv_vendor == 1u) {
            /* "Microsoft Hv" (Hyper-V Enlightenment option for Windows) */
            *eax = 0x40000010u;
            *ebx = 0x7263694du; /* "Micr" */
            *ecx = 0x666f736fu; /* "osof" */
            *edx = 0x76482074u; /* "t Hv" */
        } else if (latx_hv_vendor == 2u) {
            /* "LoongEMUVMM\0" */
            *eax = 0x40000010u;
            *ebx = 0x6e6f6f4cu; /* "Loon" */
            *ecx = 0x554d4567u; /* "gEMU" */
            *edx = 0x004d4d56u; /* "VMM\0" */
        } else {
            /* Default Linux PV: "KVMKVMKVM\0\0\0" */
            *eax = 0x40000010u;
            *ebx = 0x4b4d564bu; /* "KVMK" */
            *ecx = 0x564b4d56u; /* "VMKV" */
            *edx = 0x0000004du; /* "M\0\0\0" */
        }
        break;
    case 0x40000001u:
        if (latx_hv_vendor == 1u) {
            *eax = 0x31237648u; /* "Hv#1" */
        } else if (latx_hv_vendor == 0u) {
            /*
             * KVM PV: all advertised features are wired end-to-end:
             * NOP_IO_DELAY (1) | CLOCKSOURCE2 (3) | PV_EOI (6) |
             * PV_UNHALT (7) | PV_SEND_IPI (11) | CLOCKSOURCE_STABLE_BIT (24).
             */
            *eax = (1u << 1) | (1u << 3) | (1u << 6) | (1u << 7) |
                   (1u << 11) | (1u << 24);
        }
        break;
    case 0x40000002u:
        if (latx_hv_vendor == 1u) {
            *eax = 0x00004a61u;
            *ebx = 0x000a0000u;
        }
        break;
    case 0x40000003u:
        if (latx_hv_vendor == 1u) {
            /*
             * Hyper-V implemented privileges/features:
             * Time Ref Count (1) | APIC Access MSRs + Assist (4) |
             * Hypercall MSR + Dispatcher (5) | VP Index (6).
             */
            *eax = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6);
            *edx = (1u << 4); /* XMM fast hypercall input */
        }
        break;
    case 0x40000004u:
        if (latx_hv_vendor == 1u) {
            /*
             * Hyper-V implementation recommendations (each backed by live code):
             * Local/remote TLB flush, APIC MSRs, relaxed timing,
             * SyntheticClusterIpi and extended processor masks.
             */
            *eax = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) |
                   (1u << 10) | (1u << 11);
            *ebx = 0x00000fffu; /* Recommended spinlock retry limit */
        }
        break;
    case 0x40000005u:
        if (latx_hv_vendor == 1u) {
            *eax = nr_cpus;
            *ebx = nr_cpus;
        }
        break;
    case 0x40000010u:
        *eax = tsc_khz;
        *ebx = tsc_khz;
        *ecx = 0;
        *edx = 0;
        break;
    default:
        break;
    }

    if (index == 0x80000000u) {
        *eax = 0x80000008u;
        cpuid_vendor(ebx, edx, ecx);
    } else if (index == 0x80000001u) {
        *ecx = (1u << 0);            /* LAHF/SAHF */
        if (has_lasx) {
            *ecx |= (1u << 5);       /* ABM (LZCNT) -- x86-64-v3 */
        }
        *edx = (1u << 11)            /* SYSCALL/SYSRET */
             | (1u << 20)            /* NX */
             | (1u << 26)            /* 1 GiB pages */
             | (1u << 29);           /* long mode */
    } else if (index == 0x80000008u) {
        *eax = (48u << 8) | 40u;
    } else if (index >= 0x80000002u && index <= 0x80000004u) {
        static const char brand[49] =
            "LoongEMU x86_64 Virtual CPU";
        const char *src = brand + (index - 0x80000002u) * 16u;

        *eax = (uint32_t)src[0] | ((uint32_t)src[1] << 8)
             | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        *ebx = (uint32_t)src[4] | ((uint32_t)src[5] << 8)
             | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
        *ecx = (uint32_t)src[8] | ((uint32_t)src[9] << 8)
             | ((uint32_t)src[10] << 16) | ((uint32_t)src[11] << 24);
        *edx = (uint32_t)src[12] | ((uint32_t)src[13] << 8)
             | ((uint32_t)src[14] << 16) | ((uint32_t)src[15] << 24);
    }
}

/* Backing store for the flat TCG instruction metadata array. */
uint64_t latx_tcg_gen_insn_data[TCG_MAX_INSNS * 2];
