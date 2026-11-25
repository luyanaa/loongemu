/* Embeds the Loongson-3 vmlinux the caller supplied via $(LOONGSON_KERNEL).
 * The kernel is not vendored - it is a Debian package artifact and lives
 * outside the tree. */
    .section .rodata
    .balign 16
    .globl latm_linux_elf
    .globl latm_linux_elf_end
latm_linux_elf:
    .incbin "build/vmlinuz-loongson-3"
latm_linux_elf_end:
