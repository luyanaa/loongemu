/* Embeds the fault-differential guest as a flat RISC-V image (see
 * latr_fault_guest.S). Linked at 0x80000000, which is where
 * latr_machine_load_firmware() places it and where qemu-system-riscv64's
 * -kernel entry goes, so both engines run the same bytes at the same address. */
    .section .rodata
    .balign 8
    .globl latr_fault_guest
    .globl latr_fault_guest_end
latr_fault_guest:
    .incbin "build/latr_fault_guest.bin"
latr_fault_guest_end:
