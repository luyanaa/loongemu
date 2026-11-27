#ifndef LATR_MACHINE_H
#define LATR_MACHINE_H

#include "latr-system.h"
#include "latr.h"
#include "latr-hart-sync.h"
#include "latr-chain.h"
#include "loongemu/riscv_virtio_blk.h"

/*
 * QEMU-virt-style RISC-V machine: OpenSBI-compatible M-mode firmware entry at
 * the start of RAM, raw Linux Image at +2 MiB, DTB near the end of RAM and an
 * optional initrd below it. The machine owns one translator context and TB
 * cache per hart; latr_machine_run() dispatches them cooperatively in the
 * calling host thread.
 */
#define LATR_MACHINE_MAX_HARTS 4u
/*
 * Fence completion bound (design: docs/design/latr-smp-concurrency.md section
 * 5). A spin count, not a wall clock, so the freestanding images need no clock
 * source; the threaded scheduler treats exhaustion as a fatal machine error.
 */
#ifndef LATR_SMP_ACK_SPINS
#define LATR_SMP_ACK_SPINS UINT64_C(100000000)
#endif
#define LATR_MACHINE_FIRMWARE_BASE LATR_SYSTEM_RAM_BASE
#define LATR_MACHINE_KERNEL_BASE (LATR_SYSTEM_RAM_BASE + UINT64_C(0x00200000))
#define LATR_MACHINE_FIRMWARE_MAX UINT64_C(0x00200000)
/* fw_dynamic_info lives in the low boot ROM, mirroring QEMU's MROM. */
#define LATR_MACHINE_FW_DYNAMIC_INFO UINT64_C(0x100)
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

/* Optional probe invoked with the guest PC immediately *before* a block is
 * translated. A translator bug that loops forever never reaches enter_tb, so
 * without this hook the dispatch loop looks like it vanished: this names the
 * block that is being translated when the machine stops making progress. */
typedef void (*LatrMachineTranslateTb)(void *env, uint64_t pc);

/* Invoked with the guest PC right after the block has been assembled, i.e.
 * the translation is complete and the harness is about to enter the generated
 * code. Together with translate_tb this splits a stall into "stuck in the
 * translator" (only translate_tb fires) versus "stuck executing generated
 * code" (both fire, enter_tb fires, and nothing comes back). */
typedef void (*LatrMachineTranslatedTb)(void *env, uint64_t pc,
                                        uint32_t host_words,
                                        uint32_t guest_insns);

typedef struct LatrMachineFaultInfo {
    uint64_t host_pc;
    uint64_t guest_pc;
    uint64_t badaddr;
    uint32_t cause;
    uint32_t hartid;
    bool resolved;
    /* C1 precise recovery: fault-site classification + guest-insn retirement
     * + how many regcache bindings were live at the fault span. `site` is a
     * LatrRecoverySite; NATIVE_INTERNAL means the fault is a host bug and was
     * NOT projected to a guest exception. */
    uint8_t  site;
    uint8_t  phase; /* LeRecoveryPhase: how much of the span may have landed */
    uint32_t retired_before;
    uint8_t  bind_cnt;
    /* C1-followup: 1 when the fault-time host register snapshot was supplied
     * and latr_tb_recovery_apply() replayed the span's bindings (including the
     * empty-binding case -- architectural state was already committed). 0 when
     * the snapshot was missing. Since P0 recovery-v2 a span carrying bindings
     * with no snapshot fails closed: the function returns 0 and no guest trap
     * is taken, rather than delivering a trap over incomplete state. */
    uint8_t  applied;
} LatrMachineFaultInfo;

typedef struct LatrMachineHart {
    CPUArchState env;
    /* Run/wake/exit handshake with the scheduler (and, later, other harts). */
    LatrHartSync park;
    /* Chain-control state machine (design: chain-control-ras.md). P0 counts
     * breaks and never links; the epoch is bumped at each real break point. */
    LatrChainControl chain;
    CPUState cpu;
    LatrTbCache tb_cache;
    LatrTranslatorContext *ctx;
    uint8_t *arena;
    uint32_t arena_size;
    uint8_t *code_buf;
    uint32_t code_buf_size;
    uint32_t hartid;
    uint64_t tbs;
    uint64_t last_code;
    bool online;
} LatrMachineHart;

typedef struct LatrMachine {
    LatrSystem sys;
    LoongemuVirtioBlk virtio_blk;
    uint8_t boot_rom[0x1000];
    uint8_t *disk;
    uint64_t disk_size;
    bool disk_attached;
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
    uint64_t last_pc;
    /*
     * Every guest trap the dispatcher delivers. The harness prints its delta
     * per window: a kernel loop that is merely slow keeps this near zero, while
     * a translator that keeps taking an exception path shows a high rate even
     * when the guest PCs look like ordinary code.
     */
    uint64_t trap_count;
    /*
     * fence.i verification bookkeeping. `verify_checks` counts lookups that had
     * to prove their block after a fence; `verify_drops` counts the ones refused
     * (and therefore re-translated). A drop rate that is not near zero means the
     * byte comparison is rejecting blocks it should accept, and the phase cost is
     * re-translation, not verification.
     */
    uint64_t verify_checks;
    uint64_t verify_drops;
    uint64_t dispatch_count;
    uint64_t wake_events;
    LatrMachineEnterTb enter_tb;
    LatrMachineTranslateTb translate_tb;
    LatrMachineTranslatedTb translated_tb;
    /*
     * SBI services for the S-mode `ecall` this machine intercepts. A Linux
     * kernel that boots without firmware (or with a stand-in) issues TIME,
     * IPI, RFENCE, HSRT and DBCN through `ecall`; the machine answers them
     * itself and never exposes an M-mode trap handler to the guest.
     */
    LoongemuSbi sbi;
    LoongemuSbiCalls sbi_calls;
    uint64_t sbicalls_serviced;
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
/* Attaches a RAM-backed virtio-mmio block device at 0x10001000 (PLIC 1).
 *
 * A zero length is the "no -drive" case and is not an error: the machine model
 * degrades to a board with no block device at all -- no MMIO window is mapped,
 * no IRQ is wired, build_dtb() emits no virtio node, and the call returns 1 with
 * `disk_attached` still false. A NULL image with a non-zero length, a disk
 * smaller than one sector, or a second attach all return 0. */
int latr_machine_attach_disk(LatrMachine *m, uint8_t *image, uint64_t len);
int latr_machine_set_bootargs(LatrMachine *m, const char *args);
/*
 * Host-fault dispatcher closure: attributes a native fault PC to the guest
 * instruction that produced it and enters the guest's own trap handler at
 * that instruction (the full mepc/mcause/mtval/mstatus/privilege/tvec entry,
 * as the generated transfer_exception() would have performed), then drops the
 * native blocks so the next dispatch re-translates from the trap vector.
 * Returns 0 when the host PC is not inside any translated block of the hart.
 */
int latr_machine_host_fault(LatrMachine *m, uint32_t hartid,
                            uintptr_t host_pc, uint32_t cause,
                            uint64_t badaddr,
                            const uint64_t host_gprs[32],
                            const uint64_t host_fprs[32],
                            LatrMachineFaultInfo *info);
void latr_machine_set_enter_tb(LatrMachine *m, LatrMachineEnterTb fn);
void latr_machine_set_translate_tb(LatrMachine *m, LatrMachineTranslateTb fn);
void latr_machine_set_translated_tb(LatrMachine *m, LatrMachineTranslatedTb fn);
int latr_machine_build_dtb(LatrMachine *m);
/* Program a0/a1 (hartid/DTB), PC=firmware entry and M-mode privilege. */
int latr_machine_boot(LatrMachine *m);
/* Cooperative dispatch of all online harts. Returns 1 on exit request,
 * 0 on a guest fault/translation failure, 2 when max_tbs was reached. */
int latr_machine_run(LatrMachine *m, uint32_t max_tbs);
/*
 * One hart, in the calling thread: LRGB's service loop for a scheduler that
 * runs each hart on its own thread. Same return codes as run_hart internally
 * (1 = dispatched a block, 2 = yielded without dispatching, 3 = STOP honoured
 * and the hart is now offline, 0 = failure and the machine is marked failed).
 * The machine itself owns no threads: the embedder creates them, calls this in
 * a loop, and parks on LatrHartSync when the hart is idle.
 */
int latr_machine_run_hart(LatrMachine *m, uint32_t hartid, uint32_t max_tbs);
/*
 * The ticker's job, for a machine whose harts run on their own threads: advance
 * the shared clock and wake every hart whose timer has become due (a hart
 * parked in WFI cannot sample its own comparator). Exactly one thread calls
 * this; it is also what latr_machine_run uses per cooperative step.
 */
void latr_machine_tick(LatrMachine *m, uint64_t ns);
/*
 * Makes progress possible on a parked hart: bumps its wake counter and asks it
 * to return to the dispatcher after the block it is in. Called from the CLINT
 * MSIP write path (an IPI is the only thing that can wake a WFI hart) and from
 * the device step when it raises a deliverable interrupt.
 */
void latr_machine_wake_hart(LatrMachine *m, uint32_t hartid);
/* Total wake events delivered to harts; diagnostics for "did the IPI arrive". */
uint64_t latr_machine_wake_events(const LatrMachine *m);
/* Chain-control counters for one hart (P0: links must stay 0). */
const LatrChainControl *latr_machine_chain(const LatrMachine *m, uint32_t hartid);

#endif
