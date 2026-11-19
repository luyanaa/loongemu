.section .rodata
.balign 4096
.globl _ktext_start
_ktext_start:
.incbin "firmware/vmlinux-x86_64-text.bin"
.globl _ktext_end
_ktext_end:
.text
