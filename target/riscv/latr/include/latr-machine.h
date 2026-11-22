#ifndef LATR_MACHINE_H
#define LATR_MACHINE_H

#include "latr-system.h"
#include "latr.h"

/*
 * QEMU-virt-style RISC-V machine: OpenSBI-compatible M-mode firmware entry at
 * the start of RAM, raw Linux Image at +2 MiB, DTB near the end of RAM and an
 * optional initrd below it. The machine owns one translator context and TB
 * cache per hart; latr_machine_run() dispatches them cooperatively in the
 * calling host thread.
 */
#define LATR_MACHINE_MAX_HARTS 4u
#define LATR_MACHINE_FIRMWARE_BASE LATR_SYSTEM_RAM_BASE
#define LATR_MACHINE_KERNEL_BASE (LATR_SYSTEM_RAM_BASE + UINT64_C(0x00200000))
#define LATR_MACHINE_FIRMWARE_MAX UINT64_C(0x00200000)
#define LATR_MACHINE_DTB_SIZE (64u << 10)
#define LATR_MACHINE_DTB_RESERVE UINT64_C(0x00100000)
#define LATR_MACHINE_VIRTIO_BASE UINT64_C(0x10001000)
#define LATR_MACHINE_VIRTIO_STRIDE UINT64_C(0x1000)
#define LATR_MACHINE_VIRTIO_COUNT 8u
#define LATR_MACHINE_UART_IRQ 10u
#define LATR_MACHINE_VIRTIO_IRQ_BASE 1u
#define LATR_MACHINE_TIMEBASE 10000000u

/* Host trampoline that jumps into generated code with `env` in the ABI's
 * CPU-state register. The VMM (or bare-metal harness) supplies it. */
typedef void (*LatrMachineEnterTb)(void *env, void *code);

typedef struct LatrMachineHart {
    CPUArchState env;
    CPUState cpu;
    LatrTbCache tb_cache;
    LatrTranslatorContext *ctx;
    uint8_t *arena;
    uint32_t arena_size;
    uint8_t *code_buf;
    uint32_t code_buf_size;
    uint32_t hartid;
    bool online;
} LatrMachineHart;

typedef struct LatrMachine {
    LatrSystem sys;
    uint32_t nharts;
    uint32_t boot_hart;
    uint64_t firmware_entry;
    uint64_t kernel_entry;
    uint64_t kernel_end;
    uint64_t dtb_addr;
    uint64_t dtb_size;
    uint64_t initrd_start;
    uint64_t initrd_end;
    bool firmware_loaded;
    bool kernel_loaded;
    bool initrd_loaded;
    bool dtb_ready;
    bool booted;
    bool failed;
    bool trapped;
    uint64_t first_trap_pc;
    uint64_t first_trap_cause;
    uint64_t first_trap_bad;
    uint64_t first_trap_fetch;
    uint64_t first_trap_mem;
    uint64_t kernel_fetch_cause;
    uint64_t kernel_cpu_state;
    uint64_t kernel_env;
    uint64_t kernel_priv;
    bool kernel_probed;
    LatrMachineEnterTb enter_tb;
    char bootargs[256];
    LatrMachineHart harts[LATR_MACHINE_MAX_HARTS];
} LatrMachine;

/* `work` is split into one TB arena and one IR2 staging buffer per hart. */
int latr_machine_init(LatrMachine *m, uint8_t *ram, uint64_t ram_size,
                      uint32_t nharts, uint8_t *work, uint32_t work_size);
void latr_machine_fini(LatrMachine *m);
/* Raw OpenSBI fw_jump/fw_dynamic image at the start of RAM. */
int latr_machine_load_firmware(LatrMachine *m, const uint8_t *image,
                               uint64_t len);
/* ELF firmware; segments load at p_paddr and the entry is used as-is. */
int latr_machine_load_firmware_elf(LatrMachine *m, const uint8_t *elf,
                                   uint64_t len);
/* Flat RISC-V Linux Image (or any S-mode payload) at +2 MiB. */
int latr_machine_load_kernel(LatrMachine *m, const uint8_t *image,
                             uint64_t len);
int latr_machine_load_initrd(LatrMachine *m, const uint8_t *image,
                             uint64_t len);
int latr_machine_set_bootargs(LatrMachine *m, const char *args);
void latr_machine_set_enter_tb(LatrMachine *m, LatrMachineEnterTb fn);
int latr_machine_build_dtb(LatrMachine *m);
/* Program a0/a1 (hartid/DTB), PC=firmware entry and M-mode privilege. */
int latr_machine_boot(LatrMachine *m);
/* Cooperative dispatch of all online harts. Returns 1 on exit request,
 * 0 on a guest fault/translation failure, 2 when max_tbs was reached. */
int latr_machine_run(LatrMachine *m, uint32_t max_tbs);

#endif
