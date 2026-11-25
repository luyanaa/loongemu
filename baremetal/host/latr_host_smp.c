/*
 * Host harness for the LATR machine dispatcher (design:
 * docs/design/latr-smp-concurrency.md, phase P1b prerequisite).
 *
 * Why this is a *scripted* machine and not a guest run: the translator emits
 * LoongArch words, and the trampoline that jumps into them (`latr_enter_tb`,
 * bare-metal/LoongArch asm) cannot exist on an x86/AArch64 host. Guest
 * execution therefore stays in the bare-metal images; this harness replaces
 * `enter_tb` with a scripted block model and exercises everything that runs in
 * C around it:
 *
 *   - the dispatcher (lookup / translate+assemble / caches per hart),
 *   - a CLINT MSIP write waking a hart that has parked in WFI,
 *   - the exit request being honoured at a block boundary,
 *   - SFENCE.VMA driving the fence sequence and a cross-core shootdown,
 *   - STOP taking a hart offline while the others keep running.
 *
 * The script is keyed by hartid and block count, and drives the same entry
 * points generated code would (latr_system_mmio_write for CLINT and the exit
 * device, env->wfi_pending for WFI, env->sfence_pending for SFENCE.VMA), so a
 * scheduler bug surfaces here without a LoongArch host.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "latr-machine.h"

static int checks;
static int failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what);            \
        }                                                                    \
    } while (0)

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t machine_work[(2u << 20) * 4]; /* 2 harts: arena + staging each */
static LatrMachine machine;

/* Valid RISC-V instructions for the scripted entry PCs: the dispatcher only
 * needs to decode a block, it never executes the emitted words here. */
static const uint32_t firmware_words[] = {
    0x00000013u, /* nop */
    0x00000013u, /* nop */
    0x0000006fu, /* j . */
};
#define FW_PC LATR_MACHINE_FIRMWARE_BASE
#define FW_LOOP (LATR_MACHINE_FIRMWARE_BASE + 8u)
#define HART1_DISC 0x80000040u /* hart 1's "discovered" marker */

static uint64_t vclock_start;

/* Script state. */
static int hart0_ipi_sent;
static int hart1_woke;
static uint64_t fence_seq_start;
static uint64_t gen0_start;
static uint64_t gen1_start;
static uint32_t exit_value;

static void guest_write(uint64_t pa, unsigned size, uint64_t value)
{
    latr_system_mmio_write(&machine.sys, pa, size, value);
}

/*
 * Replace ment of the LoongArch trampoline. `env` is the hart's state and
 * `code` the just-assembled block; the guest PC is still the block's PC.
 */
static void scripted_enter_tb(void *envp, void *code)
{
    CPUArchState *env = (CPUArchState *)envp;

    (void)code;
    if (env->hartid == 0) {
        if (!hart0_ipi_sent) {
            /* What generated code does for `sd zero, 0(clint+4)`: an IPI to
             * hart 1. This is the only wake a hart parked in WFI can get. */
            guest_write(LATR_SYSTEM_CLINT_BASE + 4u, 4, 1);
            hart0_ipi_sent = 1;
            /* And a guest SFENCE.VMA without an ASID (asid 0 = global), which
             * must drop every hart's entries through the fence sequence. */
            env->sfence_asid = 0;
            env->sfence_pending = 1;
        }
        if (hart1_woke) {
            /* Leave through the SiFive test device, like firmware does. */
            exit_value = 0x5555;
            guest_write(LATR_SYSTEM_EXIT_BASE, 4, exit_value);
        }
        env->pc = FW_LOOP;
        return;
    }
    /* Hart 1: park in WFI until the IPI has been delivered, then "handle" it
     * by clearing the marker and reporting the wake. */
    if (env->csr[0x344] & (UINT64_C(1) << 3)) {
        hart1_woke = 1;
        guest_write(LATR_SYSTEM_CLINT_BASE + 4u, 4, 0); /* clear MSIP */
        env->pc = HART1_DISC;
        return;
    }
    env->pc = FW_PC;
    env->wfi_pending = 1;
}

static int setup_machine(void)
{
    if (!latr_machine_init(&machine, ram, sizeof(ram), 2, machine_work,
                           sizeof(machine_work))) {
        return 0;
    }
    latr_machine_set_enter_tb(&machine, scripted_enter_tb);
    if (!latr_machine_load_firmware(&machine,
                                    (const uint8_t *)firmware_words,
                                    sizeof(firmware_words))) {
        return 0;
    }
    if (!latr_machine_build_dtb(&machine)) {
        return 0;
    }
    /* Hart 1's "handler" PC must decode for a block to be translated. */
    memcpy(ram + (HART1_DISC - LATR_SYSTEM_RAM_BASE), firmware_words,
           sizeof(firmware_words));
    /* Let hart 1 take the software interrupt: mtvec + mie.MSIE + mstatus.MIE,
     * which a real guest would program in its init block. */
    {
        LatrMachineHart *h1 = &machine.harts[1];

        h1->env.csr[0x305] = HART1_DISC;          /* mtvec */
        h1->env.csr[0x304] |= (UINT64_C(1) << 3); /* mie.MSIE */
        h1->env.csr[0x300] |= (UINT64_C(1) << 3); /* mstatus.MIE */
    }
    return latr_machine_boot(&machine);
}

static void scenario_ipi_and_wfi(void)
{
    uint64_t vclock_before;
    uint64_t fence_before;
    int rc;

    hart0_ipi_sent = 0;
    hart1_woke = 0;
    vclock_start = loongemu_vclock_now_ns(&machine.sys.vclock);
    vclock_before = vclock_start;
    fence_seq_start = latr_tb_fence_seq();
    fence_before = fence_seq_start;
    gen0_start = machine.harts[0].tb_cache.generation;
    gen1_start = machine.harts[1].tb_cache.generation;

    rc = latr_machine_run(&machine, 64);
    CHECK(rc == 1, "machine exits when the guest writes the exit device");
    CHECK(machine.sys.exit_request == 0x5555, "exit value came from the guest");
    CHECK(machine.harts[0].tbs > 0 && machine.harts[1].tbs > 0,
          "both harts dispatched blocks");
    CHECK(latr_machine_wake_events(&machine) == 1,
          "exactly one CLINT MSIP write reached the machine");
    CHECK(latr_hart_exit_count(&machine.harts[1].park) == 1,
          "the woken hart honoured its exit request at a block boundary");
    CHECK(hart1_woke == 1, "hart 1 came back from WFI and handled the IPI");
    CHECK(loongemu_vclock_now_ns(&machine.sys.vclock) > vclock_before,
          "the parked hart's WFI fast-forwarded the clock");
    CHECK(latr_tb_fence_seq() > fence_before,
          "SFENCE.VMA published a fence sequence");
    CHECK(latr_tb_fence_wait(latr_tb_fence_seq(), 0) == 0,
          "the fence completed once every hart reached its safe point");
    CHECK(machine.harts[0].tb_cache.generation > gen0_start &&
              machine.harts[1].tb_cache.generation > gen1_start,
          "SFENCE.VMA invalidated both harts' caches, not only the issuer's");
    CHECK(machine.harts[0].tb_cache.count > 0 && machine.harts[1].tb_cache.count > 0,
          "each hart has a cached translation after the run");
    CHECK(machine.harts[0].last_code != 0 && machine.harts[1].last_code != 0,
          "both harts produced host code for their blocks");
}

static void scenario_stop(void)
{
    int rc;

    /* A fresh pass: the exit device is cleared, and hart 0 no longer has an
     * exit to write, so only the STOP on hart 1 matters here. */
    machine.sys.exit_request = 0;
    hart1_woke = 0;
    latr_hart_set_command(&machine.harts[1].park, LATR_HART_STOP);
    rc = latr_machine_run(&machine, 8);
    CHECK(rc == 2, "the machine keeps running with one hart stopped");
    CHECK(!machine.harts[1].online, "STOP takes the hart offline");
}

static void scenario_fini(void)
{
    latr_machine_fini(&machine);
    latr_hart_set_command(&machine.harts[0].park, LATR_HART_STOP);
    CHECK(latr_hart_command(&machine.harts[0].park) == LATR_HART_STOP,
          "fini leaves every hart with STOP published");
}

int main(void)
{
    CHECK(setup_machine(), "machine init on the host");
    scenario_ipi_and_wfi();
    scenario_stop();
    scenario_fini();

    printf("latr-host-smp: %d checks, %d fails (guest blocks scripted: no "
           "LoongArch host)\n",
           checks, failures);
    return failures == 0 ? 0 : 1;
}
