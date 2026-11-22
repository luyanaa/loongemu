# LoongEMU

A LoongArch-based binary-translation and virtualization playground. LoongEMU
runs x86-64, AArch64, RISC-V (RV64) and MIPS64 guest code on LoongArch CPUs, and
builds the pieces needed to do it from a freestanding environment: a portable
LBT implementation, a translation-block cache, hardware page tables, a shared
vector IR (PVIR), and bare-metal / UEFI harnesses that run under QEMU.

[中文文档](README.zh_cn.md) · [GPL-2.0](LICENSE)

## Overview

The project is a research/testbed tree rather than a single product binary. Its
central question is: *how far can translated guest code run on real LoongArch
hardware, with no host libc and no hypervisor underneath?*

Guest code is decoded and translated by **LATX** (x86-64 → LoongArch),
**LATA** (AArch64 → LoongArch), **LATR** (RISC-V RV64 → LoongArch), or
**LATM** (MIPS64r2 → LoongArch), all adapted from upstream QEMU-related sources
into self-contained freestanding static archives. x86 arithmetic flags are
handled through the **LBT** (Loongson Binary Translation) extension, while
AArch64 SVE and RISC-V RVV share a common **PVIR** vector IR that lowers to LSX
/ LASX with a helper fallback. Because not every LoongArch core implements LBT,
LoongEMU provides two back ends behind one interface:

| Back end | Target | Notes |
| --- | --- | --- |
| Hardware | LoongArch 3A6000-class cores | Real `x86add.w`, `x86mtflag`, `x86settm`, … instructions; compiled with `+lbt`. |
| Portable fallback | 2K1000LA / 2K2000 and QEMU | Same semantics in integer C. QEMU does not execute LBT, so the bare-metal harness emulates it from the `#INE` vector. |

### Hardware capability tiers

The tree is designed to **degrade gracefully across real LoongArch silicon**;
no test requires a top-of-the-line core. Two independent capability ladders
matter:

- **LBT (x86 flags), two tiers.** The table above is the ladder: hardware LBT
  (`+lbt`, real `x86add.w` / `x86mtflag` / `x86settm`) on 3A6000-class cores, and
  an integer-C fallback on 2K1000LA / 2K2000 / QEMU (emulated from `#INE`).
  Selected via the `LOONGEMU_CPU_LBT_X86` / `LOONGEMU_CPU_LBT_ARM` capability
  bits; checked by `make test-loongarch` and `make test-loongarch-fallback`.
- **Vector (AArch64 SVE / RISC-V RVV via PVIR), three tiers, chosen at run time**
  from `latx_cpu_has_lasx()` / `latx_cpu_has_lsx()`
  (`LOONGEMU_CPU_LASX` / `LOONGEMU_CPU_LSX`):

  | Tier | ISA | Chunk | Use |
  | --- | --- | --- | --- |
  | 2 | LASX (256-bit, `la_xv*`) | 32 B | widest LMUL / SVE vector paths |
  | 1 | LSX (128-bit, `la_v*`) | 16 B | LMUL ≤ 1, predicate chunks, odd tails of wider groups |
  | 0 | C helper | — | shapes that are not statically lowerable |

  Tiers 2 and 1 may be mixed within one instruction (e.g. a 32 B LASX chunk plus
  a 16 B LSX tail). **Guest architectural legality never depends on the host
  tier**: SVE access checks and RVV `vext_check_*` legality are decided from the
  guest ISA at translation time, so a scalar-only host changes only *how* code
  runs, not *what* the guest observes.

### No LVZ dependency

**LVZ is not a prerequisite for any core functionality.** The translators, the
PLV3 isolation probe, the LATR / LATM / LATA bare-metal boards and all host
tests run on a stock `qemu-system-loongarch64` (or the in-tree binary) with
`-cpu la464` / `-cpu max`. In the Makefiles, `QLVZ_REPO` and `QEMU_LA64` are
merely *paths to a QEMU binary* — overridable, and not a statement that the
LoongArch Virtualization extension is needed. `efi-lvz-probe` is an optional
diagnostic that *inspects* LVZ / privileged CSRs and also boots on plain QEMU;
a few legacy recipes (`tb-sys*`, `tb-loop`, `lbt-smoke`) simply default to the
`QLVZ` path variable, which is likewise overridable.

Above that core, the tree explores a thin VMM path. For x86-64, that is LoongArch
4 KiB hardware paging in PLV0, translation blocks executed in PLV3 under a
shadow page table with separate PLV0 / PLV3 page-table roots, guest page faults
delivered through the guest IDT, and a fixed-size translation-block cache keyed
by `(pc, cs_base, cr3, cpl)`. For RISC-V there is a fuller `LatrMachine` /
`LatrSystem` board (Sv39/48/57, CLINT, PLIC, UART, fw_cfg, pflash, ACPI, SBI,
virtio-mmio) that boots real OpenSBI from M-mode into S-mode with SMP harts.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/loongemu/`, `src/` | Portable, host-testable core: `loongbt` (LBT ops + EFLAGS + IR2 mapping), `tb_cache`, `loongarch_paging`, `x86_64_paging`, `soft_ras`, and the RISC-V board models (`riscv_sbi`, `riscv_acpi`, `riscv_plic`, `riscv_clint`, `riscv_fw`, `riscv_virtio_blk`, `phys_mem`, `mmio_bus`). |
| `latx/` | LATX x86-64 translator: Capstone-based IR1 decoder, IR2, translator, optimizations, runtime, and minimal libc/libm. |
| `lata/`, `target/arm/lata/` | LATA AArch64 → LoongArch translator core, including SVE lowering and the shared PVIR (`pvir.c`, `sve-pvir.c`) vector backend. |
| `latr/`, `target/riscv/latr/` | LATR RISC-V RV64 → LoongArch translator core, RVV legality / PVIR offload, machine model and docs (`DESIGN.md`, `MACHINE.md`, `RVV_PVIR.md`, `GAPS.md`, `RVV_P1_LEGALITY.md`). |
| `latm/`, `target/mips/latm/` | LATM MIPS64r2 → LoongArch translator core (integer, LBT unaligned memory, FPU helpers). |
| `target/i386/`, `target/arm/`, `target/riscv/` | Target-side headers and sources consumed by the translators. |
| `baremetal/bare/` | `-kernel` harnesses: single-TB, multi-TB loop, paging smoke, PLV3 isolation probe, LATA/LBT/LATM/LATR smoke, the synthetic LATM MIPS64 board (`latm_board.c`), OpenSBI-to-S-mode (`latr_opensbi_smoke.c`), and the long-mode `tb_sys` bring-up test. |
| `baremetal/efi/` | UEFI applications: LBT fallback self-test, IR1 decode smoke test, LVZ probe, VMM entry. |
| `baremetal/gen/` | Generated LATX headers (`ir2-opcode.h`, `ir2-name.h`, `format-table.h`, …). |
| `firmware/` | Pinned EDK2 `QEMU_EFI.fd` / `QEMU_VARS.fd` and x86-64 kernel snippet binaries, checked by `SHA256SUMS.txt`. |
| `tools/` | Symbol allowlist checks and an EFI image build helper. |
| `tests/` | Host unit tests for each portable module. |
| `Makefile` | Top-level entry point for host tests and QEMU/EFI targets. |

## Requirements

- A C11 host compiler (`cc`); the host tests use strict flags
  (`-std=c11 -O2 -Wall -Wextra -Werror -pedantic`).
- For cross builds: LLVM/Clang with a LoongArch backend and `ld.lld`. Point the
  Makefile variables `CLANG`, `OBJDUMP` (and `GENFW` for EFI) at your own
  installation; the checked-in defaults are environment-specific, so no fixed
  toolchain location is assumed here.
- `python3` to generate the LATX headers.
- `qemu-system-loongarch64` for the bare-metal and UEFI runs — any recent build
  works; LVZ is not required (see "No LVZ dependency" above). `QLVZ_REPO`,
  `QEMU_LA64` and `QLVZ` are overridable path variables if you use alternate
  binaries.
- A RISC-V OpenSBI `fw_dynamic` image for the LATR OpenSBI-to-S-mode test; set
  `OPEN_SBI_BIN` to wherever your distribution or local build installed it.
- EDK2 `GenFw` to produce PE32+ EFI applications; set `GENFW` to your local path.

Host tests need none of the cross toolchain — only `cc` and `make`.

## Quick start

Run the portable core tests on the host:

```sh
cd loongemu
make            # equivalent to: make test
```

This builds and runs the portable host suites. They now cover all four
translators and the shared core: LBT/paging/TB-cache (`test_loongbt`,
`test_loongbt_cpu`, `test_loongarch_paging`, `test_x86_64_paging`,
`test_tb_cache`, `test_soft_ras`), the x86 VMM helpers (`test_pci_host`,
`test_hyperv`), PVIR/SVE and ARM state (`test_pvir`, `test_sve_decode`,
`test_sve_pvir`, `test_vcache`, `test_arm_state`, `test_lata_context`), the
RISC-V board models (`test_riscv_sbi`, `test_riscv_acpi`, `test_riscv_plic`,
`test_riscv_uart`, `test_riscv_fw`), and LATM decoding. Individual suites:

```sh
make test-cpu        # LBT backend selection and EFLAGS pack/unpack
make test-lapaging   # LoongArch 4 KiB page-table pool / PTE / map / walk
make test-tbc        # translation-block cache
make test-soft-ras   # software return-address stack
make test-pci-host   # PCI host bridge
make test-hyperv     # Hyper-V ABI
make test-pvir       # shared PVIR vector IR
make test-sve-pvir   # AArch64 SVE lowering into PVIR
make test-riscv-sbi  # RISC-V SBI call layer
make test-riscv-acpi # RISC-V ACPI table generation
make test-paging     # x86-64 page walker
make test-libc       # in-tree libc parity against the host libc
make test-libm       # in-tree libm accuracy parity against the host libm
```

At the time of writing all host suites pass. Representative counts:
`loongbt` 181, `loongbt_cpu` 272, `loongarch_paging` 386, `tb_cache` 510,
`soft_ras` 61, `pvir` 222, `sve_decode` 578, `sve_pvir` 435, `arm_state` 9296
— all with zero failures. RISC-V board checks also pass (`sbi` 60, `acpi`
160, `plic` 30, `uart` 23, `fw` 44).

## Building the translation cores

Each translator is built as a freestanding static archive:

```sh
make -C baremetal all   # baremetal/build/liblatx_core.a  (x86-64 translator)
make lata-all           # lata/build/liblata_core.a       (AArch64 translator)
make latr-core          # latr/build/liblatr_core.a       (RISC-V translator)
make lata-probe         # per-file syntax check
make lata-check         # build LATA and verify its external-symbol allowlist
make latr-check         # build LATR and verify its external-symbol allowlist
make test-latm          # LATM decoder + Loongson 3A4000 profile/system/uncore/PCI mock tests
make latm-board-run     # synthetic MIPS64 board: RAM, 16550 + IRQ, FDT, precise exceptions
```

The `check_bare_symbols.sh`, `check_lata_symbols.sh` and LATR allowlist tools
compare each archive's genuinely external symbols with an allowlist, so a
silent dependency on libc/glib/libm cannot creep into a freestanding image.
The LATX archive is currently self-contained; LATA and LATR expect a small
platform layer (`malloc`, `printf`, `qsort`, …). Generated `latr/build/`
outputs are git-ignored and regenerated from `ir1/qemu/*.decode`.

## Bare-metal and UEFI harnesses

These targets build LoongArch images and run them under QEMU, printing results
over the emulated UART:

```sh
# P0: 4 KiB paging, PLV3 page fault, TLBFILL, resume (QEMU -kernel)
make test-p0

# Single translation block, LBT emulated through #INE
make bare-image && make bare-run

# Multi-TB dynamic loop
make tb-loop-image && make tb-loop-run

# PLV3 isolation probe: separate PLV0/PLV3 roots, execute-only guest code,
# VMM state unmapped from PLV3 (QEMU -cpu la464 and max)
make test-plv3-isolation

# Long-mode guest bring-up: EFER, GDT, CR3, CR0, interrupts, shadow paging,
# translated blocks at PLV3 (2 MiB huge TLB, 8-bank ASID, SoftRAS), and a real
# x86-64 kernel snippet (uses the overridable QLVZ QEMU path variable)
make tb-sys-image && make tb-sys-run
make tb-sys-smp-run                # multi-vCPU variant

# LATA, LATM, LATR and LBT smoke tests
make lata-smoke-run
make latm-isa-run
make lbt-smoke-run

# LATR RISC-V: ISA differential against QEMU-RISC-V, system-board diff, boot smoke
make test-latr                     # latr-check + ISA + qemu ref + boot, signature diff
make latr-sys-diff                 # system board vs QEMU-RISC-V (byte-for-byte)
make latr-opensbi-test             # real OpenSBI fw_dynamic -> S-mode + SRST (~18s)

# UEFI applications (EDK2 GenFw + QEMU_EFI.fd)
make efi-selftest          # LBT fallback self-test image, no LBT instructions
make efi-boot              # boot it on a non-LBT core: 17/17 checks
make efi-ir1-smoke         # IR1 decoding on LoongArch
make efi-lvz-probe         # optional LVZ/privileged CSR probe (plain QEMU)
make vmm-efi && make vmm-boot
# Integrated host UEFI -> System VMM; requires x86-64 OVMF CODE and VARS
make vmm-integrated OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
make vmm-integrated-boot OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
```

QEMU runs are time-bounded (the recipes use `timeout` or a fixed sleep and then
kill the guest), and logs are written under `build/`.
The integrated QEMU smoke test checks the handoff, `tb_sys` self-tests and
continued execution of x86 OVMF. It does not establish that OVMF completes
boot or that a guest operating system starts.


## Verification of the two back ends

```sh
make test-loongarch           # objdump: every expected LBT group reaches the object
make test-loongarch-fallback  # objdump: fallback contains no LBT instruction at all
```

These checks guarantee the fallback can run on cores without LBT. Actual
hardware execution of the LBT back end is *not* verified here; that requires a
3A6000-class machine.

## Current state

Three strands are moving in parallel:

- **x86-64 (`tb-sys`)** — running a kernel snippet end to end through LATX and a
  LoongArch shadow page table. Recent work added strict PLV3 isolation (separate
  `PGDH`/`PGDL` roots, W^X code cache, VMM state never mapped at PLV3), a default
  software return-address stack (`option_soft_ras = 1`), 2 MiB (`PS=20`) shadow
  TLB batches, 8 ASID-banked fast-jump roots per vCPU, owner-routed IPIs, and
  A/D compare-exchange restart handling.
- **AArch64 (LATA)** — SVE support is now substantive: a unified guest-memory
  contract with precise Data Aborts and FFR suppression, checked SVE memory
  lowering, predicate-mask chunking over LSX 16 B lanes with LASX fallback, a
  helper-call tier carrying per-TB PVIR state, and bindable/thread-local vcache.
- **RISC-V (LATR)** — a full guest machine is up: Sv39/48/57, CLINT/PLIC/UART,
  ACPI, SBI 2.0 and virtio-mmio block; RVV subsets lower through PVIR to LSX/LASX
  with legality checks; SMP uses per-hart TLS contexts and a locked set-associative
  TB cache. Real OpenSBI boots M-mode → S-mode and shuts down via SBI SRST.
  `make test-latr` and `make latr-opensbi-test` pass.

Work in the tree may be in progress; the host test suites are the stable entry
point. See `target/riscv/latr/DESIGN.md` and `MACHINE.md` for LATR detail.

## Known limitations

- This is a bring-up/testbed tree, not a production hypervisor; there is no
  device model beyond a few stubs (serial, PIT, PCI, APIC).
- The x86 `tb_sys` PLV3 shadow mapping is **not an isolation boundary for
  untrusted guests**. Isolation was hardened — separate `PGDH`/`PGDL` roots, an
  execute-only/W^X code cache, and VMM secrets, stacks and prologue/epilogue
  buffers never mapped at PLV3, checked by `make test-plv3-isolation` — but the
  guest still shares the PLV3 root with a small runtime surface (`g_vcpu_env`,
  jump caches, scratch stack, `pf_table`). Run only trusted test guests, and
  note the software checked-access path does not yet cover every memory
  operation.
- When no suitable host GOP framebuffer is available, the Bochs fallback is
  guest-visible RAM only; it is not a host display output.
- Physical 7A PCI configuration passthrough is disabled unless host read/write
  callbacks are explicitly supplied. The integrated boot keeps them `NULL`;
  regression tests assert the backend stays closed. Enabling it requires a
  separate BAR/DMA/IRQ ownership design plus IOMMU/DMA isolation; the blacklist
  and errata handling alone are not sufficient.
- The Makefiles ship environment-specific toolchain defaults; set `CLANG`,
  `OBJDUMP`, `LD_LLD`, `GENFW`, `QEMU_LA64` / `QLVZ`, `OPEN_SBI_BIN`, … for your
  own machine. No particular install location is assumed by the project.
- QEMU does not execute LBT instructions, so the bare-metal harness emulates
  them; the hardware path is only exercised on real 3A6000-class silicon.
- LATR and LATA SVE are research prototypes. LATR implements RV64IMAFDC plus a
  subset of RVV (VLEN=128 / ELEN=64), but not all of RVV 1.0; there is no real
  HPM event backend and AIA/IMSIC/APLIC are absent. SVE lowering relies on a
  helper-call tier for shapes that are not statically lowerable. See
  `target/riscv/latr/RVV_P1_LEGALITY.md` and `GAPS.md`.
- `build/` output (including generated `latr/build/`) is generated and ignored
  by git; `firmware/` blobs are tracked on purpose and pinned by
  `firmware/SHA256SUMS.txt`.

## License

GPL-2.0 — see [LICENSE](LICENSE). The vendored `latx/capstone_git` and
`latx/upstream` trees carry their own upstream licenses and notices.
