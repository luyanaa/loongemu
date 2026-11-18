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
    env->cr[3] = new_cr3;
    /*
     * A new address space invalidates every cached guest translation.  The
     * TB cache is keyed on cr3 separately, but a walk result is not.
     */
    tlb_flush(0);
}

void cpu_x86_update_cr4(CPUX86State *env, uint32_t new_cr4)
{
    env->cr[4] = new_cr4;
    env->hflags = (env->hflags & ~(HF_SMAP_MASK | HF_OSFXSR_MASK))
                  | ((new_cr4 & CR4_SMAP_MASK) ? HF_SMAP_MASK : 0)
                  | ((new_cr4 & CR4_OSFXSR_MASK) ? HF_OSFXSR_MASK : 0);
}

void cpu_sync_avx_hflag(CPUX86State *env)
{
    (void)env;
}

void cpu_sync_bndcs_hflags(CPUX86State *env)
{
    (void)env;
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
uint64_t cpu_get_tsc(CPUX86State *env)
{
    return env->tsc++;
}

void cpu_set_fpuc(CPUX86State *env, uint16_t fpuc)
{
    env->fpuc = fpuc;
}

void cpu_set_mxcsr(CPUX86State *env, uint32_t val)
{
    env->mxcsr = val;
}

static uint8_t apic_tpr;
static uint64_t apic_base;

int cpu_get_apic_tpr(CPUX86State *env)
{
    (void)env;
    return apic_tpr;
}

void cpu_set_apic_tpr(CPUX86State *env, uint8_t val)
{
    (void)env;
    apic_tpr = val;
}

uint64_t cpu_get_apic_base(CPUX86State *env)
{
    (void)env;
    return apic_base;
}

void cpu_set_apic_base(CPUX86State *env, uint64_t val)
{
    (void)env;
    apic_base = val;
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

int qemu_guest_getrandom(void *buf, size_t len, void **errp)
{
    (void)buf;
    (void)len;
    (void)errp;
    return -1; /* RDRAND reports failure instead of inventing entropy */
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
    (void)env;
    (void)count;
    *eax = *ebx = *ecx = *edx = 0;

    switch (index) {
    case 0x0:
        *eax = 7;
        cpuid_vendor(ebx, edx, ecx);
        break;
    case 0x1: {
        /* x86-64-v2 baseline: SSE4.2/POPCNT are always safe. */
        uint32_t f_edx = (1u << 0)  | (1u << 4)  | (1u << 5)  | (1u << 6)
                       | (1u << 8)  | (1u << 9)  | (1u << 11) | (1u << 12)
                       | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16)
                       | (1u << 17) | (1u << 19) | (1u << 23) | (1u << 24)
                       | (1u << 25) | (1u << 26);
        uint32_t f_ecx = (1u << 0) | (1u << 1) | (1u << 9) | (1u << 19)
                       | (1u << 20) | (1u << 23);

        /* AVX/AVX2 need 256-bit vector state, i.e. LASX on the host. */
        if (latx_cpu_has_lasx()) {
            f_ecx |= (1u << 12)   /* FMA    */
                   | (1u << 26)   /* XSAVE  */
                   | (1u << 27)   /* OSXSAVE*/
                   | (1u << 28)   /* AVX    */
                   | (1u << 29);  /* F16C   */
        }
        *eax = 0x000306f2u;
        *ebx = 0x00000800u;
        *ecx = f_ecx;
        *edx = f_edx;
        break;
    }
    case 0x4:
    case 0xb:
        break; /* no cache/APIC topology enumeration */
    case 0xd:
        if (count == 0) {
            if (latx_cpu_has_lasx()) {
                *eax = 0x7;      /* x87 | SSE | AVX */
                *ebx = 0x340;    /* XSAVE area size for XCR0 = 7 */
                *ecx = 0;        /* XSS unsupported */
                *edx = 0;
            }
        } else if (count == 1) {
            if (latx_cpu_has_lasx()) {
                *eax = 1;        /* XSAVEOPT */
                *ebx = 0x240;
            }
        } else if (count == 2) {
            if (latx_cpu_has_lasx()) {
                *eax = 0;        /* YMM state is 256-bit */
                *ebx = 0x340;
            }
        }
        break;
    case 0x6:
    case 0xa:
        break;
    case 0x7:
        if (count == 0 && latx_cpu_has_lasx()) {
            /* AVX2/BMI without any AVX-512 state. */
            *ebx = (1u << 3)    /* BMI1  */
                 | (1u << 5)    /* AVX2  */
                 | (1u << 8);   /* BMI2  */
        }
        break;
    default:
        break;
    }

    if (index == 0x80000000u) {
        *eax = 0x80000008u;
        cpuid_vendor(ebx, edx, ecx);
    } else if (index == 0x80000001u) {
        *ecx = (1u << 0);            /* LAHF/SAHF */
        *edx = (1u << 11)            /* SYSCALL/SYSRET */
             | (1u << 20)            /* NX */
             | (1u << 26)            /* 1 GiB pages */
             | (1u << 29);           /* long mode */
    } else if (index == 0x80000008u) {
        *eax = ((40u - 1u) << 8) | (48u - 1u);
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
