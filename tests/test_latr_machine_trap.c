/*
 * Host unit test for the runtime synchronous-exception entry
 * (latr_take_guest_trap) and its use by the native-fault dispatcher.
 *
 * Guest traps taken inside translated code already go through the generated
 * transfer_exception() sequence. Traps arriving from outside generated code --
 * a native fault that the dispatcher attributes to a guest instruction -- used
 * to be merely *latched* (env.exception/pc) and then silently dropped by the
 * dispatch loop, so the guest never saw them. latr_take_guest_trap() closes
 * that chain with exactly the semantics the generated code implements:
 *
 *   - M-mode traps to mtvec always; S/U-mode traps to stvec iff the cause is
 *     delegated via medeleg, else to mtvec;
 *   - mstatus.{SPIE,SPP} / {MPIE,MPP} from the *previous* privilege, and the
 *     enable bit cleared;
 *   - epc = faulting PC (bit 0 cleared), cause/tval recorded;
 *   - the trap vector PC is the non-vectored base (vectored mode offsets only
 *     interrupt targets);
 *   - LR reservations do not survive a trap.
 */
#include <stdio.h>
#include <string.h>

#include "latr.h"

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

#define CSR_MSTATUS 0x300
#define CSR_MEDELEG 0x302
#define CSR_MTVEC   0x305
#define CSR_SEPC    0x141
#define CSR_SCAUSE  0x142
#define CSR_STVAL   0x143
#define CSR_STVEC   0x105
#define CSR_MEPC    0x341
#define CSR_MCAUSE  0x342
#define CSR_MTVAL   0x343

#define MSTATUS_SIE  (UINT64_C(1) << 1)
#define MSTATUS_MIE  (UINT64_C(1) << 3)
#define MSTATUS_SPIE (UINT64_C(1) << 5)
#define MSTATUS_MPIE (UINT64_C(1) << 7)
#define MSTATUS_SPP  (UINT64_C(1) << 8)
#define MSTATUS_MPP  (UINT64_C(3) << 11)

static CPUArchState env;

static void reset_env(unsigned privilege, uint64_t status)
{
    memset(&env, 0, sizeof(env));
    env.privilege = privilege;
    env.csr[CSR_MSTATUS] = status;
    env.csr[CSR_STVEC] = UINT64_C(0x1000) | 1; /* vectored mode */
    env.csr[CSR_MTVEC] = UINT64_C(0x2000) | 1; /* vectored mode */
    env.pc = UINT64_C(0x80001234);
    env.reservation_valid = 1;
}

static void test_supervisor_delegated(void)
{
    /* S-mode with the cause delegated: S target. */
    reset_env(LATR_RISCV_PRIV_S, MSTATUS_SIE);
    env.csr[CSR_MEDELEG] = UINT64_C(1) << 13;

    latr_take_guest_trap(&env, 13, UINT64_C(0xdead0));

    CHECK(env.csr[CSR_SEPC] == UINT64_C(0x80001234), "sepc = faulting pc");
    CHECK(env.csr[CSR_SCAUSE] == 13, "scause");
    CHECK(env.csr[CSR_STVAL] == UINT64_C(0xdead0), "stval");
    CHECK(env.csr[CSR_MSTATUS] == (MSTATUS_SPIE | MSTATUS_SPP),
          "SPIE=old SIE, SIE=0, SPP=S");
    CHECK(env.privilege == LATR_RISCV_PRIV_S, "stays in S-mode");
    /* Vectored stvec (mode 1) must dispatch sync exceptions to the base. */
    CHECK(env.pc == UINT64_C(0x1000), "pc = stvec base, not vectored");
    CHECK(env.exception == 13 && env.badaddr == UINT64_C(0xdead0),
          "latched for diagnostics");
    CHECK(env.reservation_valid == 0, "LR reservation invalidated");
}

static void test_supervisor_undelegated(void)
{
    /* S-mode without delegation: M target with MPP = S. */
    reset_env(LATR_RISCV_PRIV_S, MSTATUS_MIE);
    env.csr[CSR_MEDELEG] = 0;

    latr_take_guest_trap(&env, 2, UINT64_C(0x1111));

    CHECK(env.csr[CSR_MEPC] == UINT64_C(0x80001234), "mepc");
    CHECK(env.csr[CSR_MCAUSE] == 2 && env.csr[CSR_MTVAL] == UINT64_C(0x1111),
          "mcause/mtval");
    CHECK(env.csr[CSR_MSTATUS] == (MSTATUS_MPIE | (UINT64_C(1) << 11)),
          "MPIE=old MIE, MIE=0, MPP=S");
    CHECK(env.privilege == LATR_RISCV_PRIV_M, "promoted to M-mode");
    CHECK(env.pc == UINT64_C(0x2000), "pc = mtvec base");
    CHECK(env.csr[CSR_SEPC] == 0, "sepc untouched");
}

static void test_machine_always_m(void)
{
    /* M-mode ignores medeleg entirely. */
    reset_env(LATR_RISCV_PRIV_M, 0);
    env.csr[CSR_MEDELEG] = ~(uint64_t)0;

    latr_take_guest_trap(&env, 5, UINT64_C(0x2222));

    CHECK(env.privilege == LATR_RISCV_PRIV_M, "M trap stays in M-mode");
    CHECK(env.csr[CSR_MCAUSE] == 5, "mcause");
    CHECK(env.csr[CSR_MSTATUS] == MSTATUS_MPP,
          "MPIE=0 (old MIE=0), MPP=M");
    CHECK(env.pc == UINT64_C(0x2000), "pc = mtvec base");
}

static void test_user_delegated(void)
{
    /* U-mode with delegation: S target, SPP = 0 (came from U). */
    reset_env(LATR_RISCV_PRIV_U, MSTATUS_SIE);
    env.csr[CSR_MEDELEG] = UINT64_C(1) << 15;

    latr_take_guest_trap(&env, 15, 0);

    CHECK(env.privilege == LATR_RISCV_PRIV_S, "delegated U trap to S");
    CHECK(env.csr[CSR_MSTATUS] == MSTATUS_SPIE,
          "SPIE=old SIE, SIE=0, SPP=U");
    CHECK(env.csr[CSR_SEPC] == UINT64_C(0x80001234), "sepc");
}

static void test_mepc_bit0_cleared(void)
{
    /* Compressed-instruction misalignment keeps epc halfword aligned. */
    reset_env(LATR_RISCV_PRIV_M, 0);
    env.pc = UINT64_C(0x80001235);

    latr_take_guest_trap(&env, 1, 0);

    CHECK(env.csr[CSR_MEPC] == UINT64_C(0x80001234), "mepc bit 0 cleared");
}

int main(void)
{
    test_supervisor_delegated();
    test_supervisor_undelegated();
    test_machine_always_m();
    test_user_delegated();
    test_mepc_bit0_cleared();

    printf("latr machine trap: %d checks passed", checks);
    if (failures != 0) {
        printf(", %d FAILED\n", failures);
        return 1;
    }
    printf("\n");
    return 0;
}
