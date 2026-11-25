# Host build for the portable LoongEMU pieces.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Iinclude
LBT_SRC := src/loongbt_semantics.c src/loongbt_cpu.c src/loongbt_hw.c src/loongbt_ir2.c

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
CLANG   ?= $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,clang)
ifeq ($(origin AR),default)
AR := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-ar,llvm-ar)
else
AR ?= $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-ar,llvm-ar)
endif
OBJDUMP ?= $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-objdump,llvm-objdump)
OBJCOPY ?= $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-objcopy,llvm-objcopy)
# BSD/macOS has no `timeout` unless coreutils is installed; CI (Debian) does.
TIMEOUT ?= $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)
LLD_PREFIX ?= $(shell brew --prefix lld 2>/dev/null)
LD_LLD   ?= $(if $(LLD_PREFIX),$(LLD_PREFIX)/bin/ld.lld,ld.lld)
QEMU_PREFIX ?= $(shell brew --prefix qemu 2>/dev/null)

.PHONY: all test test-pvir test-fpsr test-riscv-sbi test-riscv-virtio test-riscv-acpi test-riscv-plic test-riscv-uart test-riscv-clint test-riscv-fw test-sve-decode test-sve-pvir test-sve-helper-support test-sve-helper-lifecycle test-vcache test-arm-state test-cpu test-lapaging test-pci-host test-hyperv test-tbc test-soft-ras test-p0 test-plv3-isolation tb-sys-run tb-sys-smp-run vmm-efi vmm-boot vmm-integrated vmm-integrated-boot test-paging test-libc test-libm test-loongarch test-loongarch-fallback efi-selftest efi-boot test-latr latr-core latr-isa-image latr-isa-run latr-riscv-ref latr-check latr-boot-image latr-boot-run latr-sys-ref latr-sys-run latr-sys-diff latr-opensbi-image latr-opensbi-run latr-opensbi-trace latr-opensbi-trace-run latr-opensbi-test test-latm test-latm-decode latm-core latm-isa-image latm-isa-run latm-riscv-ref clean

all: test

build/test_loongbt: tests/test_loongbt.c $(LBT_SRC) include/loongemu/loongbt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaremetal/gen/latx/include tests/test_loongbt.c $(LBT_SRC) -o $@

# Guest-write domain contract (roadmap 3.2): device DMA and CPU stores share one
# critical section, tested with a real second thread.
build/test_guest_write_domain: tests/test_guest_write_domain.c \
                               src/guest_dma.c src/phys_mem.c \
                               include/loongemu/guest_dma.h \
                               include/loongemu/phys_mem.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -Iinclude \
		src/guest_dma.c src/phys_mem.c tests/test_guest_write_domain.c -o $@

build/test_tb_cache: tests/test_tb_cache.c src/tb_cache.c include/loongemu/tb_cache.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/tb_cache.c tests/test_tb_cache.c -o $@

build/test_pvir: tests/test_pvir.c shared/pvir/src/pvir.c \
                 shared/pvir/src/pvir-plan.c \
                 shared/pvir/include/la-pvir.h \
                 shared/pvir/include/pvir-plan.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Itarget/arm/lata/include -Ishared/pvir/include \
	      shared/pvir/src/pvir.c shared/pvir/src/pvir-plan.c \
	      tests/test_pvir.c -o $@

test-pvir: build/test_pvir
	./build/test_pvir

build/test_fpsr: tests/test_fpsr.c target/arm/lata/fpsr.c \
                 target/arm/lata/include/lata-fpsr.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Itarget/arm/lata/include -Ishared/pvir/include \
	      target/arm/lata/fpsr.c tests/test_fpsr.c -o $@

test-fpsr: build/test_fpsr
	./build/test_fpsr

test-pvir-rvv-shapes: build/test_pvir_rvv_shapes
	./build/test_pvir_rvv_shapes

build/test_pvir_rvv_shapes: tests/test_pvir_rvv_shapes.c shared/pvir/src/pvir.c \
                           shared/pvir/include/la-pvir.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I target/arm/lata/include -I shared/pvir/include \
	      shared/pvir/src/pvir.c tests/test_pvir_rvv_shapes.c -o $@

test-riscv-sbi: build/test_riscv_sbi
	./build/test_riscv_sbi

test-riscv-virtio: build/test_riscv_virtio_blk
	./build/test_riscv_virtio_blk

build/test_riscv_virtio_blk: tests/test_riscv_virtio_blk.c \
                             src/riscv_virtio_blk.c src/guest_dma.c \
                             src/phys_mem.c src/mmio_bus.c \
                             include/loongemu/riscv_virtio_blk.h \
                             include/loongemu/guest_dma.h \
                             include/loongemu/phys_mem.h
	@mkdir -p build
	$(CC) $(CFLAGS) tests/test_riscv_virtio_blk.c src/riscv_virtio_blk.c \
	      src/guest_dma.c src/phys_mem.c src/mmio_bus.c -o $@

test-riscv-acpi: build/test_riscv_acpi
	./build/test_riscv_acpi

test-riscv-clint: build/test_riscv_clint
	./build/test_riscv_clint

build/test_riscv_clint: tests/test_riscv_clint.c src/riscv_clint.c \
                        src/mmio_bus.c include/loongemu/riscv_clint.h \
                        include/loongemu/mmio_bus.h
	$(CC) $(CFLAGS) -I include tests/test_riscv_clint.c src/riscv_clint.c \
	      src/mmio_bus.c -o $@

test-riscv-plic: build/test_riscv_plic
	./build/test_riscv_plic

test-riscv-uart: build/test_riscv_uart
	./build/test_riscv_uart

build/test_riscv_uart: tests/test_riscv_uart.c src/uart16550.c src/mmio_bus.c \
                       src/riscv_plic.c include/loongemu/uart16550.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I include \
	      tests/test_riscv_uart.c src/uart16550.c src/mmio_bus.c \
	      src/riscv_plic.c -o $@

test-riscv-fw: build/test_riscv_fw
	./build/test_riscv_fw

build/test_riscv_fw: tests/test_riscv_fw.c src/riscv_fw.c src/riscv_acpi.c \
                     src/ovmf_board.c src/phys_mem.c \
                     include/loongemu/riscv_fw.h include/loongemu/riscv_acpi.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I include \
	      tests/test_riscv_fw.c src/riscv_fw.c src/riscv_acpi.c \
	      src/ovmf_board.c src/phys_mem.c -o $@

build/test_riscv_plic: tests/test_riscv_plic.c src/riscv_plic.c \
                       include/loongemu/riscv_plic.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I include \
	      tests/test_riscv_plic.c src/riscv_plic.c src/mmio_bus.c -o $@

build/test_riscv_acpi: tests/test_riscv_acpi.c src/riscv_acpi.c \
                       include/loongemu/riscv_acpi.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I include \
	      tests/test_riscv_acpi.c src/riscv_acpi.c -o $@

build/test_riscv_sbi: tests/test_riscv_sbi.c src/riscv_sbi.c \
                      include/loongemu/riscv_sbi.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I include \
	      tests/test_riscv_sbi.c src/riscv_sbi.c -o $@

build/test_sve_decode: tests/test_sve_decode.c \
                       target/arm/lata/ir1/sve-decode.c \
                       target/arm/lata/ir1/sve-decode.h \
                       target/arm/lata/include/ir1.h \
                       target/arm/lata/include/ir1-arg.h \
                       shared/pvir/include/la-pvir.h \
                       lata/compat/target/arm/cpu.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-gnu-empty-struct \
	      -Ilata/compat -iquote latx/compat \
	      -Itarget/arm/lata/include -Ishared/pvir/include -Itarget/arm/lata/ir1 \
	      target/arm/lata/ir1/sve-decode.c tests/test_sve_decode.c -o $@

test-sve-decode: build/test_sve_decode
	./build/test_sve_decode

build/test_sve_pvir: tests/test_sve_pvir.c target/arm/lata/ir1/sve-pvir.c \
                     target/arm/lata/ir1/sve-decode.c shared/pvir/src/pvir.c \
                     target/arm/lata/ir1/sve-pvir.h \
                     target/arm/lata/ir1/sve-decode.h \
                     target/arm/lata/include/ir1.h \
                     target/arm/lata/include/ir1-arg.h \
                     shared/pvir/include/la-pvir.h \
                     target/arm/lata/include/env.h \
                     lata/compat/target/arm/cpu.h \
                     latx/runtime/cpu_features.c \
                     latx/compat/qemu/cpu-features.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-gnu-empty-struct -I. \
	      -Ilata/compat -iquote latx/compat \
	      -Itarget/arm/lata/include -Ishared/pvir/include -Itarget/arm/lata/ir1 \
	      shared/pvir/src/pvir.c target/arm/lata/ir1/sve-decode.c \
	      target/arm/lata/ir1/sve-pvir.c latx/runtime/cpu_features.c \
	      tests/test_sve_pvir.c -o $@

test-sve-pvir: build/test_sve_pvir
	./build/test_sve_pvir

# No decodable SVE encoding may fall through all three lowering tiers into a
# silent no-op; see tests/test_sve_helper_support.c.
build/test_sve_helper_support: tests/test_sve_helper_support.c \
                               target/arm/lata/translator/sve-helper-support.c \
                               target/arm/lata/ir1/sve-pvir.c \
                               target/arm/lata/ir1/sve-decode.c \
                               shared/pvir/src/pvir.c \
                               target/arm/lata/translator/sve-lasx-lower.h \
                               target/arm/lata/ir1/sve-pvir.h \
                               target/arm/lata/ir1/sve-decode.h \
                               shared/pvir/include/la-pvir.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-gnu-empty-struct -I. \
	      -Ilata/compat -iquote latx/compat \
	      -Itarget/arm/lata/include -Ishared/pvir/include -Itarget/arm/lata/ir1 \
	      -Itarget/arm/lata/translator \
	      shared/pvir/src/pvir.c target/arm/lata/ir1/sve-decode.c \
	      target/arm/lata/ir1/sve-pvir.c latx/runtime/cpu_features.c \
	      target/arm/lata/translator/sve-helper-support.c \
	      tests/test_sve_helper_support.c -o $@

# Helper-descriptor lifetime: chunks keyed by TranslationBlock, released at
# flush/teardown instead of growing until context destruction.
build/test_sve_helper_lifecycle: tests/test_sve_helper_lifecycle.c \
                                 target/arm/lata/translator/sve-helper.c \
                                 target/arm/lata/translator/sve-helper-support.c \
                                 target/arm/lata/ir1/sve-pvir.c \
                                 target/arm/lata/ir1/sve-decode.c \
                                 shared/pvir/src/pvir.c \
                                 target/arm/lata/mem.c \
                                 target/arm/lata/translator/sve-lasx-lower.h \
                                 target/arm/lata/include/lata.h \
                                 lata/compat/target/arm/tcg/translate.h
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-deprecated-non-prototype \
	      -DLATA_ENABLE_TLS -pthread -DTARGET_AARCH64 -DCONFIG_LATA \
	      -DCONFIG_LATA_STANDALONE -Wno-gnu-empty-struct -Wno-macro-redefined \
	      -Wno-unused-parameter \
	      -Ilata/compat -Ilatx/compat -I. -Itarget/arm/lata/include -Ishared/pvir/include \
	      -Ishared/backend/loongarch/include \
	      -Itarget/arm/lata -Itarget/arm/lata/translator -Itarget/arm/lata/ir1 \
	      -Ilatx/upstream/include \
	      shared/pvir/src/pvir.c target/arm/lata/ir1/sve-decode.c \
	      target/arm/lata/ir1/sve-pvir.c latx/runtime/cpu_features.c \
	      target/arm/lata/mem.c target/arm/lata/translator/sve-helper.c \
	      target/arm/lata/translator/sve-helper-support.c \
	      tests/test_sve_helper_lifecycle.c -o $@

test-sve-helper-lifecycle: build/test_sve_helper_lifecycle
	./build/test_sve_helper_lifecycle

test-sve-helper-support: build/test_sve_helper_support
	./build/test_sve_helper_support

build/test_vcache: tests/test_vcache.c lata/vcache.c \
                   target/arm/lata/include/lata-vcache.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-gnu-empty-struct \
	      -Itarget/arm/lata/include -Ishared/pvir/include \
	      lata/vcache.c tests/test_vcache.c -o $@

test-vcache: build/test_vcache
	./build/test_vcache

build/test_arm_state: tests/test_arm_state.c \
                      target/arm/lata/include/env.h \
                      lata/compat/target/arm/cpu.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ilata/compat -Ilatx/compat \
	      -Itarget/arm/lata/include -Ishared/pvir/include tests/test_arm_state.c -o $@

test-arm-state: build/test_arm_state
	./build/test_arm_state

build/test_lata_context: tests/test_lata_context.c lata/vcache.c \
                         target/arm/lata/mem.c \
                         target/arm/lata/ftemp-snapshot.c \
                         target/arm/lata/include/lata.h \
                         target/arm/lata/include/lata-mem.h \
                         target/arm/lata/include/lata-vcache.h \
                         lata/compat/target/arm/cpu.h
	@mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DLATA_ENABLE_TLS -pthread \
	      -DTARGET_AARCH64 -DCONFIG_LATA -DCONFIG_LATA_STANDALONE \
	      -Wno-gnu-empty-struct -Wno-macro-redefined -Wno-unused-parameter \
	      -Ilata/compat -Ilatx/compat -I. -Itarget/arm/lata/include -Ishared/pvir/include \
	      -Ishared/backend/loongarch/include \
	      -Itarget/arm/lata -Ilatx/upstream/include \
	      lata/vcache.c target/arm/lata/mem.c \
	      target/arm/lata/ftemp-snapshot.c \
	      tests/test_lata_context.c -o $@

test-lata-context: build/test_lata_context
	./build/test_lata_context

test-tbc: build/test_tb_cache
	./build/test_tb_cache

build/test_soft_ras: tests/test_soft_ras.c src/soft_ras.c include/loongemu/soft_ras.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/soft_ras.c tests/test_soft_ras.c -o $@

test-soft-ras: build/test_soft_ras
	./build/test_soft_ras

build/test_lata_dispatch: tests/test_lata_dispatch.c src/lata_dispatch.c \
                          src/tb_cache.c include/loongemu/lata_dispatch.h \
                          include/loongemu/tb_cache.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/lata_dispatch.c src/tb_cache.c tests/test_lata_dispatch.c -o $@

test-lata-dispatch: build/test_lata_dispatch
	./build/test_lata_dispatch



build/test_ir2_peephole: tests/test_ir2_peephole.c \
                        shared/backend/loongarch/ir2-peephole.c \
                        shared/backend/loongarch/include/ir2.h \
                        shared/backend/loongarch/include/la-ir2.h \
                        shared/backend/loongarch/include/ir2-sideeffect.h \
                        shared/perf/src/perf.c \
                        shared/perf/include/loongemu-perf.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-unused-function \
	      -DLOONGEMU_PERF -DLOONGEMU_IR2_PEEPHOLE=1 \
	      -Ishared/backend/loongarch/include -Ishared/perf/include \
	      shared/backend/loongarch/ir2-peephole.c shared/perf/src/perf.c \
	      tests/test_ir2_peephole.c -o $@


test: build/test_loongbt build/test_loongbt_cpu build/test_loongarch_paging build/test_x86_64_paging \
      build/test_ir2_peephole build/test_pci_host build/test_hyperv build/test_tb_cache build/test_soft_ras build/test_pvir build/test_sve_decode build/test_sve_pvir build/test_vcache build/test_arm_state build/test_lata_context \
      build/test_latm_decode build/test_latm_insn_map build/test_riscv_virtio_blk build/test_lata_dispatch \
      build/test_riscv_sbi build/test_riscv_acpi build/test_riscv_plic \
      build/test_riscv_uart build/test_riscv_fw \
      build/test_fpsr \
      build/test_sve_helper_support build/test_sve_helper_lifecycle \
      build/test_lata_nzcv build/test_ir2_lbt_opcodes build/test_le_recovery_abi \
      build/test_softfloat_fma build/test_guest_write_domain
	./build/test_ir2_peephole
	./build/test_loongbt
	./build/test_loongbt_cpu
	./build/test_loongarch_paging
	./build/test_x86_64_paging
	./build/test_pci_host
	./build/test_tb_cache
	./build/test_soft_ras
	./build/test_hyperv
	./build/test_pvir
	./build/test_fpsr
	./build/test_riscv_sbi
	./build/test_riscv_acpi
	./build/test_riscv_plic
	./build/test_riscv_uart
	./build/test_riscv_fw
	./build/test_sve_decode
	./build/test_sve_pvir
	./build/test_sve_helper_support
	./build/test_sve_helper_lifecycle
	./build/test_riscv_virtio_blk
	./build/test_vcache
	./build/test_arm_state
	./build/test_lata_context
	./build/test_latm_decode
	./build/test_latm_insn_map
	./build/test_lata_dispatch
	./build/test_lata_nzcv
	./build/test_ir2_lbt_opcodes
	./build/test_le_recovery_abi
	./build/test_softfloat_fma
	./build/test_guest_write_domain

# Back-end selection and the EFLAGS pack/unpack bridge.  Kept separate from
# test_loongbt so a failure points at the glue rather than the semantics.
build/test_loongbt_cpu: tests/test_loongbt_cpu.c $(LBT_SRC) include/loongemu/loongbt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaremetal/gen/latx/include tests/test_loongbt_cpu.c $(LBT_SRC) -o $@

test-cpu: build/test_loongbt_cpu
	./build/test_loongbt_cpu

# LoongArch 4 KiB hardware page tables: pool, PTE encoding, map/walk.
build/test_loongarch_paging: tests/test_loongarch_paging.c src/loongarch_paging.c \
                             include/loongemu/loongarch_paging.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/loongarch_paging.c tests/test_loongarch_paging.c -o $@

test-lapaging: build/test_loongarch_paging
	./build/test_loongarch_paging

build/test_pci_host: tests/test_pci_host.c src/pci_host.c src/phys_mem.c \
                     include/loongemu/pci_host.h include/loongemu/phys_mem.h \
                     include/loongemu/acpi_tables.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/pci_host.c src/phys_mem.c tests/test_pci_host.c -o $@

test-pci-host: build/test_pci_host
	./build/test_pci_host

# AArch64 NZCV / condition reference (see tests/test_lata_nzcv.c): the software
# path LATA needs on hosts without LBT has to match this before it is wired in.
# Vendored softfloat FMA probe: the LATR FMA work depends on it, and the fused
# cancellation cases are currently known-broken (see the file header).
SOFTFLOAT_INC := latx/upstream/include
build/test_softfloat_fma: tests/test_softfloat_fma.c latx/upstream/fpu/softfloat.c \
                          $(SOFTFLOAT_INC)/fpu/softfloat.h
	@mkdir -p build
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
	      -Wno-sign-compare -Wno-implicit-fallthrough \
	      -I $(SOFTFLOAT_INC) -I latx/compat -I latx/compat/include -I include \
	      -include stdint.h -include stdbool.h -D__WORDSIZE=64 \
	      tests/test_softfloat_fma.c latx/upstream/fpu/softfloat.c -lm -o $@

# Shared recovery ABI: row schema plus the one fail-closed decision function.
build/test_le_recovery_abi: tests/test_le_recovery_abi.c \
                            shared/backend/loongarch/include/le-recovery.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I shared/backend/loongarch/include \
	      tests/test_le_recovery_abi.c -o $@

# LBT-opcode classifier + drift check against the enum it is generated from.
IR2_BACKEND_INC := shared/backend/loongarch/include
build/test_ir2_lbt_opcodes: tests/test_ir2_lbt_opcodes.c $(IR2_BACKEND_INC)/ir2-lbt-opcodes.h $(IR2_BACKEND_INC)/ir2-opcode.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I $(IR2_BACKEND_INC) \
	      -DIR2_OPCODE_HEADER='"$(IR2_BACKEND_INC)/ir2-opcode.h"' \
	      -DIR2_LBT_HEADER='"$(IR2_BACKEND_INC)/ir2-lbt-opcodes.h"' \
	      tests/test_ir2_lbt_opcodes.c -o $@

build/test_lata_nzcv: tests/test_lata_nzcv.c
	@mkdir -p build
	$(CC) $(CFLAGS) tests/test_lata_nzcv.c -o $@

build/test_hyperv: tests/test_hyperv.c src/hyperv.c include/loongemu/hyperv.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/hyperv.c tests/test_hyperv.c -o $@

test-hyperv: build/test_hyperv
	./build/test_hyperv

# ---- P0: 4 KiB paging, PLV3 page fault, PLV0 walk + TLBFILL, resume ----
P0_SRC := baremetal/bare/paging_smoke.c src/loongarch_paging.c

build/p0_paging.elf: $(P0_SRC) include/loongemu/loongarch_paging.h
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O1 -mno-lsx -mno-lasx -Iinclude \
	         -c baremetal/bare/paging_smoke.c -o build/p0_paging.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O1 -mno-lsx -mno-lasx -Iinclude \
	         -c src/loongarch_paging.c -o build/p0_loongarch_paging.o
	$(LD_LLD) -T baremetal/bare/bare.lds -e _start \
	   build/p0_paging.o build/p0_loongarch_paging.o -o $@

test-p0: build/p0_paging.elf
	@rm -f build/p0.log
	@timeout 20 qemu-system-loongarch64 -M virt -cpu max -m 512M \
	   -kernel build/p0_paging.elf -display none -monitor none \
	   -serial file:build/p0.log -no-reboot >/dev/null 2>&1 || true
	@cat build/p0.log
	@grep -q 'p0: P0-OK' build/p0.log && echo "  P0: page fault serviced, execution resumed" || \
	  { echo "P0 REGRESSION"; exit 1; }
	@grep -q 'value=0x1234abcd5678ef90' build/p0.log || \
	  { echo "P0 WRONG DATA"; exit 1; }

# ---- P0 PLV3 Isolation Probe (non-LVZ 2K3000/3B6000M compatible) ----
PLV3_ISO_SRC := baremetal/bare/plv3_isolation_probe.c src/loongarch_paging.c

build/plv3_isolation_probe.elf: $(PLV3_ISO_SRC) include/loongemu/loongarch_paging.h baremetal/bare/bare.lds
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -fno-stack-protector -O2 -mno-lsx -mno-lasx -Iinclude \
	         -c baremetal/bare/plv3_isolation_probe.c -o build/plv3_isolation_probe.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -fno-stack-protector -O2 -mno-lsx -mno-lasx -Iinclude \
	         -c src/loongarch_paging.c -o build/plv3_iso_loongarch_paging.o
	$(LD_LLD) -T baremetal/bare/bare.lds -e _start \
	   build/plv3_isolation_probe.o build/plv3_iso_loongarch_paging.o -o $@

QEMU_LA64 ?= $(shell command -v qemu-system-loongarch64 || echo $(QLVZ_REPO))

test-plv3-isolation: build/plv3_isolation_probe.elf
	@for cpu in la464 max; do \
	  echo "=== plv3-iso (CPU=$$cpu) ==="; \
	  env QEMU_BIN="$(QEMU_LA64)" QEMU_KERNEL="build/plv3_isolation_probe.elf" \
	      QEMU_LOG="build/plv3_iso_$$cpu.log" QEMU_DONE="plv3-iso: done" \
	      QEMU_TIMEOUT=20 QEMU_CPU="$$cpu" \
	      bash scripts/run_qemu_lata.sh; \
	  cat "build/plv3_iso_$$cpu.log"; \
	  grep -qa 'plv3-iso: done' "build/plv3_iso_$$cpu.log" || exit 1; \
	  grep -qa 'fails=0000000000000000 ok' "build/plv3_iso_$$cpu.log" || exit 1; \
	done

# This host's clang defaults to an SDK path that does not exist; xcrun knows the
# real one.  Empty on a machine where the default already resolves.
HOST_SYSROOT := $(shell xcrun --show-sdk-path 2>/dev/null | sed 's/^/-isysroot /')

build/test_x86_64_paging: tests/test_x86_64_paging.c src/x86_64_paging.c include/loongemu/x86_64_paging.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/x86_64_paging.c tests/test_x86_64_paging.c -o $@

test-paging: build/test_x86_64_paging
	./build/test_x86_64_paging

build/test_arm64_paging: tests/test_arm64_paging.c src/arm64_paging.c include/loongemu/arm64_paging.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(HOST_SYSROOT) src/arm64_paging.c tests/test_arm64_paging.c -o $@

# ARMv8 Stage-1 4 KiB walker.  Pure host test: the walker takes a memory
# callback, so the descriptor interpretation is checked without a machine.
test-arm64-paging: build/test_arm64_paging
	./build/test_arm64_paging

# Assemble the hardware back end for LoongArch with the LBT extension enabled
# and prove the expected instructions really reach the object.
build/loongbt_hw_lbt.o: src/loongbt_hw.c include/loongemu/loongbt.h
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -Xclang -target-feature -Xclang +lbt -DLOONGBT_ENABLE_HW=1 -O2 -Iinclude -c src/loongbt_hw.c -o $@

test-loongarch: build/loongbt_hw_lbt.o
	@for m in 'x86add\.w' 'x86adc\.d' 'x86sub\.w' 'x86sbc\.b' 'x86and\.h' 'x86or\.d' \
	          'x86xor\.b' 'x86mul\.wu' 'x86rotl\.w' 'x86rotr\.d' 'x86rcl\.h' \
	          'x86rcr\.w' 'x86sll\.d' 'x86srl\.b' 'x86sra\.w' 'x86inc\.d' \
	          'x86dec\.b' 'x86mtflag' 'x86mfflag' 'x86mftop' 'x86mttop' \
	          'x86inctop' 'x86dectop' 'x86settm' 'x86clrtm' 'setx86j' \
	          'setx86loope' 'setx86loopne'; do \
	  $(OBJDUMP) -d $< | grep -qE "\\b$$m\\b" || { echo "MISSING $$m"; exit 1; }; \
	done; echo "  hardware backend: all LBT groups present"

# The fallback must contain no LBT instruction at all: that is what lets it run
# on cores without the extension (2K1000LA / 2K2000).
build/loongbt_fallback_la.o: src/loongbt_semantics.c src/loongbt_cpu.c src/loongbt_hw.c include/loongemu/loongbt.h
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -O2 -Iinclude -c src/loongbt_semantics.c -o build/loongbt_semantics_la.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -O2 -Iinclude -c src/loongbt_cpu.c -o build/loongbt_cpu_la.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -O2 -Iinclude -c src/loongbt_hw.c -o build/loongbt_hw_la.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -O2 -Iinclude -Ibaremetal/gen/latx/include -c src/loongbt_ir2.c -o build/loongbt_ir2_la.o

test-loongarch-fallback: build/loongbt_fallback_la.o
	@if $(OBJDUMP) -d build/loongbt_semantics_la.o build/loongbt_cpu_la.o build/loongbt_hw_la.o build/loongbt_ir2_la.o \
	    | grep -qE '\b(x86[a-z.]+|setx86[a-z]+)\b'; then \
	  echo "FALLBACK CONTAINS LBT INSTRUCTIONS"; exit 1; \
	fi; echo "  fallback: no LBT instructions (2K1000LA/2K2000 safe)"

clean:
	rm -rf build

# ---- freestanding EFI self-test of the LoongBT fallback (no LBT anywhere) ----
EFI_SRC := baremetal/efi/lbt_selftest.c src/loongbt_semantics.c src/loongbt_cpu.c \
           src/loongbt_hw.c src/loongbt_ir2.c
EFI_INC := -Iinclude -Ibaremetal/gen/latx/include
GENFW   ?= /tmp/edk2/BaseTools/Source/C/bin/GenFw

build/lbt_selftest.o: $(EFI_SRC) include/loongemu/loongbt.h
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize $(EFI_INC) -c baremetal/efi/lbt_selftest.c -o $@
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize $(EFI_INC) -c src/loongbt_semantics.c -o build/efi_loongbt_semantics.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize $(EFI_INC) -c src/loongbt_cpu.c -o build/efi_loongbt_cpu.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize $(EFI_INC) -c src/loongbt_hw.c -o build/efi_loongbt_hw.o
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize $(EFI_INC) -c src/loongbt_ir2.c -o build/efi_loongbt_ir2.o

build/lbt_selftest.elf: build/lbt_selftest.o build/efi_loongbt_semantics.o \
                        build/efi_loongbt_cpu.o build/efi_loongbt_hw.o build/efi_loongbt_ir2.o
	@printf 'ENTRY(efi_main)\nSECTIONS { . = 0x1000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n' > build/efi.lds
	$(LD_LLD) --emit-relocs -T build/efi.lds -e efi_main \
	   build/lbt_selftest.o build/efi_loongbt_semantics.o build/efi_loongbt_cpu.o \
	   build/efi_loongbt_hw.o build/efi_loongbt_ir2.o -o $@

build/lbt_selftest.efi: build/lbt_selftest.elf
	$(GENFW) -o $@ -e UEFI_APPLICATION $<
	@$(OBJDUMP) --file-headers $@ | grep -E 'Machine|Subsystem' || true
	@if $(OBJDUMP) -d build/lbt_selftest.elf | grep -qE '\b(x86[a-z.]+|setx86[a-z]+)\b'; then \
	  echo "EFI IMAGE CONTAINS LBT INSTRUCTIONS"; exit 1; \
	fi; echo "  efi image: no LBT instructions"

efi-selftest: build/lbt_selftest.efi

efi-boot: efi-selftest
	@rm -rf build/esp && mkdir -p build/esp/EFI/BOOT
	@cp build/lbt_selftest.efi build/esp/EFI/BOOT/BOOTLOONGARCH64.EFI
	@rm -f build/efi-serial.log
	@( qemu-system-loongarch64 -M virt -cpu la464 -m 2G \
	     -bios firmware/QEMU_EFI.fd \
	     -drive id=esp,file=fat:rw:build/esp,format=raw,if=none \
	     -device virtio-blk-pci,drive=esp \
	     -display none -monitor none -serial file:build/efi-serial.log -no-reboot & \
	   echo $$! > build/qemu.pid )
	@sleep 40; kill $$(cat build/qemu.pid) 2>/dev/null; true
	@grep -aE 'latx-efi|BdsDxe' build/efi-serial.log | head -20
	@grep -aq 'latx-efi: done' build/efi-serial.log
	@grep -aq 'latx-efi: lbt=0x0000000000000000 checks=0x0000000000000011 fails=0x0000000000000000 ok' build/efi-serial.log
	@echo "  efi boot: fallback ran on a non-LBT core, 17/17 checks" 

# ---- in-tree libm replacement: accuracy parity against the host libm ----
LIBM_RENAMES := -Dsin=latx_sin -Dcos=latx_cos -Dsincos=latx_sincos \
                -Datan=latx_atan -Datan2=latx_atan2 -Dpow=latx_pow \
                -Dlog2=latx_log2 -Dlogb=latx_logb -Dscalbn=latx_scalbn \
                -Dilogb=latx_ilogb -Dlog1p=latx_log1p -Dfabs=latx_fabs \
                -Dfloor=latx_floor

build/libm-host/.stamp: $(wildcard latx/compat/libm/*.c) $(wildcard latx/compat/libm/*.h)
	@rm -rf build/libm-host && mkdir -p build/libm-host
	@for f in latx/compat/libm/*.c; do \
	  cc -O2 -std=c11 -I latx/compat/libm $(LIBM_RENAMES) -c "$$f" -o "build/libm-host/$$(basename $$f .c).o" || exit 1; \
	done
	@touch $@

build/test_libm_parity: tests/test_libm_parity.c build/libm-host/.stamp
	cc -O2 -std=c11 $< build/libm-host/*.o -o $@ -lm

test-libm: build/test_libm_parity
	./build/test_libm_parity

# ---- in-tree libc: behaviour parity against the host libc ----
LIBC_RENAMES := -Dmemcpy=latx_memcpy -Dmemmove=latx_memmove -Dmemset=latx_memset \
                -Dmemcmp=latx_memcmp -Dmemchr=latx_memchr -Dstrlen=latx_strlen \
                -Dstrcmp=latx_strcmp -Dstrncmp=latx_strncmp -Dstrcpy=latx_strcpy \
                -Dstrncpy=latx_strncpy -Dstrcat=latx_strcat -Dstrchr=latx_strchr \
                -Dstrrchr=latx_strrchr -Dstrdup=latx_strdup \
                -Dsnprintf=latx_snprintf -Dsprintf=latx_sprintf \
                -Dvsnprintf=latx_vsnprintf -Dprintf=latx_printf \
                -Dfprintf=latx_fprintf -Dfputs=latx_fputs -Dputs=latx_puts \
                -Dputchar=latx_putchar -Dfflush=latx_fflush \
                -Dmalloc=latx_malloc -Dcalloc=latx_calloc -Drealloc=latx_realloc \
                -Dfree=latx_free -Dabort=latx_abort -Dexit=latx_exit \
                -Dqemu_log=latx_qemu_log

build/libc-host/.stamp: latx/compat/libc_min.c latx/compat/miniglib.c \
                        latx/compat/*.h latx/compat/qemu/*.h
	@rm -rf build/libc-host && mkdir -p build/libc-host
	cc -O2 -std=c11 -fno-builtin -I latx/compat $(LIBC_RENAMES) \
	   -c latx/compat/libc_min.c -o build/libc-host/libc_min.o
	cc -O2 -std=c11 -fno-builtin -I latx/compat $(LIBC_RENAMES) \
	   -c latx/compat/miniglib.c -o build/libc-host/miniglib.o
	@touch $@

build/test_minilibc: tests/test_minilibc.c build/libc-host/.stamp
	cc -O2 -std=c11 $< build/libc-host/*.o -o $@

test-libc: build/test_minilibc
	./build/test_minilibc

# ---- EFI smoke test for IR1 decoding on LoongArch ----
build/ir1_smoke.o: baremetal/efi/ir1_smoke.c
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -DTARGET_X86_64 -DCONFIG_LATX \
	         -DCONFIG_LATX_FLAG_REDUCTION -DCONFIG_LATX_CAPSTONE_GIT \
	         -DCAPSTONE_HAS_X86 -DCAPSTONE_USE_SYS_DYN_MEM=1 -D__thread= \
	         -I latx/compat -I latx/include -I latx -I latx/qemu -I baremetal/gen/latx/include \
	         -I latx/capstone_git/include \
	         -I latx/upstream/include -isystem $(shell $(CLANG) -print-resource-dir)/include \
	         -include latx/compat/latx_bare_prelude.h \
	         -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
	         -c $< -o $@

build/ir1_smoke.elf: build/ir1_smoke.o baremetal/build/liblatx_core.a
	@printf 'ENTRY(efi_main)\nSECTIONS { . = 0x1000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n' > build/ir1_smoke.lds
	$(LD_LLD) --emit-relocs -T build/ir1_smoke.lds -e efi_main \
	   build/ir1_smoke.o baremetal/build/liblatx_core.a -o $@

build/ir1_smoke.efi: build/ir1_smoke.elf
	$(GENFW) -o $@ -e UEFI_APPLICATION $<

efi-ir1-smoke: build/ir1_smoke.efi
	@rm -rf build/esp_ir1 && mkdir -p build/esp_ir1/EFI/BOOT
	@cp build/ir1_smoke.efi build/esp_ir1/EFI/BOOT/BOOTLOONGARCH64.EFI
	@rm -f build/ir1_smoke.log
	@( qemu-system-loongarch64 -M virt -cpu la464 -m 2G \
	     -bios firmware/QEMU_EFI.fd \
	     -drive id=esp,file=fat:rw:build/esp_ir1,format=raw,if=none \
	     -device virtio-blk-pci,drive=esp \
	     -display none -monitor none -serial file:build/ir1_smoke.log -no-reboot & \
	   echo $$! > build/qemu_ir1.pid )
	@sleep 45; kill $$(cat build/qemu_ir1.pid) 2>/dev/null; true
	@grep -aE 'ir1-smoke|Exception|ERA' build/ir1_smoke.log | head -30

# ---- LVZ / privileged CSR probe ----
build/lvz_probe.o: baremetal/efi/lvz_probe.c
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
	         -c $< -o $@

build/lvz_probe.elf: build/lvz_probe.o
	@printf 'ENTRY(efi_main)\nSECTIONS { . = 0x1000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n' > build/lvz_probe.lds
	$(LD_LLD) --emit-relocs -T build/lvz_probe.lds -e efi_main build/lvz_probe.o -o $@

build/lvz_probe.efi: build/lvz_probe.elf
	$(GENFW) -o $@ -e UEFI_APPLICATION $<

efi-lvz-probe: build/lvz_probe.efi
	@rm -rf build/esp_lvz && mkdir -p build/esp_lvz/EFI/BOOT
	@cp build/lvz_probe.efi build/esp_lvz/EFI/BOOT/BOOTLOONGARCH64.EFI
	@rm -f build/lvz_probe.log
	@( qemu-system-loongarch64 -M virt -cpu la464 -m 2G \
	     -bios firmware/QEMU_EFI.fd \
	     -drive id=esp,file=fat:rw:build/esp_lvz,format=raw,if=none \
	     -device virtio-blk-pci,drive=esp \
	     -display none -monitor none -serial file:build/lvz_probe.log -no-reboot & \
	   echo $$! > build/qemu_lvz.pid )
	@sleep 40; kill $$(cat build/qemu_lvz.pid) 2>/dev/null; true
	@grep -aE 'lvz-probe|probe:' build/lvz_probe.log | head -40

# ---- bare-metal LoongArch execution harness ----
# LATA_FORCE_NO_LBT=1 builds the ISA image with the software NZCV path selected,
# so the non-LBT code can be exercised on a host that has LBT. Normal builds are
# unaffected.
ifeq ($(LATA_FORCE_NO_LBT),1)
LATA_BARE_EXTRA_CFLAGS := -DLATA_FORCE_NO_LBT
else
LATA_BARE_EXTRA_CFLAGS :=
endif
LATA_BARE_CFLAGS := --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
               -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
               -DTARGET_AARCH64 -DCONFIG_LATA -DCONFIG_LATA_TU -DCONFIG_LATA_STANDALONE \
               -D__WORDSIZE=64 -D__thread= $(LATA_BARE_EXTRA_CFLAGS) \
               -I lata/compat -I . -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
               -I shared/backend/loongarch/include \
               -I latx/compat -I latx/include -I latx/upstream/include \
               -isystem $(shell $(CLANG) -print-resource-dir)/include

# ---- bare-metal RISC-V -> LoongArch translator smoke ----
LATR_BARE_CFLAGS := --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
               -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
               -DTARGET_RISCV64 -DCONFIG_LATR -DCONFIG_LATR_STANDALONE \
               -DCONFIG_LATX -D__WORDSIZE=64 -D__thread= \
               -I latr/compat -I target/riscv/latr/include -I . \
               -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
               -I shared/backend/loongarch/include \
               -I shared/perf/include \
               -I latx/compat -I latx/compat/include -I latx/upstream/include \
               -I include \
               -isystem $(shell $(CLANG) -print-resource-dir)/include
ifeq ($(PERF),1)
LATR_BARE_CFLAGS += -DLOONGEMU_PERF
endif


build/latr_isa_test.o: baremetal/bare/latr_isa_test.c \
                       target/riscv/latr/include/riscv-env.h \
                       target/riscv/latr/include/latr.h \
                       target/riscv/latr/include/mmu.h \
                       target/riscv/latr/include/translate.h \
                       tests/latr_isa_diff_vectors.h \
                       build/latr_rand_vectors.h
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -I build -c $< -o $@

build/latr_miniglib.o: latx/compat/miniglib.c
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -c $< -o $@

build/latr_libc_min.o: latx/compat/libc_min.c
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -c $< -o $@

latr-core:
	$(MAKE) -C latr all PERF=$(PERF)

build/bare_latr_isa.elf: build/latr_isa_test.o build/latr_miniglib.o \
                         build/latr_libc_min.o latr-core baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_isa_test.o build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

build/latr_system_guest.elf: tests/latr_system_guest.c tests/latr_system_guest.ld
	@mkdir -p build
	$(CLANG) --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64 \
	         -mcmodel=medany -ffreestanding -nostdlib -fno-builtin -O2 \
	         -mno-relax -c $< -o build/latr_system_guest.o
	$(LD_LLD) -T tests/latr_system_guest.ld --no-relax \
	         -o $@ build/latr_system_guest.o

build/latr_system_guest_elf.c: build/latr_system_guest.elf
	@python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); \
print("const unsigned char latr_system_guest_elf[] = {" + ",".join(str(b) for b in d) + "};"); \
print("const unsigned int latr_system_guest_elf_len = %d;" % len(d))' $< > $@

latr-sys-ref: build/latr_system_guest.elf
	@rm -f build/latr_system_guest_qemu.log
	@timeout 15 $(QEMU_RISCV64) -M virt -cpu rv64 -m 128M -bios none \
	     -kernel $< -display none -monitor none \
	     -serial file:build/latr_system_guest_qemu.log -no-reboot 2>/dev/null || true
	@grep -a 'SYS:' build/latr_system_guest_qemu.log | tail -1

# Two-hart guest-level SMP scenario: real machine, real generated code, real
# CLINT/WFI/FENCE.I. Takes the place of the guest-level half the host harness
# cannot run (it has no LoongArch execution).
# Fault differential: one guest image, two engines (LATR machine vs QEMU).
latr-fault-image: build/bare_latr_fault.elf

build/latr_fault_guest.elf: baremetal/bare/latr_fault_guest.S
	@mkdir -p build
	$(CLANG) --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 \
	         -mno-relax -ffreestanding -nostdlib -fuse-ld=lld \
	         -Wl,--no-relax -Wl,-Ttext=0x80000000 $< -o $@

build/latr_fault_guest.bin: build/latr_fault_guest.elf
	$(OBJCOPY) -O binary $< $@

build/latr_fault_blob.o: baremetal/bare/latr_fault_blob.S build/latr_fault_guest.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -c $< -o $@

build/bare_latr_fault.elf: baremetal/bare/latr_fault_test.c \
                           build/latr_fault_blob.o latr-core \
                           build/latr_miniglib.o build/latr_libc_min.o \
                           baremetal/bare/bare.lds
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -c $< -o build/latr_fault_test.o
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_fault_test.o build/latr_fault_blob.o \
	   build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

latr-fault-run: latr-fault-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_fault.elf" \
	     QEMU_LOG="build/latr_fault.log" QEMU_DONE="latr-fault: done" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a "latr-fault" build/latr_fault.log | head -6
	@grep -qa "latr-fault: PASS" build/latr_fault.log

latr-fault-ref: build/latr_fault_guest.elf
	@command -v $(QEMU_RISCV64) >/dev/null 2>&1 || { \
	     echo "latr-fault-ref: $(QEMU_RISCV64) is not installed or not on PATH"; \
	     exit 1; }
	rm -f build/latr_fault_ref.log
	$(if $(TIMEOUT),$(TIMEOUT) 20,) $(QEMU_RISCV64) -M virt -cpu max -m 128M \
	     -bios none -kernel $< -display none -monitor none \
	     -serial file:build/latr_fault_ref.log -no-reboot 2>/dev/null || true
	grep -aE 'latr-fault-diff=' build/latr_fault_ref.log

latr-fault-diff:
	@rm -f build/latr_fault.log build/latr_fault_ref.log
	@$(MAKE) --no-print-directory latr-fault-run latr-fault-ref \
	     CLANG=$(CLANG) AR=$(AR) LD_LLD=$(LD_LLD) QLVZ_REPO=$(QLVZ_REPO) \
	     >/dev/null
	@latr_sig=$$(grep -aho 'latr-fault-diff=[0-9a-f]*' build/latr_fault.log | tail -1); \
	 qemu_sig=$$(grep -aho 'latr-fault-diff=[0-9a-f]*' build/latr_fault_ref.log | tail -1); \
	 if [ -z "$$latr_sig" ] || [ -z "$$qemu_sig" ]; then \
	     echo "latr-fault-diff: missing signature (latr='$$latr_sig' qemu='$$qemu_sig')"; \
	     exit 1; \
	 fi; \
	 if [ "$$latr_sig" != "$$qemu_sig" ]; then \
	     echo "latr-fault-diff: MISMATCH latr=$$latr_sig qemu=$$qemu_sig"; \
	     exit 1; \
	 fi; \
	 echo "latr-fault-diff: PASS $$latr_sig"

latr-smp-image: build/bare_latr_smp.elf

build/latr_smp_guest.elf: baremetal/bare/latr_smp_guest.S
	@mkdir -p build
	$(CLANG) --target=riscv64-unknown-elf -march=rv64ia -mabi=lp64 \
	         -mno-relax -ffreestanding -nostdlib -fuse-ld=lld \
	         -Wl,--no-relax -Wl,-Ttext=0x80000000 $< -o $@

build/latr_smp_guest.bin: build/latr_smp_guest.elf
	$(OBJCOPY) -O binary $< $@

build/latr_smp_blob.o: baremetal/bare/latr_smp_blob.S build/latr_smp_guest.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -c $< -o $@

build/bare_latr_smp.elf: baremetal/bare/latr_smp_test.c \
                         build/latr_smp_blob.o latr-core \
                         build/latr_miniglib.o build/latr_libc_min.o \
                         baremetal/bare/bare.lds
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -c $< -o build/latr_smp_test.o
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_smp_test.o build/latr_smp_blob.o \
	   build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

latr-smp-run: latr-smp-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_smp.elf" \
	     QEMU_LOG="build/latr_smp.log" QEMU_DONE="latr-smp: done" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a "latr-smp" build/latr_smp.log | head -10
	@grep -qa "latr-smp: PASS" build/latr_smp.log

latr-sys-image: build/bare_latr_sys_diff.elf

build/bare_latr_sys_diff.elf: baremetal/bare/latr_sys_diff.c \
                              build/latr_system_guest_elf.c latr-core \
                              build/latr_miniglib.o build/latr_libc_min.o \
                              baremetal/bare/bare.lds
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -I build -c $< -o build/latr_sys_diff.o
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_sys_diff.o build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

latr-sys-run: latr-sys-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_sys_diff.elf" \
	     QEMU_LOG="build/latr_sys_diff.log" QEMU_DONE="latr-sys: done" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a "latr-sys" build/latr_sys_diff.log | head -10
	@grep -qa "latr-sys: PASS" build/latr_sys_diff.log

latr-sys-diff: latr-sys-ref latr-sys-run
	@q=$$(grep -a 'SYS:' build/latr_system_guest_qemu.log | tail -1 | sed 's/.*SYS:/SYS:/'); \
	 l=$$(grep -a 'latr-sys: guest=' build/latr_sys_diff.log | tail -1 | sed 's/.*guest=//'); \
	 echo "qemu: $$q"; echo "latr: $$l"; \
	 test -n "$$q" && test "$$q" = "$$l" && echo "latr-sys-diff: PASS"

latr-boot-image: build/bare_latr_boot.elf

build/latr_boot_guest.elf: tests/latr_boot_guest.S
	@mkdir -p build
	$(CLANG) --target=riscv64-unknown-elf -march=rv64i -mabi=lp64 \
	         -mcmodel=medany -ffreestanding -nostdlib -fno-builtin \
	         -fuse-ld=lld -Wl,-Ttext=0x80000000 -Wl,-e,_start \
	         -Wl,--no-relax $< -o $@

build/latr_boot_guest.bin: build/latr_boot_guest.elf
	$(OBJCOPY) -O binary $< $@

build/latr_boot_guest_blob.c: build/latr_boot_guest.bin
	@python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); \
print("const unsigned char latr_boot_guest[] = {" + ",".join(str(b) for b in d) + "};"); \
print("const unsigned int latr_boot_guest_len = %d;" % len(d))' $< > $@

build/latr_boot_guest_blob.o: build/latr_boot_guest_blob.c
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -c $< -o $@

build/bare_latr_boot.elf: baremetal/bare/latr_boot_smoke.c latr-core \
                          target/riscv/latr/include/latr-system.h \
                          target/riscv/latr/include/latr.h \
                          build/latr_boot_guest_blob.o \
                          build/latr_miniglib.o build/latr_libc_min.o \
                          baremetal/bare/bare.lds
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -I build -c baremetal/bare/latr_boot_smoke.c -o build/latr_boot_smoke.o
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_boot_smoke.o build/latr_boot_guest_blob.o \
	   build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

latr-boot-run: latr-boot-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_boot.elf" \
	     QEMU_LOG="build/latr_boot.log" QEMU_DONE="latr-boot: done" \
	     QEMU_TIMEOUT=30 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a "latr-boot" build/latr_boot.log | head -10
	@grep -qa "latr-boot: PASS" build/latr_boot.log

# Optional: boot a real prebuilt OpenSBI fw_dynamic through the LATR machine.
OPEN_SBI_BIN ?= /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin

build/opensbi64.bin: $(OPEN_SBI_BIN)
	@mkdir -p build
	cp $< $@

build/latr_opensbi_blob.o: baremetal/bare/latr_opensbi_blob.S build/opensbi64.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -c $< -o $@

# A real S-mode next stage, so the handoff OpenSBI performs is observable
# instead of jumping into blank RAM. Linked at LATR_MACHINE_KERNEL_BASE
# (0x80200000) because the payload is position-dependent and uses `la`
# (auipc+addi) throughout; latr_machine_load_kernel() places it there.
build/latr_smode_payload.elf: baremetal/bare/latr_smode_payload.S
	@mkdir -p build
	$(CLANG) --target=riscv64-unknown-elf -march=rv64i -mabi=lp64 \
	         -mno-relax -ffreestanding -nostdlib -fuse-ld=lld \
	         -Wl,--no-relax -Wl,-Ttext=0x80200000 $< -o $@

build/latr_smode_payload.bin: build/latr_smode_payload.elf
	$(OBJCOPY) -O binary $< $@

build/latr_smode_blob.o: baremetal/bare/latr_smode_blob.S build/latr_smode_payload.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -c $< -o $@

# Budget knobs for the real-firmware run. The defaults fail fast; a long
# investigation passes e.g. OPEN_SBI_TBS=400000 OPEN_SBI_STALL=0, because
# OpenSBI legitimately stays silent for tens of thousands of TBs while it
# walks the device tree.
OPEN_SBI_TBS ?= 4000
OPEN_SBI_STALL ?= 4
OPEN_SBI_PCLOG ?= 0
OPEN_SBI_CODEWORDS ?= 200
OPEN_SBI_PROBES ?= 0
OPEN_SBI_STOP ?= 1
OPEN_SBI_EXTRA_CFLAGS ?=

.PHONY: opensbi-cfg-force
opensbi-cfg-force:

build/opensbi_cfg.stamp: opensbi-cfg-force
	@mkdir -p build
	@echo '$(OPEN_SBI_TBS) $(OPEN_SBI_STALL)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@

build/bare_latr_opensbi.elf: baremetal/bare/latr_opensbi_smoke.c latr-core \
                             build/latr_opensbi_blob.o \
                             build/latr_smode_blob.o \
                             build/latr_miniglib.o build/latr_libc_min.o \
                             baremetal/bare/bare.lds build/opensbi_cfg.stamp
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -I build \
	         -DLATR_OPEN_SBI_TBS=$(OPEN_SBI_TBS) \
	         -DLATR_OPEN_SBI_STALL_ROUNDS=$(OPEN_SBI_STALL) \
	         -DLATR_OPEN_SBI_PCLOG_FROM=$(OPEN_SBI_PCLOG) \
	         -DLATR_OPEN_SBI_CODEWORDS=$(OPEN_SBI_CODEWORDS) \
	         -DLATR_OPEN_SBI_PROBES=$(OPEN_SBI_PROBES) \
	         -DLATR_OPEN_SBI_STOP=$(OPEN_SBI_STOP) \
	         $(OPEN_SBI_EXTRA_CFLAGS) \
	         -c baremetal/bare/latr_opensbi_smoke.c -o build/latr_opensbi_smoke.o
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_opensbi_smoke.o build/latr_opensbi_blob.o \
	   build/latr_smode_blob.o \
	   build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

latr-opensbi-image: build/bare_latr_opensbi.elf

# Differential-debug build: same firmware, but every dispatched TB is recorded
# so the guest PC stream can be diffed against `qemu-system-riscv64 -d exec`.
OPEN_SBI_TRACE_TBS ?= 32000

build/latr_opensbi_smoke_trace.o: baremetal/bare/latr_opensbi_smoke.c
	@mkdir -p build
	$(CLANG) $(LATR_BARE_CFLAGS) -I build -DLATR_OPEN_SBI_TRACE=1 \
	         -DLATR_OPEN_SBI_TBS=$(OPEN_SBI_TRACE_TBS) -c $< -o $@

build/bare_latr_opensbi_trace.elf: build/latr_opensbi_smoke_trace.o \
                                   build/latr_opensbi_blob.o \
                                   build/latr_smode_blob.o \
                                   build/latr_miniglib.o build/latr_libc_min.o \
                                   baremetal/bare/bare.lds
	@mkdir -p build
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latr_opensbi_smoke_trace.o build/latr_opensbi_blob.o \
	   build/latr_smode_blob.o \
	   build/latr_miniglib.o build/latr_libc_min.o \
	   latr/build/liblatr_core.a -o $@

latr-opensbi-trace: build/bare_latr_opensbi_trace.elf

latr-opensbi-trace-run: build/bare_latr_opensbi_trace.elf
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_opensbi_trace.elf" \
	     QEMU_LOG="build/latr_opensbi_trace.log" QEMU_DONE="latr-opensbi: done" \
	     QEMU_TIMEOUT=1800 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a "latr-opensbi: rc=\|trace_n=" build/latr_opensbi_trace.log | cut -c1-120

latr-opensbi-run: latr-opensbi-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_opensbi.elf" \
	     QEMU_LOG="build/latr_opensbi.log" QEMU_DONE="latr-opensbi: done" \
	     QEMU_TIMEOUT=1800 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a "latr-opensbi" build/latr_opensbi.log | head -6
	@grep -qa "latr-opensbi: PASS" build/latr_opensbi.log

# Acceptance test for rung 1 of the boot ladder: a real fw_dynamic OpenSBI must
# run from the M-mode entry, reach S-mode, and shut the machine down. Kept out
# of test-latr because it needs the QEMU-shipped firmware binary, but it stays a
# single command so it can be wired into CI wherever that file exists.
LATR_OPEN_SBI_TEST_TBS ?= 1500000

latr-opensbi-test:
	@$(MAKE) --no-print-directory latr-opensbi-run \
	    OPEN_SBI_TBS=$(LATR_OPEN_SBI_TEST_TBS) OPEN_SBI_STALL=0 \
	    OPEN_SBI_CODEWORDS=0

latr-isa-image: build/bare_latr_isa.elf

latr-check:
	$(MAKE) -C latr check

test-latr: latr-check latr-isa-run latr-riscv-ref latr-boot-run
	@latr_sig=$$(grep -aEo 'latr-diff=[0-9a-f]+' build/latr_isa.log | tail -1); \
	 qemu_sig=$$(grep -aEo 'latr-diff=[0-9a-f]+' build/latr_riscv_ref.log | tail -1); \
	 test -n "$$latr_sig" && test "$$latr_sig" = "$$qemu_sig" && \
	 echo "latr-diff: PASS $$latr_sig"

latr-isa-run: latr-isa-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latr_isa.elf" \
	     QEMU_LOG="build/latr_isa.log" QEMU_DONE="latr-isa: done" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -aE 'latr-isa' build/latr_isa.log | head -30
	@grep -qa 'latr-isa: PASS' build/latr_isa.log

# Random-instruction differential vectors: one generator, two consumers (the
# LATR ISA image includes the .h, the QEMU reference assembles the .inc), so the
# executed words cannot drift apart.
LATR_RAND_GEN := tools/gen_latr_rand_vectors.py
build/latr_rand_vectors.h build/latr_rand_run.inc: $(LATR_RAND_GEN)
	@mkdir -p build
	$(PYTHON) $(LATR_RAND_GEN)

build/latr_riscv_ref.elf: tests/latr_isa_qemu_ref.S \
                          tests/latr_isa_diff_vectors.h \
                          build/latr_rand_run.inc
	@mkdir -p build
	$(CLANG) --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d \
	         -ffreestanding -nostdlib -fno-builtin -O2 -fuse-ld=lld \
	         -I tests -I build -Wl,-Ttext=0x80000000 -Wl,-e,_start $< -o $@

# Additional seeds, run in CI after the fixed one. Each seed is a different
# 48-instruction stream: a translator bug that one stream happens to miss is
# found by the next, at the cost of one rebuild and one QEMU boot.
LATR_RAND_SEEDS ?= 1 2 3
latr-rand-sweep:
	@set -e; \
	 for seed in $(LATR_RAND_SEEDS); do \
	     echo "--- rand seed $$seed"; \
	     LATR_RAND_SEED=$$seed $(PYTHON) $(LATR_RAND_GEN) >/dev/null; \
	     rm -f build/latr_isa_test.o build/bare_latr_isa.elf \
	           build/latr_riscv_ref.elf; \
	     $(MAKE) --no-print-directory latr-rand-diff \
	         CLANG=$(CLANG) AR=$(AR) LD_LLD=$(LD_LLD) \
	         QLVZ_REPO=$(QLVZ_REPO) | tail -1; \
	 done; \
	 $(PYTHON) $(LATR_RAND_GEN) >/dev/null; \
	 rm -f build/latr_isa_test.o build/bare_latr_isa.elf \
	       build/latr_riscv_ref.elf

# Unified benchmark: one table out of the logs the targets already write. It is
# report-only on purpose (see tools/latr_bench.py); `latr-bench-run` refreshes
# every source first, which is what CI does.
latr-bench:
	@$(PYTHON) tools/latr_bench.py

latr-bench-run: latr-isa-run latr-smp-run latr-rand-diff latr-fault-diff
	@$(PYTHON) tools/latr_bench.py

# Delete both logs first: a stale log left by an earlier seed would otherwise be
# compared and "pass". A missing signature is then a hard error.
latr-rand-diff:
	@rm -f build/latr_isa.log build/latr_riscv_ref.log
	@$(MAKE) --no-print-directory latr-isa-run latr-riscv-ref \
	     CLANG=$(CLANG) AR=$(AR) LD_LLD=$(LD_LLD) QLVZ_REPO=$(QLVZ_REPO) \
	     >/dev/null
	@latr_sig=$$(grep -aho 'latr-rand-diff=[0-9a-f]*' build/latr_isa.log | tail -1); \
	 qemu_sig=$$(grep -aho 'latr-rand-diff=[0-9a-f]*' build/latr_riscv_ref.log | tail -1); \
	 if [ -z "$$latr_sig" ] || [ -z "$$qemu_sig" ]; then \
	     echo "latr-rand-diff: missing signature (latr='$$latr_sig' qemu='$$qemu_sig')"; \
	     exit 1; \
	 fi; \
	 if [ "$$latr_sig" != "$$qemu_sig" ]; then \
	     echo "latr-rand-diff: MISMATCH latr=$$latr_sig qemu=$$qemu_sig"; \
	     exit 1; \
	 fi; \
	 echo "latr-rand-diff: PASS $$latr_sig"

latr-riscv-ref: build/latr_riscv_ref.elf
	@command -v $(QEMU_RISCV64) >/dev/null 2>&1 || { \
	     echo "latr-riscv-ref: $(QEMU_RISCV64) is not installed or not on PATH"; \
	     echo "latr-riscv-ref: PATH=$$PATH"; \
	     exit 1; }
	rm -f build/latr_riscv_ref.log
	$(if $(TIMEOUT),$(TIMEOUT) 20,) $(QEMU_RISCV64) -M virt -cpu max -m 128M -bios none \
	     -kernel $< -display none -monitor none \
	     -serial file:build/latr_riscv_ref.log -no-reboot 2>/dev/null || true
	grep -a 'latr-riscv-ref: PASS' build/latr_riscv_ref.log
	grep -aE 'latr-diff=' build/latr_riscv_ref.log

build/lata_smoke.o: baremetal/bare/lata_smoke.c
	@mkdir -p build
	$(CLANG) $(LATA_BARE_CFLAGS) -c $< -o $@

build/lata_miniglib.o: latx/compat/miniglib.c
	@mkdir -p build
	$(CLANG) $(LATA_BARE_CFLAGS) -c $< -o $@

build/lata_libc_min.o: latx/compat/libc_min.c
	@mkdir -p build
	$(CLANG) $(LATA_BARE_CFLAGS) -c $< -o $@

build/lata_isa_test.o: baremetal/bare/lata_isa_test.c
	@mkdir -p build
	$(CLANG) $(LATA_BARE_CFLAGS) -c $< -o $@

build/bare_lata_isa.elf: build/lata_isa_test.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/lata_isa_test.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a -o $@

build/bare_lata.elf: build/lata_smoke.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/lata_smoke.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a -o $@

build/lbt_smoke.o: baremetal/bare/lbt_smoke.c
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
	         -c $< -o $@

build/lbt_smoke.elf: build/lbt_smoke.o baremetal/bare/bare.lds
	$(LD_LLD) -T baremetal/bare/bare.lds -e _start $< -o $@

QLVZ ?= $(HOME)/Documents/QEMU-LVZ/build/qemu-system-loongarch64
QLVZ_REPO ?= $(CURDIR)/qemu-system-loongarch64
QEMU_RISCV64 ?= $(if $(wildcard ../QEMU-LVZ/build/qemu-system-riscv64),../QEMU-LVZ/build/qemu-system-riscv64,qemu-system-riscv64)
lbt-smoke-run: build/lbt_smoke.elf
	@rm -f build/lbt_smoke.log
	@timeout 8 $(QLVZ) -M virt -cpu max -m 512M -kernel build/lbt_smoke.elf \
	     -display none -monitor none -serial file:build/lbt_smoke.log -no-reboot 2>/dev/null || true
	@grep -a 'lbt-smoke:' build/lbt_smoke.log | tail -9
	@grep -qa 'lbt-smoke: PASS' build/lbt_smoke.log

lata-isa-image: build/bare_lata_isa.elf

# Stage 1 regression: aarch64 constructs through the translator, checked in the
# environment.  Same watchdog: the image finishes and then spins by design.
lata-isa-run: lata-isa-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_lata_isa.elf" \
	     QEMU_LOG="build/lata_isa.log" QEMU_DONE="lata-isa: done" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -aE 'lata-isa' build/lata_isa.log | head -30
	@grep -qa 'lata-isa: PASS' build/lata_isa.log

lata-smoke-image: build/bare_lata.elf

# Run through the watchdog script: a bare QEMU launch leaves an orphan, because
# the image finishes its test and then spins forever by design.
lata-smoke-run: lata-smoke-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_lata.elf" \
	     QEMU_LOG="build/lata_smoke.log" QEMU_DONE="lata-smoke: done" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -aE 'lata-smoke' build/lata_smoke.log | head -20
	@grep -qa 'lata-smoke: PASS' build/lata_smoke.log

test-latm-decode: build/test_latm_decode
	./build/test_latm_decode

test-latm-tlb: build/test_latm_tlb
	./build/test_latm_tlb

test-latm-vclock: build/test_latm_vclock
	./build/test_latm_vclock

test-latm-interrupts: build/test_latm_interrupts
	./build/test_latm_interrupts

test-latm-insn-map: build/test_latm_insn_map
	./build/test_latm_insn_map

build/test_latm_tb_cache: tests/test_latm_tb_cache.c src/tb_cache.c \
                          include/loongemu/tb_cache.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude src/tb_cache.c \
	      tests/test_latm_tb_cache.c -o $@

test-latm-tb-cache: build/test_latm_tb_cache
	./build/test_latm_tb_cache

test-latm-cpu: build/test_latm_cpu
	./build/test_latm_cpu

test-latm-loongson: build/test_latm_loongson
	./build/test_latm_loongson

build/test_latm_loongson: tests/test_latm_loongson.c \
                          target/mips/latm/helpers/loongson.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_loongson.c target/mips/latm/helpers/loongson.c -o $@

test-latm-system: build/test_latm_system
	./build/test_latm_system

test-latm-uncore: build/test_latm_uncore
	./build/test_latm_uncore

test-latm-llsc: build/test_latm_llsc
	./build/test_latm_llsc

test-latm-unaligned: build/test_latm_unaligned
	./build/test_latm_unaligned

build/test_latm_unaligned: tests/test_latm_unaligned.c \
                           target/mips/latm/helpers/memory.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_unaligned.c target/mips/latm/helpers/memory.c -o $@

build/test_latm_llsc: tests/test_latm_llsc.c \
                      target/mips/latm/helpers/int.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_llsc.c target/mips/latm/helpers/int.c -o $@

test-latm-pci: build/test_latm_pci
	./build/test_latm_pci

test-latm-board-system: build/test_latm_board_system
	./build/test_latm_board_system

build/test_latm_board_system: tests/test_latm_board_system.c \
                              target/mips/latm/system/latm_system.c \
                              target/mips/latm/system/latm_mmio.c \
                              target/mips/latm/system/latm_memory.c \
                              target/mips/latm/system/latm_dtb.c \
                              target/mips/latm/system/latm_pci.c \
                              target/mips/latm/system/latm_uart.c \
                              target/mips/latm/uncore/latm_uncore.c \
                              src/pci_host.c src/phys_mem.c src/acpi_tables.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . -Iinclude \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_board_system.c \
	      target/mips/latm/system/latm_system.c \
	      target/mips/latm/system/latm_mmio.c \
	      target/mips/latm/system/latm_memory.c \
	      target/mips/latm/system/latm_dtb.c \
	      target/mips/latm/system/latm_pci.c \
	      target/mips/latm/system/latm_uart.c \
	      target/mips/latm/uncore/latm_uncore.c \
	      src/pci_host.c src/phys_mem.c src/acpi_tables.c -o $@

build/test_latm_pci: tests/test_latm_pci.c \
                     target/mips/latm/system/latm_pci.c \
                     target/mips/latm/system/latm_mmio.c \
                     src/pci_host.c src/phys_mem.c src/acpi_tables.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . -Iinclude \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_pci.c target/mips/latm/system/latm_pci.c \
	      target/mips/latm/system/latm_mmio.c src/pci_host.c \
	      src/phys_mem.c src/acpi_tables.c -o $@

build/test_latm_uncore: tests/test_latm_uncore.c \
                        target/mips/latm/uncore/latm_uncore.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_uncore.c target/mips/latm/uncore/latm_uncore.c -o $@

build/test_latm_system: tests/test_latm_system.c \
                        target/mips/latm/system/latm_mmio.c \
                        target/mips/latm/system/latm_memory.c \
                        target/mips/latm/system/latm_dtb.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_system.c target/mips/latm/system/latm_mmio.c \
	      target/mips/latm/system/latm_memory.c \
	      target/mips/latm/system/latm_dtb.c -o $@

build/test_latm_cpu: tests/test_latm_cpu.c \
                    target/mips/latm/cpu/loongson-profile.c \
                    target/mips/latm/helpers/cp0.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_cpu.c target/mips/latm/cpu/loongson-profile.c \
	      target/mips/latm/helpers/cp0.c -o $@

build/test_latm_insn_map: tests/test_latm_insn_map.c \
                          target/mips/latm/tb_insn_map.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
      -I shared/backend/loongarch/include \
      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_insn_map.c target/mips/latm/tb_insn_map.c -o $@

build/test_latm_interrupts: tests/test_latm_interrupts.c \
                           target/mips/latm/helpers/cp0.c \
                           target/mips/latm/helpers/exception.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_interrupts.c target/mips/latm/helpers/cp0.c \
	      target/mips/latm/helpers/exception.c -o $@

build/test_latm_vclock: tests/test_latm_vclock.c src/vclock.c include/loongemu/vclock.h
	@mkdir -p build
	$(CC) $(CFLAGS) tests/test_latm_vclock.c src/vclock.c -o $@

build/test_latm_tlb: tests/test_latm_tlb.c target/mips/latm/helpers/memory.c \
                     target/mips/latm/helpers/cp0.c target/mips/latm/helpers/exception.c
	@mkdir -p build
	$(CC) -I target/mips/latm/compat -I target/mips/latm/include \
	      -I target/mips -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
	      -I latx/compat -I latx/upstream/include -I . \
	      $(CFLAGS) -Wno-gnu-empty-struct \
	      tests/test_latm_tlb.c target/mips/latm/helpers/memory.c \
	      target/mips/latm/helpers/cp0.c target/mips/latm/helpers/exception.c -o $@

build/test_latm_decode: tests/test_latm_decode.c target/mips/latm/ir1/decode-mips.c target/mips/latm/include/ir1.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I target/mips/latm/include -I target/mips \
	      -I target/mips/latm/compat -I latx/compat \
	      -I latx/upstream/include -I . \
	      tests/test_latm_decode.c target/mips/latm/ir1/decode-mips.c -o $@

# ---- LATM: MIPS64r2 -> LoongArch ----
.PHONY: test-latm test-latm-decode test-latm-tlb test-latm-vclock test-latm-tb-cache latm-core latm-isa-image latm-isa-run latm-board-image latm-board-run latm-boot-smoke-run
test-latm: test-latm-decode test-latm-tlb test-latm-vclock test-latm-interrupts test-latm-insn-map test-latm-tb-cache test-latm-cpu test-latm-loongson test-latm-system test-latm-uncore test-latm-llsc test-latm-unaligned test-latm-pci test-latm-board-system latm-isa-run latm-board-run latm-boot-smoke-run

# ---- bare-metal MIPS64r2 (LATM) execution harness ----
LATM_BARE_CFLAGS := --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
               -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
               -DTARGET_MIPS64 -DCONFIG_LATM -DCONFIG_LATM_STANDALONE \
               -D__WORDSIZE=64 -D__thread= \
               -I target/mips/latm/include -I target/mips/latm/compat -I target/mips \
               -I target/arm/lata/include -I shared/pvir/include -I target/arm/lata \
               -I shared/backend/loongarch/include -I shared/perf/include \
               -I latx/compat -I latx/include -I latx/upstream/include -I . \
               -isystem $(shell $(CLANG) -print-resource-dir)/include

# LATM_FORCE_NO_LBT=1 makes the LATM image take the plain `jirl` chaining path.
# qemu advertises CPUCFG.2[22] but does not decode `gr2scr`/`jiscr0`, so the
# CI gate builds with this and the harness skips the jiscr1-only case.
ifeq ($(LATM_FORCE_NO_LBT),1)
LATM_BARE_CFLAGS += -DLATM_FORCE_NO_LBT
endif

latm-core:
	@$(MAKE) -C latm all

build/latm_isa_test.o: baremetal/bare/latm_isa_test.c \
                       target/mips/latm/include/latm.h \
                       target/mips/latm/include/latm-platform.h \
                       target/mips/cpu.h target/mips/internal.h
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -c $< -o $@

build/latm_miniglib.o: latx/compat/miniglib.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -c $< -o $@

build/latm_libc_min.o: latx/compat/libc_min.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -c $< -o $@

build/bare_latm_isa.elf: build/latm_isa_test.o build/latm_miniglib.o build/latm_libc_min.o latm/build/liblatm_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latm_isa_test.o build/latm_miniglib.o build/latm_libc_min.o latm/build/liblatm_core.a -o $@

latm-isa-image: latm-core build/bare_latm_isa.elf

build/latm_board.o: baremetal/bare/latm_board.c \
                    target/mips/latm/include/latm.h \
                    target/mips/latm/include/latm-platform.h \
                    target/mips/cpu.h target/mips/internal.h
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_phys_mem.o: src/phys_mem.c include/loongemu/phys_mem.h
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_tb_cache.o: src/tb_cache.c include/loongemu/tb_cache.h
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_vclock.o: src/vclock.c include/loongemu/vclock.h
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/bare_latm_board.elf: build/latm_board.o build/latm_phys_mem.o                             build/latm_vclock.o build/latm_tb_cache.o                             build/latm_system.o build/latm_mmio.o                             build/latm_memory.o build/latm_dtb.o                             build/latm_pci.o build/latm_uart.o                             build/latm_uncore.o build/latm_acpi_tables.o                             build/latm_pci_host.o                             build/latm_miniglib.o build/latm_libc_min.o                             latm/build/liblatm_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latm_board.o build/latm_phys_mem.o build/latm_vclock.o \
	   build/latm_tb_cache.o build/latm_system.o build/latm_mmio.o \
	   build/latm_memory.o build/latm_dtb.o build/latm_pci.o \
	   build/latm_uart.o build/latm_uncore.o build/latm_acpi_tables.o \
	   build/latm_pci_host.o build/latm_miniglib.o build/latm_libc_min.o \
	   latm/build/liblatm_core.a -o $@

latm-board-image: latm-core build/bare_latm_board.elf

build/latm_boot_smoke.o: baremetal/bare/latm_boot_smoke.c \
                         target/mips/latm/include/latm.h \
                         target/mips/latm/include/latm-system-board.h \
                         target/mips/cpu.h target/mips/internal.h
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_system.o: target/mips/latm/system/latm_system.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_mmio.o: target/mips/latm/system/latm_mmio.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_memory.o: target/mips/latm/system/latm_memory.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_dtb.o: target/mips/latm/system/latm_dtb.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_pci.o: target/mips/latm/system/latm_pci.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_uart.o: target/mips/latm/system/latm_uart.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_uncore.o: target/mips/latm/uncore/latm_uncore.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_acpi_tables.o: src/acpi_tables.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/latm_pci_host.o: src/pci_host.c
	@mkdir -p build
	$(CLANG) $(LATM_BARE_CFLAGS) -Iinclude -c $< -o $@

build/bare_latm_boot_smoke.elf: build/latm_boot_smoke.o build/latm_system.o \
                                 build/latm_mmio.o build/latm_memory.o \
                                 build/latm_dtb.o build/latm_pci.o \
                                 build/latm_uart.o build/latm_uncore.o \
                                 build/latm_phys_mem.o build/latm_vclock.o \
                                 build/latm_acpi_tables.o build/latm_pci_host.o \
                                 build/latm_miniglib.o build/latm_libc_min.o \
                                 latm/build/liblatm_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/latm_boot_smoke.o build/latm_system.o build/latm_mmio.o \
	   build/latm_memory.o build/latm_dtb.o build/latm_pci.o \
	   build/latm_uart.o build/latm_uncore.o build/latm_phys_mem.o \
	   build/latm_vclock.o build/latm_acpi_tables.o build/latm_pci_host.o \
	   build/latm_miniglib.o build/latm_libc_min.o \
	   latm/build/liblatm_core.a -o $@

latm-boot-smoke-image: latm-core build/bare_latm_boot_smoke.elf

latm-boot-smoke-run: latm-boot-smoke-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latm_boot_smoke.elf" \
	     QEMU_LOG="build/latm_boot_smoke.log" QEMU_DONE="latm-boot-smoke: PASS" \
	     QEMU_TIMEOUT=30 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a 'latm-boot-smoke' build/latm_boot_smoke.log

latm-board-run: latm-board-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latm_board.elf" \
	     QEMU_LOG="build/latm_board.log" QEMU_DONE="latm-board: done" \
	     QEMU_TIMEOUT=20 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -a 'latm-board' build/latm_board.log
	@grep -qa 'latm-board: done' build/latm_board.log

latm-isa-run: latm-isa-image
	@env QEMU_BIN="$(QLVZ_REPO)" QEMU_KERNEL="build/bare_latm_isa.elf" \
	     QEMU_LOG="build/latm_isa.log" QEMU_DONE="latm-isa: checks=.*PASS" \
	     QEMU_TIMEOUT=60 CPU=la464 \
	     bash scripts/run_qemu_lata.sh
	@grep -aE 'latm-isa' build/latm_isa.log | head -40
	@grep -qa 'latm-isa: checks=.*PASS' build/latm_isa.log

BARE_CFLAGS := --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
               -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
               -DTARGET_X86_64 -DCONFIG_LATX -DCONFIG_LATX_FLAG_REDUCTION \
               -DCONFIG_LATX_AVX_OPT -DCONFIG_LATX_CAPSTONE_GIT \
               -DCAPSTONE_HAS_X86 -DCAPSTONE_USE_SYS_DYN_MEM=1 -D__thread= \
               -I latx/compat -I latx/include -I latx -I latx/qemu \
               -I latx/compat/libm -I baremetal/gen/latx/include \
               -I latx/capstone_git/include \
               -I latx/upstream/include \
               -isystem $(shell $(CLANG) -print-resource-dir)/include \
               -include latx/compat/latx_bare_prelude.h
# TB_SYS_TRACE=1 prints one line per dispatched TB in the paging/PLV3 test, to
# compare a stalling image against a working one.
ifeq ($(TB_SYS_TRACE),1)
BARE_CFLAGS += -DTB_SYS_TRACE_STEPS
endif

BARE_DEPFLAGS = -MD -MP -MF $(patsubst %.o,%.d,$@) -MT $@
BARE_DEP_OBJECTS := build/bare_main.o build/tb_loop.o build/lbt_emu.o \
                    build/tb_cache_bare.o build/tb_harness.o \
                    build/loongarch_paging_bare.o build/acpi_tables_bare.o \
                    build/tb_sys.o build/regress_x86.o build/tb_harness_test.o \
                    build/vmm_entry.o build/vmm_loongarch_paging.o \
                    build/vmm_acpi_tables.o build/vmm_vclock.o \
                    build/vmm_integrated_entry.o build/vmm_integrated_tb_sys.o
-include $(patsubst %.o,%.d,$(BARE_DEP_OBJECTS))

build/bare_main.o: baremetal/bare/main.c baremetal/bare/lbt_emu.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Ibaremetal/bare -c $< -o $@

build/bare_latx.elf: build/bare_main.o build/lbt_emu.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/bare_main.o build/lbt_emu.o baremetal/build/liblatx_core.a -o $@

# ---- P1: multi-TB dynamic execution loop ----
build/tb_loop.o: baremetal/bare/tb_loop.c include/loongemu/tb_cache.h baremetal/bare/lbt_emu.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/lbt_emu.o: baremetal/bare/lbt_emu.c baremetal/bare/lbt_emu.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/tb_cache_bare.o: src/tb_cache.c include/loongemu/tb_cache.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -c $< -o $@

build/tb_harness.o: baremetal/bare/tb_harness.c baremetal/bare/tb_harness.h latx/include/ir1.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/tb_loop.elf: build/tb_loop.o build/tb_harness.o build/tb_cache_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/tb_loop.o build/tb_harness.o build/tb_cache_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a -o $@

tb-loop-image: build/tb_loop.elf

build/loongarch_paging_bare.o: src/loongarch_paging.c include/loongemu/loongarch_paging.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -c $< -o $@

build/acpi_tables_bare.o: src/acpi_tables.c
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/tb_sys.o: baremetal/bare/tb_sys.c include/loongemu/tb_cache.h include/loongemu/loongarch_paging.h include/loongemu/hyperv.h baremetal/bare/lbt_emu.h baremetal/bare/tb_harness.h latx/include/ir1.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/vmlinux_text.o: baremetal/bare/vmlinux_text.S firmware/vmlinux-x86_64-text.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib -c $< -o $@

INITRAMFS ?= firmware/initramfs.cpio.gz
# firmware/initramfs.cpio.gz is gitignored (a test asset, not source), so a
# fresh checkout had no way to build the SMP/boot images. The boot smoke only
# needs a *valid* archive -- it asserts translation counters and reaches its
# stop marker with a kernel-only boot -- so a missing file is generated with the
# root entry only. A real image placed there is never overwritten.
$(INITRAMFS):
	@python3 scripts/mk_initramfs.py $@

build/vmlinux_segs.o: baremetal/bare/vmlinux_segs.S firmware/vmlinux-x86_64-seg1.bin firmware/vmlinux-x86_64-seg2.bin firmware/vmlinux-x86_64-seg3.bin $(INITRAMFS)
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         '-DLOONGEMU_INITRAMFS_PATH="$(abspath $(INITRAMFS))"' -c $< -o $@

.PHONY: refresh-baremetal-core
refresh-baremetal-core:
	$(MAKE) -C baremetal all

baremetal/build/liblatx_core.a: | refresh-baremetal-core
	@test -f $@

build/tb_sys.elf: build/tb_sys.o build/tb_harness.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o build/acpi_tables_bare.o build/vmlinux_text.o build/vmlinux_segs.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/tb_sys.o build/tb_harness.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o build/acpi_tables_bare.o build/vmlinux_text.o build/vmlinux_segs.o baremetal/build/liblatx_core.a -o $@

tb-sys-image: build/tb_sys.elf

build/regress_x86.o: baremetal/bare/regress_x86.c include/loongemu/tb_cache.h include/loongemu/loongarch_paging.h baremetal/bare/lbt_emu.h baremetal/bare/tb_harness.h latx/include/ir1.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@


build/tb_harness_test.o: baremetal/bare/tb_harness_test.c baremetal/bare/tb_harness.h latx/include/ir1.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@
build/regress_x86.elf: build/regress_x86.o build/tb_harness.o build/tb_harness_test.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/regress_x86.o build/tb_harness.o build/tb_harness_test.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a -o $@

regress-x86-image: build/regress_x86.elf

tb-sys-run: build/tb_sys.elf
	@rm -f build/tb_sys.log build/tb_sys.err
	@timeout 90 $(QLVZ) -M virt -cpu max -m 512M -kernel build/tb_sys.elf \
	     -display none -monitor none -nic none \
	     -serial file:build/tb_sys.log -no-reboot 2>build/tb_sys.err || true
	@if ! grep -qa 'tb-sys: done' build/tb_sys.log 2>/dev/null; then \
	    echo "tb-sys: the harness did not reach 'tb-sys: done'; qemu stderr:"; \
	    tail -n 10 build/tb_sys.err 2>/dev/null || true; fi
	@grep -a 'tb-sys:' build/tb_sys.log | tail -12
	@grep -qa 'tb-sys: done' build/tb_sys.log
	@grep -qa 'fails=0000000000000000 ok' build/tb_sys.log

tb-sys-smp-run: build/tb_sys.elf
	@rm -f build/tb_sys_smp.log build/tb_sys_smp.err
	@timeout 90 $(QLVZ) -M virt -cpu max -smp 4 -m 512M -kernel build/tb_sys.elf \
	     -display none -monitor none -nic none \
	     -serial file:build/tb_sys_smp.log -no-reboot \
	     2>build/tb_sys_smp.err || true
	@if ! grep -qa 'tb-sys: done' build/tb_sys_smp.log 2>/dev/null; then \
	    echo "tb-sys-smp: the harness did not reach 'tb-sys: done'; qemu stderr:"; \
	    tail -n 10 build/tb_sys_smp.err 2>/dev/null || true; fi
	@if ! grep -qa 'fails=0000000000000000 ok' build/tb_sys_smp.log 2>/dev/null; then \
	    echo "tb-sys-smp: failing checks:"; \
	    grep -a 'FAIL ' build/tb_sys_smp.log | head -20 || true; fi
	@grep -a 'tb-sys:' build/tb_sys_smp.log | tail -12
	@grep -qa 'tb-sys: phys_cpus_online=0000000000000004' build/tb_sys_smp.log
	@grep -qa 'tb-sys: done' build/tb_sys_smp.log
	@grep -qa 'fails=0000000000000000 ok' build/tb_sys_smp.log

tb-loop-run: build/tb_loop.elf
	@rm -f build/tb_loop.log
	@timeout 15 $(QLVZ) -M virt -cpu max -m 512M -kernel build/tb_loop.elf \
	     -display none -monitor none -serial file:build/tb_loop.log -no-reboot 2>/dev/null || true
	@grep -a 'tb-loop:' build/tb_loop.log | tail -8
	@grep -qa 'tb-loop: done' build/tb_loop.log
	@grep -qa 'fails=0' build/tb_loop.log


lata-all:
	$(MAKE) -C lata all

lata-probe:
	$(MAKE) -C lata probe

lata-check: lata-all
	./tools/check_lata_symbols.sh lata/build/liblata_core.a lata/lata_externals.allowlist

bare-image: build/bare_latx.elf

bare-run: bare-image
	@rm -f build/bare_run.log
	@( qemu-system-loongarch64 -M virt -cpu la464 -m 512M -kernel build/bare_latx.elf \
	     -display none -monitor none -serial file:build/bare_run.log -no-reboot & \
	   echo $$! > build/qemu_bare.pid )
	@sleep 12; kill $$(cat build/qemu_bare.pid) 2>/dev/null; true
	@grep -aE 'bare:|lbt|FAIL' build/bare_run.log | head -30

# ---- P0 UEFI entry: standalone probe and System VMM handoff ------------
VMM_INC := -Iinclude -Ibaremetal/efi
VMM_EFI_CFLAGS := --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
                  -fno-builtin -fno-stack-protector -O2 -mno-lsx -mno-lasx \
                  -fno-vectorize -fno-slp-vectorize $(VMM_INC)
OVMF_CODE ?= $(firstword $(wildcard \
	$(QEMU_PREFIX)/share/qemu/edk2-x86_64-code.fd \
	/usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd firmware/OVMF_CODE.fd))
OVMF_VARS ?= $(firstword $(wildcard \
	$(QEMU_PREFIX)/share/qemu/edk2-x86_64-vars.fd \
	$(shell brew --prefix 2>/dev/null)/share/qemu/edk2-i386-vars.fd \
	/usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/x64/OVMF_VARS.fd \
	/usr/share/edk2-ovmf/x64/OVMF_VARS.fd firmware/OVMF_VARS.fd))

# Standalone paging probe remains a separate EFI application.
build/vmm_entry.o: baremetal/efi/vmm_entry.c include/loongemu/vmm_handoff.h \
                   include/loongemu/loongarch_paging.h baremetal/efi/efi_min.h
	@mkdir -p build
	$(CLANG) $(VMM_EFI_CFLAGS) $(BARE_DEPFLAGS) -DVMM_STANDALONE_PROBE -c $< -o $@

build/vmm_loongarch_paging.o: src/loongarch_paging.c include/loongemu/loongarch_paging.h
	@mkdir -p build
	$(CLANG) $(VMM_EFI_CFLAGS) $(BARE_DEPFLAGS) -c $< -o $@

build/vmm_acpi_tables.o: src/acpi_tables.c include/loongemu/acpi_tables.h
	@mkdir -p build
	$(CLANG) $(VMM_EFI_CFLAGS) $(BARE_DEPFLAGS) -c $< -o $@

build/vmm_vclock.o: src/vclock.c include/loongemu/vclock.h
	@mkdir -p build
	$(CLANG) $(VMM_EFI_CFLAGS) $(BARE_DEPFLAGS) -c $< -o $@

build/vmm_entry.elf: build/vmm_entry.o build/vmm_loongarch_paging.o \
                     build/vmm_acpi_tables.o build/vmm_vclock.o
	@printf 'ENTRY(efi_main)\nSECTIONS { . = 0x1000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n' > build/vmm.lds
	$(LD_LLD) --emit-relocs -T build/vmm.lds -e efi_main \
	   build/vmm_entry.o build/vmm_loongarch_paging.o build/vmm_acpi_tables.o \
	   build/vmm_vclock.o -o $@

build/vmm_entry.efi: build/vmm_entry.elf
	$(GENFW) -o $@ -e UEFI_APPLICATION $<

vmm-efi: build/vmm_entry.efi

vmm-boot: vmm-efi
	@rm -rf build/esp_vmm && mkdir -p build/esp_vmm/EFI/BOOT
	@cp build/vmm_entry.efi build/esp_vmm/EFI/BOOT/BOOTLOONGARCH64.EFI
	@rm -f build/vmm-serial.log
	@( qemu-system-loongarch64 -M virt -cpu la464 -m 2G \
	     -bios firmware/QEMU_EFI.fd \
	     -drive id=esp,file=fat:rw:build/esp_vmm,format=raw,if=none \
	     -device virtio-blk-pci,drive=esp \
	     -display none -monitor none -serial file:build/vmm-serial.log -no-reboot 2>/dev/null & \
	   echo $$! > build/qemu_vmm.pid )
	@sleep 40; kill $$(cat build/qemu_vmm.pid) 2>/dev/null; true
	@grep -aE 'vmm:' build/vmm-serial.log | head -30

# The integrated EFI image embeds the supplied x86-64 guest firmware, then
# copies it into independent UEFI-owned buffers before ExitBootServices.
build/vmm_ovmf_payload.o: baremetal/efi/vmm_ovmf_payload.S $(OVMF_CODE) $(OVMF_VARS)
	@test -r "$(OVMF_CODE)" || { echo "Set OVMF_CODE to an x86-64 OVMF_CODE.fd file" >&2; exit 1; }
	@test -r "$(OVMF_VARS)" || { echo "Set OVMF_VARS to an x86-64 OVMF_VARS.fd file" >&2; exit 1; }
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         '-DLOONGEMU_OVMF_CODE_PATH="$(abspath $(OVMF_CODE))"' \
	         '-DLOONGEMU_OVMF_VARS_PATH="$(abspath $(OVMF_VARS))"' \
	         -c $< -o $@

build/vmm_integrated_entry.o: baremetal/efi/vmm_entry.c include/loongemu/vmm_handoff.h \
                              include/loongemu/loongarch_paging.h baremetal/efi/efi_min.h
	@mkdir -p build
	$(CLANG) $(VMM_EFI_CFLAGS) $(BARE_DEPFLAGS) -DLOONGEMU_BOOT_OVMF -c $< -o $@

build/vmm_integrated_tb_sys.o: baremetal/bare/tb_sys.c include/loongemu/vmm_handoff.h \
                               include/loongemu/hyperv.h baremetal/bare/tb_harness.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) $(BARE_DEPFLAGS) -DLOONGEMU_BOOT_OVMF -Iinclude -Ibaremetal/bare -c $< -o $@

build/vmm_integrated.elf: build/vmm_integrated_entry.o build/vmm_integrated_tb_sys.o \
                          build/vmm_ovmf_payload.o build/tb_harness.o \
                          build/tb_cache_bare.o build/loongarch_paging_bare.o \
                          build/lbt_emu.o build/acpi_tables_bare.o \
                          build/vmlinux_text.o build/vmlinux_segs.o \
                          baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	$(LD_LLD) --emit-relocs -T baremetal/bare/bare.lds -e efi_main \
	   build/vmm_integrated_entry.o build/vmm_integrated_tb_sys.o \
	   build/vmm_ovmf_payload.o build/tb_harness.o build/tb_cache_bare.o \
	   build/loongarch_paging_bare.o build/lbt_emu.o build/acpi_tables_bare.o \
	   build/vmlinux_text.o build/vmlinux_segs.o \
	   baremetal/build/liblatx_core.a -o $@

build/vmm_integrated.efi: build/vmm_integrated.elf
	$(GENFW) -o $@ -e UEFI_APPLICATION $<

vmm-integrated: build/vmm_integrated.efi

vmm-integrated-boot: vmm-integrated
	@rm -rf build/esp_vmm_integrated && mkdir -p build/esp_vmm_integrated/EFI/BOOT
	@cp build/vmm_integrated.efi build/esp_vmm_integrated/EFI/BOOT/BOOTLOONGARCH64.EFI
	@rm -f build/vmm-integrated-serial.log
	@( qemu-system-loongarch64 -M virt -cpu la464 -m 2G \
	     -bios firmware/QEMU_EFI.fd \
	     -drive id=esp,file=fat:rw:build/esp_vmm_integrated,format=raw,if=none \
	     -device virtio-blk-pci,drive=esp \
	     -display none -monitor none -serial file:build/vmm-integrated-serial.log -no-reboot 2>/dev/null & \
	   echo $$! > build/qemu_vmm_integrated.pid )
	@sleep 60; kill $$(cat build/qemu_vmm_integrated.pid) 2>/dev/null; true
	@grep -aE 'vmm:|tb-sys:' build/vmm-integrated-serial.log | tail -40
	@grep -qa 'fails=0' build/vmm-integrated-serial.log
	@grep -qa 'tb-sys: ovmf_run_live=1' build/vmm-integrated-serial.log
	@! grep -qa 'FAIL\|ovmf_abort\|ovmf_persistent_abort\|refusing to start' build/vmm-integrated-serial.log
