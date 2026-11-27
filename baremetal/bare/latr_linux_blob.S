/* Embeds the RISC-V kernel image the caller supplied via $(LATR_LINUX_IMAGE).
 * The image is not vendored - it is a distribution package artifact (for
 * example AOSC's linux-kernel-*-riscv64.deb, whose /boot/vmlinux-* file is a
 * PE-wrapped flat Image that QEMU also loads whole), so this target is
 * local-only and never part of CI. */
    .section .rodata
    .balign 16
    .globl latr_linux_image
    .globl latr_linux_image_end
latr_linux_image:
    .incbin "build/linux/Image"
latr_linux_image_end:
