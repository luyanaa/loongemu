/* Raw prebuilt OpenSBI fw_dynamic image, embedded for the optional smoke.
 * The Makefile copies the binary to build/opensbi64.bin before assembling. */
.section .rodata
.balign 8
.globl latr_opensbi_image
.globl latr_opensbi_end
latr_opensbi_image:
.incbin "build/opensbi64.bin"
latr_opensbi_end:
