/* Embeds the initramfs handed to the guest kernel through the DTB's
 * linux,initrd-start/end. The asset is the repo's test initramfs
 * (firmware/initramfs.cpio.gz), not a distribution artifact, so unlike the
 * kernel blob this one is always available. A kernel with no root= and no
 * initrd panics in mount_root; this is what turns the boot past "Freeing
 * unused kernel memory" into a real /init. */
    .section .rodata
    .balign 16
    .globl latr_initrd_image
    .globl latr_initrd_image_end
latr_initrd_image:
/* LATR_INITRD_PATH is passed by the Makefile so the default stays the repo
 * asset while a riscv64 userspace can be selected without editing this file. */
#ifndef LATR_INITRD_PATH
#define LATR_INITRD_PATH "firmware/initramfs.cpio.gz"
#endif
    .incbin LATR_INITRD_PATH
latr_initrd_image_end:
