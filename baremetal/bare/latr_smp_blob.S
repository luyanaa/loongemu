/* Embeds the two-hart SMP guest as a flat RISC-V image (see
 * latr_smp_guest.S). The Makefile assembles and links it at 0x80000000, which
 * is where latr_machine_load_firmware() places it, then converts it to a raw
 * binary; this object only carries the bytes. */
    .section .rodata
    .balign 8
    .globl latr_smp_guest
    .globl latr_smp_guest_end
latr_smp_guest:
    .incbin "build/latr_smp_guest.bin"
latr_smp_guest_end:
