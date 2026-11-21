# LoongEMU

A LoongArch-based binary-translation and virtualization playground. LoongEMU
runs x86-64 (and, experimentally, AArch64) guest code on LoongArch CPUs, and
builds the pieces needed to do it from a freestanding environment: a portable
LBT implementation, a translation-block cache, hardware page tables, and
bare-metal / UEFI harnesses that run under QEMU.

[中文文档](README.zh_cn.md) · [GPL-2.0](LICENSE)

## Overview

The project is a research/testbed tree rather than a single product binary. Its
central question is: *how far can translated guest code run on real LoongArch
hardware, with no host libc and no hypervisor underneath?*

Guest code is decoded and translated by **LATX** (x86-64 → LoongArch),
**LATA** (AArch64 → LoongArch), or **LATM** (MIPS64r2 → LoongArch), all adapted
from upstream QEMU-related sources into self-contained freestanding static
archives. x86 arithmetic flags are
handled through the **LBT** (Loongson Binary Translation) extension. Because
not every LoongArch core implements LBT, LoongEMU provides two back ends behind
one interface:

| Back end | Target | Notes |
| --- | --- | --- |
| Hardware | LoongArch 3A6000-class cores | Real `x86add.w`, `x86mtflag`, `x86settm`, … instructions; compiled with `+lbt`. |
| Portable fallback | 2K1000LA / 2K2000 and QEMU | Same semantics in integer C. QEMU does not execute LBT, so the bare-metal harness emulates it from the `#INE` vector. |

Above that core, the tree explores a thin VMM path: LoongArch 4 KiB hardware
paging in PLV0, translation blocks executed in PLV3 under a shadow page table,
guest page faults delivered through the guest IDT, and a fixed-size
translation-block cache keyed by `(pc, cs_base, cr3, cpl)`.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/loongemu/`, `src/` | Portable, host-testable core: `loongbt` (LBT ops + EFLAGS + IR2 mapping), `tb_cache`, `loongarch_paging`, `x86_64_paging`. |
| `latx/` | LATX x86-64 translator: Capstone-based IR1 decoder, IR2, translator, optimizations, runtime, and minimal libc/libm. |
| `lata/`, `target/arm/lata/` | LATA AArch64 → LoongArch translator core. |
| `latm/`, `target/mips/latm/` | LATM MIPS64r2 → LoongArch translator core (integer, LBT unaligned memory, FPU helpers). |
| `target/i386/`, `target/arm/` | Target-side headers and sources consumed by the translators. |
| `baremetal/bare/` | `-kernel` harnesses: single-TB, multi-TB loop, paging smoke, LATA/LBT/LATM smoke, the synthetic LATM MIPS64 board (`latm_board.c`), and the long-mode `tb_sys` bring-up test. |
| `baremetal/efi/` | UEFI applications: LBT fallback self-test, IR1 decode smoke test, LVZ probe, VMM entry. |
| `baremetal/gen/` | Generated LATX headers (`ir2-opcode.h`, `ir2-name.h`, `format-table.h`, …). |
| `firmware/` | Pinned EDK2 `QEMU_EFI.fd` / `QEMU_VARS.fd` and x86-64 kernel snippet binaries, checked by `SHA256SUMS.txt`. |
| `tools/` | Symbol allowlist checks and an EFI image build helper. |
| `tests/` | Host unit tests for each portable module. |
| `Makefile` | Top-level entry point for host tests and QEMU/EFI targets. |

## Requirements

- A C11 host compiler (`cc`); the host tests use strict flags
  (`-std=c11 -O2 -Wall -Wextra -Werror -pedantic`).
- For cross builds: LLVM/Clang with a LoongArch backend and `ld.lld`.
  The Makefiles default to Homebrew paths such as
  `/opt/homebrew/Cellar/llvm/23.1.5_1/bin/clang`, so override `CLANG`,
  `OBJDUMP` (and `GENFW` for EFI) if your versions differ.
- `python3` to generate the LATX headers.
- `qemu-system-loongarch64` for the bare-metal and UEFI runs.
- QEMU-LVZ for the LBT / hardware-paging harnesses; set `QLVZ` to its binary
  (the Makefile defaults to `~/Documents/QEMU-LVZ/build/qemu-system-loongarch64`).
- EDK2 `GenFw` to produce PE32+ EFI applications (`GENFW ?= /tmp/edk2/BaseTools/Source/C/bin/GenFw`).

Host tests need none of the cross toolchain — only `cc` and `make`.

## Quick start

Run the portable core tests on the host:

```sh
cd loongemu
make            # equivalent to: make test
```

This builds and runs four suites: `test_loongbt`, `test_loongbt_cpu`,
`test_loongarch_paging`, and `test_tb_cache`. Individual suites:

```sh
make test-cpu        # LBT backend selection and EFLAGS pack/unpack
make test-lapaging   # LoongArch 4 KiB page-table pool / PTE / map / walk
make test-tbc        # translation-block cache
make test-paging     # x86-64 page walker
make test-libc       # in-tree libc parity against the host libc
make test-libm       # in-tree libm accuracy parity against the host libm
```

At the time of writing all host suites pass (`loongbt` 181 checks,
`loongbt_cpu` 272, `loongarch_paging` 367, `tb_cache` 510, all with zero
failures).

## Building the translation cores

Both translators are built as freestanding static archives:

```sh
make -C baremetal all   # baremetal/build/liblatx_core.a  (x86-64 translator)
make lata-all           # lata/build/liblata_core.a       (AArch64 translator)
make lata-probe         # per-file syntax check
make lata-check         # build LATA and verify its external-symbol allowlist
make test-latm          # LATM decoder + Loongson 3A4000 profile/system/uncore/PCI mock tests
make latm-board-run     # synthetic MIPS64 board: RAM, 16550 + IRQ, FDT, precise exceptions
```

The `check_bare_symbols.sh` and `check_lata_symbols.sh` tools compare each
archive's genuinely external symbols with an allowlist, so a silent dependency
on libc/glib/libm cannot creep into a freestanding image. The LATX archive is
currently self-contained; the LATA archive expects a small platform layer
(`malloc`, `printf`, `qsort`, …).

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

# Long-mode guest bring-up: EFER, GDT, CR3, CR0, interrupts, shadow paging,
# translated blocks at PLV3, and a real x86-64 kernel snippet (needs QLVZ)
make tb-sys-image && make tb-sys-run

# LATA and LBT smoke tests
make lata-smoke-run
make latm-isa-run
make lbt-smoke-run

# UEFI applications (EDK2 GenFw + QEMU_EFI.fd)
make efi-selftest          # LBT fallback self-test image, no LBT instructions
make efi-boot              # boot it on a non-LBT core: 17/17 checks
make efi-ir1-smoke         # IR1 decoding on LoongArch
make efi-lvz-probe         # privileged/LVZ CSR probe
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

Development is focused on `tb-sys`: running an x86-64 kernel snippet end to end
through LATX translation and a LoongArch shadow page table. Recent commits cover
guest page-table walks, `#PF` delivery through the guest IDT, PLV3 translated
blocks, interrupt/timer injection, `IRETQ`/`SYSRET` and MSR handling, and
loading the kernel's `PT_LOAD` segments. Work in the tree may be in progress;
the host test suites are the stable entry point.

## Known limitations

- This is a bring-up/testbed tree, not a production hypervisor; there is no
  device model beyond a few stubs (serial, PIT, PCI, APIC).
- The x86 `tb_sys` PLV3 shadow mapping is **not an isolation boundary for
  untrusted guests**: guest effective addresses can collide with mappings for
  translated host code and VMM state. Run only trusted test guests. The
  software checked-access path does not yet cover every memory operation.
- When no suitable host GOP framebuffer is available, the Bochs fallback is
  guest-visible RAM only; it is not a host display output.
- Physical 7A PCI configuration passthrough is disabled unless host read/write
  callbacks are explicitly supplied. The integrated boot keeps them `NULL`;
  regression tests assert the backend stays closed. Enabling it requires a
  separate BAR/DMA/IRQ ownership design plus IOMMU/DMA isolation; the blacklist
  and errata handling alone are not sufficient.
- Toolchain paths in the Makefiles are pinned to a Homebrew LLVM layout and
  need overriding on other systems.
- QEMU does not execute LBT instructions, so the bare-metal harness emulates
  them; the hardware path is only exercised on real 3A6000-class silicon.
- `build/` output is generated and ignored by git; `firmware/` blobs are
  tracked on purpose and pinned by `firmware/SHA256SUMS.txt`.

## License

GPL-2.0 — see [LICENSE](LICENSE). The vendored `latx/capstone_git` and
`latx/upstream` trees carry their own upstream licenses and notices.
