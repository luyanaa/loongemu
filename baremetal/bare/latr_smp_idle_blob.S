/* Embeds the scripted-scenario firmware (see latr_smp_idle.S) as a flat
 * RISC-V image, exactly like latr_smp_blob.S does for the SMP guest. */
    .section .rodata
    .balign 8
    .globl latr_smp_idle
    .globl latr_smp_idle_end
latr_smp_idle:
    .incbin "build/latr_smp_idle.bin"
latr_smp_idle_end:
