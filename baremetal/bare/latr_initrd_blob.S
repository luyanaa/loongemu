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
    .incbin "firmware/initramfs.cpio.gz"
latr_initrd_image_end:
