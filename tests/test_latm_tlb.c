#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "internal.h"
#include "latm-helpers.h"
#include "exec/cpu_ldst.h"

static int fault_seen;

void cpu_loop_exit(struct CPUState *cs)
{
    (void)cs;
    fault_seen = 1;
}

static int checks, fails;
static void check(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s got=%llx want=%llx\n", what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

int main(void)
{
    CPUMIPSState env;
    CPUMIPSTLBContext tlb;
    static uint8_t buffer[4096] __attribute__((aligned(4096)));
    uint64_t vaddr = 0xffffffffe0001000ull;
    uint64_t paddr = (uint64_t)(uintptr_t)buffer;
    uint64_t value = 0x1122334455667788ull;
    uint64_t phys;

    memset(&env, 0, sizeof(env));
    env.SEGBITS = 42;
    env.PABITS = 36;
    latm_tlb_init(&env, &tlb, 16);
    env.CP0_SegCtl0 = (CP0SC_AM_MK << CP0SC_AM) |
                      ((CP0SC_AM_MK << CP0SC_AM) << 16);
    env.CP0_EntryHi = vaddr;
    env.CP0_EntryLo0 = ((paddr >> 12) << 6) | 2 | 4;
    env.CP0_EntryLo1 = ((paddr >> 12) << 6) | 2 | 4;
    env.CP0_Index = 0;
    latm_helper_tlb(&env, MIPS_TLB_TLBWI);
    *(uint64_t *)(uintptr_t)buffer = value;
    fault_seen = 0;
    phys = cpu_mips_translate_address(&env, vaddr, MMU_DATA_LOAD, 0);
    check(fault_seen, 0, "TLB walk did not fault");
    check(phys, paddr, "TLB maps kseg3");
    fault_seen = 0;
    check(cpu_ldq_mmuidx_ra(&env, vaddr, 0, 0), value, "TLB-backed load");
    check(fault_seen, 0, "TLB load did not fault");

    env.CP0_EntryLo0 = ((paddr >> 12) << 6) | 4; /* V0=0 */
    env.CP0_EntryLo1 = ((paddr >> 12) << 6) | 4;
    env.CP0_Index = 0;
    latm_helper_tlb(&env, MIPS_TLB_TLBWI);
    env.hflags = 0;
    env.active_tc.PC = 0xffffffffe0001000;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    fault_seen = 0;
    phys = cpu_mips_translate_address(&env, 0xffffffffe0001000,
                                      MMU_DATA_LOAD, 0);
    check(fault_seen, 1, "invalid TLB entry raises");
    check(phys, 0, "failed translation returns zero");
    check(env.CP0_BadVAddr, 0xffffffffe0001000, "BadVAddr on TLB fault");
    check((env.CP0_Cause >> 2) & 0x1f, EXCP_TLBL, "TLB fault Cause");

    env.hflags = 0;
    env.active_tc.PC = 0xffffffffe0001000;
    env.exception_base = 0x80000000;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    do_raise_exception_err(&env, EXCP_TLBL, 0, 0);
    check(fault_seen, 1, "exception exited");
    check(env.CP0_EPC, 0xffffffffe0001000, "exception EPC");
    check((env.CP0_Cause >> 2) & 0x1f, EXCP_TLBL, "Cause.ExcCode");
    check((env.CP0_Status >> CP0St_EXL) & 1, 1, "EXL");
    check(env.active_tc.PC, 0x80000180, "exception vector");

    env.hflags = MIPS_HFLAG_B;
    env.active_tc.PC = 0xffffffffe0001004;
    env.CP0_Status = 0;
    env.CP0_Cause = 0;
    do_raise_exception_err(&env, EXCP_TLBL, 0, 0);
    check(env.CP0_EPC, 0xffffffffe0001000, "delay-slot EPC");
    check((env.CP0_Cause >> 31) & 1, 1, "Cause.BD");

    {
        static const uint32_t excs[] = {
            EXCP_SYSCALL, EXCP_BREAK, EXCP_OVERFLOW, EXCP_FPE
        };
        for (unsigned i = 0; i < sizeof(excs) / sizeof(excs[0]); i++) {
            env.hflags = MIPS_HFLAG_B;
            env.active_tc.PC = 0xffffffffe0001004;
            env.CP0_Status = 0;
            env.CP0_Cause = 0;
            fault_seen = 0;
            do_raise_exception_err(&env, excs[i], 0, 0);
            check(env.CP0_EPC, 0xffffffffe0001000, "delay exception EPC");
            check((env.CP0_Cause >> 31) & 1, 1, "delay exception BD");
            check((env.CP0_Cause >> 2) & 0x1f, excs[i],
                  "delay exception cause");
        }
    }

    printf("latm-tlb: checks=%d fails=%d\n", checks, fails);
    return fails != 0;
}
