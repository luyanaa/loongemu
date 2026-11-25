#ifndef LATR_SYSTEM_H
#define LATR_SYSTEM_H

#include "latr.h"
#include "loongemu/guest_dma.h"
#include "loongemu/mmio_bus.h"
#include "loongemu/phys_mem.h"
#include "loongemu/riscv_clint.h"
#include "loongemu/riscv_plic.h"
#include "loongemu/uart16550.h"
#include "loongemu/riscv_fw.h"
#include "loongemu/ovmf_board.h"
#include "loongemu/vclock.h"

/* Minimal RISC-V virt board used by the LATR system/boot loop. */
#define LATR_SYSTEM_RAM_BASE    UINT64_C(0x80000000)
#define LATR_SYSTEM_CLINT_BASE  UINT64_C(0x02000000)
#define LATR_SYSTEM_PLIC_BASE   UINT64_C(0x0c000000)
#define LATR_SYSTEM_UART_BASE   UINT64_C(0x10000000)
#define LATR_SYSTEM_EXIT_BASE   UINT64_C(0x00100000)
/* QEMU virt sizes the SiFive test device at 4 KiB and the DTB says so, so back
 * the whole window instead of only the one word the device actually decodes. */
#define LATR_SYSTEM_EXIT_SIZE   UINT64_C(0x1000)
#define LATR_SYSTEM_FWCFG_BASE  UINT64_C(0x10100000)
#define LATR_SYSTEM_PFLASH_CODE UINT64_C(0x20000000)
#define LATR_SYSTEM_PFLASH_VARS UINT64_C(0x22000000)
#define LATR_SYSTEM_PFLASH_CODE_SIZE (1u << 20)
#define LATR_SYSTEM_PFLASH_VARS_SIZE (1u << 16)

/*
 * Device-model serialisation (design: latr-smp-concurrency.md section 7).
 *
 * All of LatrSystem's mutable state (MMIO windows, CLINT, PLIC, UART, virtio,
 * ACPI tables) and the bus behind them are guarded by one spin lock. Guest RAM
 * is not: it is ordinary shared memory accessed by plain host loads/stores and
 * host atomics. The lock is never held across a translated block, only across
 * one device access or one bookkeeping update, so the failure mode is a lost
 * wake-up rather than a stalled hart.
 *
 * Ordering rule (section 6): device lock -> DMA lock -> reservation epoch. A
 * device step may call the DMA path, never the other way round.
 */
#ifndef LATR_SYSTEM_LOCK_SPINS
#define LATR_SYSTEM_LOCK_SPINS UINT64_C(1) << 24
#endif

typedef struct LatrSystem {
    CPUArchState env;
    LoongemuMemMap map;
    LoongemuMmioBus bus;
    /* Device-side view of guest RAM: every DMA store is published through it so
     * the LR/SC reservation domain is invalidated for the touched range. */
    LoongemuGuestDma dma;
    LoongemuClint clint;
    LoongemuPlic plic;
    LoongemuUart16550 uart;
    LoongemuVClock vclock;
    uint8_t *ram;
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t exit_request;
    /* Highest guest address touched by latr_system_load_elf(). */
    uint64_t image_end;
    /* Console tail captured for the harness. It must be big enough to hold a
     * full firmware banner: OpenSBI prints its version plus a seven-line logo
     * and then its platform/domain feature lists, which together run well past
     * 1 KiB. A 256-byte window ended mid-logo and a 1 KiB window cut off at the
     * logo, and both look exactly like a guest that stopped printing -- which
     * sent an earlier investigation chasing a phantom wedge. Once the S-mode
     * payload started printing through SBI ecall the need for a larger window
     * became obvious, so size it for a full boot log, not just a banner. */
    char uart_log[8192];
    uint32_t uart_log_len;
    uint64_t rsdp_gpa;
    uint64_t acpi_tables_gpa;
    uint32_t acpi_tables_len;
    LoongemuFwCfg fw_cfg;
    LoongemuFwCfgDmaOps fw_dma_ops;
    LoongemuRiscvPflash pflash;
    uint8_t pflash_code[LATR_SYSTEM_PFLASH_CODE_SIZE];
    uint8_t pflash_vars[LATR_SYSTEM_PFLASH_VARS_SIZE];
    /* Guarded-state lock; failures are counted, never silently ignored. */
    volatile int device_lock;
    volatile uint64_t device_lock_acquisitions;
    volatile uint64_t device_lock_failures;
} LatrSystem;

int latr_system_init(LatrSystem *sys, uint8_t *ram, uint64_t ram_size,
                     uint32_t hartid);
/* Same board with PLIC contexts for `nharts` harts (2*hart = M, 2*hart+1 = S). */
int latr_system_init_harts(LatrSystem *sys, uint8_t *ram, uint64_t ram_size,
                           uint32_t nharts, uint32_t boot_hart);
int latr_system_load_elf(LatrSystem *sys, const uint8_t *elf, uint64_t len,
                         uint64_t *entry);
/* Advances the shared virtual clock once per machine step. */
void latr_system_tick(LatrSystem *sys, uint64_t ns);
/* Timer comparator due now? Used by the machine's ticker to wake parked harts;
 * reads the hart state atomically so a concurrently dispatching hart is safe. */
int latr_system_timer_due(LatrSystem *sys, const CPUArchState *env,
                          uint32_t hartid);
uint64_t latr_system_lock_failures(const LatrSystem *sys);
/* Reflects CLINT/device state for one hart into that hart's MIP. */
void latr_system_update_hart(LatrSystem *sys, CPUArchState *env, uint32_t hartid);
/* Hart 0 convenience wrapper: tick 1 us then update hart 0. */
void latr_system_update(LatrSystem *sys);
void latr_system_mmio_write(LatrSystem *sys, uint64_t pa, unsigned size,
                            uint64_t value);
const char *latr_system_uart_log(const LatrSystem *sys);
int latr_system_build_acpi(LatrSystem *sys, uint64_t *rsdp_gpa);
int latr_system_load_firmware(LatrSystem *sys, const uint8_t *image,
                              uint64_t len);

#endif
