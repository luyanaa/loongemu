/*
 * Bare-metal driver for the LATR fault differential.
 *
 * Runs the guest in baremetal/bare/latr_fault_guest.S through the real machine
 * (one hart, M-mode) and echoes the guest's UART output, which carries the
 * `latr-fault-diff=<hex>` line. The same guest binary is run natively by
 * qemu-system-riscv64 (`make latr-fault-ref`), so the comparison covers the
 * whole trap path: decode, address resolution, cause/mepc/mtval, and the return
 * into the handler.
 *
 * The guest is loaded as firmware: it is position-dependent at 0x80000000,
 * which is what both the machine's firmware window and QEMU's -kernel entry use.
 */
#include <stdint.h>
#include "latr.h"
#include "latr-machine.h"

extern void latr_enter_tb(void *env, void *code);
extern uint8_t latr_fault_guest[];
extern uint8_t latr_fault_guest_end[];

__asm__(
".text\n"
".globl latr_enter_tb\n"
"latr_enter_tb:\n"
"  addi.d $sp, $sp, -16\n"
"  st.d   $ra, $sp, 0\n"
"  st.d   $s8, $sp, 8\n"
"  move   $s8, $a0\n"
"  jirl   $ra, $a1, 0\n"
"  ld.d   $ra, $sp, 0\n"
"  ld.d   $s8, $sp, 8\n"
"  addi.d $sp, $sp, 16\n"
"  jirl   $zero, $ra, 0\n");

#define UART0_THR ((volatile uint8_t *)0x1fe001e0)
static void uart_putc(int c)
{
    *UART0_THR = (uint8_t)c;
}
static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}
static void uart_dec(uint64_t v)
{
    char b[21];
    int i = 20;

    b[i] = 0;
    do {
        b[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && i > 0);
    uart_puts(&b[i]);
}

void kmain(void);
__asm__(".text\n.globl _start\n_start:\n  pcalau12i $sp, %pc_hi20(latr_fault_stack)\n  addi.d $sp, $sp, %pc_lo12(latr_fault_stack)\n"
        "  lu12i.w $t0, 128\n  add.d $sp, $sp, $t0\n  bl kmain\n1: b 1b\n");
/* Same reason as the SMP driver: the machine + translator path is deep, and a
 * small stack corrupts callee-saved pointers instead of faulting cleanly. */
static uint8_t latr_fault_stack[1u << 20] __attribute__((aligned(16), used));

static uint8_t ram[16u << 20] __attribute__((aligned(4096)));
static uint8_t machine_work[(2u << 20) * 2u] __attribute__((aligned(4096)));
static LatrMachine machine;

void kmain(void)
{
    int rc;

    uart_puts("latr-fault: begin\n");
    if (!latr_machine_init(&machine, ram, sizeof(ram), 1, machine_work,
                           sizeof(machine_work))) {
        uart_puts("latr-fault: FAIL machine init\nlatr-fault: done\n");
        return;
    }
    latr_machine_set_enter_tb(&machine, latr_enter_tb);
    if (!latr_machine_load_firmware(
            &machine, latr_fault_guest,
            (uint64_t)(latr_fault_guest_end - latr_fault_guest))) {
        uart_puts("latr-fault: FAIL firmware load\nlatr-fault: done\n");
        return;
    }
    if (!latr_machine_build_dtb(&machine) || !latr_machine_boot(&machine)) {
        uart_puts("latr-fault: FAIL boot\nlatr-fault: done\n");
        return;
    }

    rc = latr_machine_run(&machine, 2000);
    uart_puts("latr-fault: guest=");
    uart_puts(latr_system_uart_log(&machine.sys));
    uart_puts("\nlatr-fault: rc=");
    uart_dec((uint64_t)rc);
    uart_puts(" exit=");
    uart_dec(machine.sys.exit_request);
    uart_puts(" trapped=");
    uart_dec((uint64_t)machine.trapped);
    uart_puts(" trap-cause=");
    uart_dec(machine.first_trap_cause);
    uart_puts(" tbs=");
    uart_dec(machine.harts[0].tbs);
    uart_puts("\n");
    uart_puts(rc == 1 && machine.sys.exit_request == 0x5555
                  ? "latr-fault: PASS\n"
                  : "latr-fault: FAIL\n");
    uart_puts("latr-fault: done\n");
}
