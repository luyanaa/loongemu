/* Minimal S-mode payload for the OpenSBI smoke test, embedded as a flat image.
 * The Makefile assembles and links baremetal/bare/latr_smode_payload.S at
 * 0x80200000 (LATR_MACHINE_KERNEL_BASE) and converts it to a raw binary here. */
.section .rodata
.balign 8
.globl latr_smode_payload
.globl latr_smode_payload_end
latr_smode_payload:
.incbin "build/latr_smode_payload.bin"
latr_smode_payload_end:
