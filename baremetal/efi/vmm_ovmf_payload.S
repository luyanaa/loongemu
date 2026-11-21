#ifndef LOONGEMU_OVMF_CODE_PATH
#error "LOONGEMU_OVMF_CODE_PATH must name an x86-64 OVMF_CODE.fd"
#endif
#ifndef LOONGEMU_OVMF_VARS_PATH
#error "LOONGEMU_OVMF_VARS_PATH must name an x86-64 OVMF_VARS.fd"
#endif

.section .rodata.ovmf_code,"a",@progbits
.balign 4096
.globl _ovmf_code_start
_ovmf_code_start:
.incbin LOONGEMU_OVMF_CODE_PATH
.globl _ovmf_code_end
_ovmf_code_end:

.section .rodata.ovmf_vars,"a",@progbits
.balign 4096
.globl _ovmf_vars_start
_ovmf_vars_start:
.incbin LOONGEMU_OVMF_VARS_PATH
.globl _ovmf_vars_end
_ovmf_vars_end:

.text
