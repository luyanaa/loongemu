/* Embeds the SMC stress firmware (see latr_smp_smc.S) as a flat RISC-V image. */
    .section .rodata
    .balign 8
    .globl latr_smp_smc
    .globl latr_smp_smc_end
latr_smp_smc:
    .incbin "build/latr_smp_smc.bin"
latr_smp_smc_end:
