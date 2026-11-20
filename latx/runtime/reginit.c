/*
 * Tree-owned replacement for LATX's constructor-time register table setup.
 * The original lives in latx-config.c behind __attribute__((constructor)),
 * which never runs in a bare/UEFI image; without it every static IR2 operand
 * is register zero and generated code addresses memory at address 0.
 */
#include "qemu/osdep.h"
#include "ir2.h"
#include "reg-alloc.h"
#include "reg-map.h"

void latx_runtime_init(void)
{
    /*
     * Vector-extension policy, decided once at bring-up.
     *
     * AVX/AVX2 need 256-bit vector state, which this engine gets from LASX; the
     * option is bound to the actual hardware rather than left to a default, so
     * a translator built for a 3A5000-class host cannot emit xv* on a
     * 2K2000-class one.
     *
     * A host with no LSX cannot run x86-64 at all: SSE and SSE2 are part of the
     * baseline ABI and are implemented on LSX here.  Refusing at start is the
     * only honest answer -- exposing SSE2 on such a host means the first SSE
     * instruction faults in the guest with no explanation.
     */
    option_enable_lasx = latx_cpu_has_lasx();
    if (!latx_cpu_has_lsx()) {
        latx_platform_refuse(
            "this host has no LSX: x86-64 requires SSE/SSE2 (x86-64-v1)");
    }

    env_ir2_opnd = INIT_RA(IR2_OPND_GPR, reg_statics_map[S_ENV]);

    imm_zero_ir2_opnd = INIT_IMM(0);
    zero_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_zero);
    ra_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_ra);
    tp_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_tp);
    sp_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_sp);
    a0_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a0);
    a1_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a1);
    a2_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a2);
    a3_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a3);
    a4_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a4);
    a5_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a5);
    a6_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a6);
    a7_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_a7);
    t0_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t0);
    t1_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t1);
    t2_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t2);
    t3_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t3);
    t4_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t4);
    t5_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t5);
    t6_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t6);
    t7_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t7);
    t8_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_t8);
    r21_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_r21);
    fp_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_fp);
    s0_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s0);
    s1_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s1);
    s2_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s2);
    s3_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s3);
    s4_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s4);
    s5_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s5);
    s6_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s6);
    s7_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s7);
    s8_ir2_opnd = INIT_RA(IR2_OPND_GPR, la_s8);
    fcsr_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 0);
    fcsr1_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 1);
    fcsr2_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 2);
    fcsr3_ir2_opnd = INIT_RA(IR2_OPND_FCSR, 3);
    fcc0_ir2_opnd = INIT_RA(IR2_OPND_CC, 0);
    fcc1_ir2_opnd = INIT_RA(IR2_OPND_CC, 1);
    fcc2_ir2_opnd = INIT_RA(IR2_OPND_CC, 2);
    fcc3_ir2_opnd = INIT_RA(IR2_OPND_CC, 3);
    fcc4_ir2_opnd = INIT_RA(IR2_OPND_CC, 4);
    fcc5_ir2_opnd = INIT_RA(IR2_OPND_CC, 5);
    fcc6_ir2_opnd = INIT_RA(IR2_OPND_CC, 6);
    fcc7_ir2_opnd = INIT_RA(IR2_OPND_CC, 7);

    scr0_ir2_opnd = INIT_RA(IR2_OPND_SCR, 0);
    scr1_ir2_opnd = INIT_RA(IR2_OPND_SCR, 1);
    scr2_ir2_opnd = INIT_RA(IR2_OPND_SCR, 2);
    scr3_ir2_opnd = INIT_RA(IR2_OPND_SCR, 3);
}
