/*
 * The remaining PT_LOAD segments of the same x86-64 vmlinux.
 *
 * Segment 1 covers 0xffffffff82800000 (rel 0x1800000) and segment 3 covers
 * 0xffffffff82d8f000 (rel 0x1d8f000); the first segment's text is carried in
 * vmlinux_text.S.  All three are placed at their load offsets from 0x1000000,
 * which is where the kernel is run, so its absolute addresses resolve.
 *
 * Provenance: firmware/vmlinux-x86_64-seg{1,3}.bin
 */
.section .rodata
.balign 4096
.globl _kseg1_start
_kseg1_start:
.incbin "firmware/vmlinux-x86_64-seg1.bin"
.globl _kseg1_end
_kseg1_end:
.balign 4096
.globl _kseg2_start
_kseg2_start:
.incbin "firmware/vmlinux-x86_64-seg2.bin"
.globl _kseg2_end
_kseg2_end:
.balign 4096
.globl _kseg3_start
_kseg3_start:
.incbin "firmware/vmlinux-x86_64-seg3.bin"
.globl _kseg3_end
_kseg3_end:
.balign 4096
.globl _initramfs_start
_initramfs_start:
#ifndef LOONGEMU_INITRAMFS_PATH
#define LOONGEMU_INITRAMFS_PATH "firmware/initramfs.cpio.gz"
#endif
.incbin LOONGEMU_INITRAMFS_PATH
.globl _initramfs_end
_initramfs_end:
.text
