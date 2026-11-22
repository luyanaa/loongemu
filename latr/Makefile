# Freestanding RISC-V -> LoongArch translation core. QEMU's decoder tables are
# generated from the adjacent QEMU-LVZ checkout, which is also the golden ref.
LLVM_PREFIX ?= $(shell brew --prefix llvm)
CLANG       ?= $(LLVM_PREFIX)/bin/clang
AR          := $(LLVM_PREFIX)/bin/llvm-ar
HOST_CC     ?= cc
PYTHON      ?= python3
TARGET      ?= loongarch64-unknown-linux-gnu
QEMU_SRC    ?= ../../QEMU-LVZ
NM          ?= $(LLVM_PREFIX)/bin/llvm-nm
LATR        := ../target/riscv/latr
LATA        := ../target/arm/lata
LATX        := ../latx
BUILD       := build

INCS := -I$(LATR)/include -I$(BUILD) -Icompat -I.. \
        -I$(LATA)/include -I$(LATA) -I$(LATX)/compat -I$(LATX)/compat/include \
        -I$(LATX)/upstream/include -I../include
RESINC := $(shell $(CLANG) -print-resource-dir)/include
DEFS := -DTARGET_RISCV64 -DCONFIG_LATR -DCONFIG_LATR_STANDALONE \
        -DCONFIG_LATX -D__WORDSIZE=64 -D__thread=
BASE := --target=$(TARGET) -ffreestanding -nostdinc -fno-builtin \
        -fno-stack-protector -O2 -g -mno-lsx -mno-lasx -fno-vectorize \
        -fno-slp-vectorize -Wno-unused-parameter -Wno-unused-variable
CFLAGS := $(BASE) $(INCS) -isystem $(RESINC) $(DEFS)
HOST_CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
               -Wno-unused-function -I$(LATR)/include -I$(BUILD) \
               -idirafter $(LATX)/compat -idirafter $(LATX)/compat/include \
               -idirafter $(LATX)/upstream/include

# QEMU-LVZ is an optional override; a checked-in copy under ir1/qemu and
# scripts/decodetree.py keeps the freestanding core buildable on its own.
QEMU_INSNS := $(if $(wildcard $(QEMU_SRC)/target/riscv/insn32.decode),$(QEMU_SRC)/target/riscv,$(LATR)/ir1/qemu)
QEMU_DECODETREE := $(if $(wildcard $(QEMU_SRC)/scripts/decodetree.py),$(QEMU_SRC)/scripts/decodetree.py,$(LATR)/scripts/decodetree.py)

DECODE32 := $(BUILD)/decode-insn32.c.inc
DECODE16 := $(BUILD)/decode-insn16.c.inc
TRANS_STUBS := $(BUILD)/decode-trans-stubs.inc

CORE_OBJS := $(BUILD)/ir2.o $(BUILD)/ir2-assemble.o $(BUILD)/la-append.o \
             $(BUILD)/mem.o $(BUILD)/macro-helper.o $(BUILD)/latr.o \
             $(BUILD)/reg-alloc.o $(BUILD)/translate-riscv.o \
             $(BUILD)/translate-riscv-csr.o $(BUILD)/translate-riscv-fp.o \
             $(BUILD)/translate-riscv-fp-helper.o $(BUILD)/softfloat.o \
             $(BUILD)/decode-riscv.o $(BUILD)/pvir.o $(BUILD)/pvir-exec.o $(BUILD)/pvir-lower-lsx.o $(BUILD)/cpu_features.o $(BUILD)/mmu.o $(BUILD)/interrupt.o $(BUILD)/mmio.o $(BUILD)/rvv.o $(BUILD)/translate-riscv-rvv.o $(BUILD)/tb-cache.o $(BUILD)/tb-insn-map.o \
             $(BUILD)/phys_mem.o $(BUILD)/mmio_bus.o $(BUILD)/latr_system.o $(BUILD)/latr_fdt.o $(BUILD)/latr_machine.o $(BUILD)/uart16550.o $(BUILD)/riscv_clint.o $(BUILD)/riscv_plic.o $(BUILD)/riscv_acpi.o $(BUILD)/riscv_fw.o $(BUILD)/ovmf_board.o $(BUILD)/riscv_sbi.o \
             $(BUILD)/runtime.o \
             $(BUILD)/vclock.o $(BUILD)/compiler-rt.o

.PHONY: all test-decode check probe clean
all: $(BUILD)/liblatr_core.a

test-decode: $(BUILD)/test-latr-decode
	./$(BUILD)/test-latr-decode

check: all
	NM="$(NM)" sh ../tools/check_lata_symbols.sh $(BUILD)/liblatr_core.a $(CURDIR)/latr_externals.allowlist

probe: $(CORE_OBJS)
	@echo "LATR core compiled with $(CLANG)"

$(BUILD):
	@mkdir -p $@

$(DECODE32): $(QEMU_INSNS)/insn32.decode $(QEMU_DECODETREE) | $(BUILD)
	$(PYTHON) $(QEMU_DECODETREE) --decode=decode_insn32 \
		$(QEMU_INSNS)/insn32.decode -o $@

$(DECODE16): $(QEMU_INSNS)/insn16.decode $(QEMU_DECODETREE) $(DECODE32) | $(BUILD)
	$(PYTHON) $(QEMU_DECODETREE) -w 16 --decode=decode_insn16 \
		$(QEMU_INSNS)/insn16.decode -o $@

$(TRANS_STUBS): $(DECODE32) $(DECODE16) $(LATR)/ir1/rv64gc-opcodes.def \
                $(LATR)/ir1/gen_decode_stubs.py | $(BUILD)
	$(PYTHON) $(LATR)/ir1/gen_decode_stubs.py $(DECODE32) $(DECODE16) \
		$(LATR)/ir1/rv64gc-opcodes.def $@

$(BUILD)/ir2.o: $(LATA)/ir2/ir2.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/ir2-assemble.o: $(LATA)/ir2/ir2-assemble.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/la-append.o: $(LATA)/ir2/la-append.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/mem.o: $(LATA)/mem.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/macro-helper.o: $(LATA)/translator/macro-helper.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/latr.o: $(LATR)/latr.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/ir1.h \
                $(LATR)/include/riscv-env.h \
                $(LATR)/ir1/rv64gc-opcodes.def | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/reg-alloc.o: $(LATR)/translator/reg-alloc.c \
                      $(LATR)/include/reg-alloc.h \
                      $(LATR)/include/latr.h \
                      $(LATR)/include/riscv-env.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/translate-riscv.o: $(LATR)/translator/translate-riscv.c \
                            $(LATR)/include/latr.h \
                            $(LATR)/include/translate.h \
                            $(LATR)/include/ir1.h \
                            $(LATR)/include/riscv-env.h \
                            $(LATR)/ir1/rv64gc-opcodes.def | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(BUILD)/translate-riscv-csr.o: $(LATR)/translator/translate-riscv-csr.c \
                                $(LATR)/include/latr.h \
                                $(LATR)/include/translate.h \
                                $(LATR)/include/ir1.h \
                                $(LATR)/include/riscv-env.h \
                                $(LATR)/ir1/rv64gc-opcodes.def | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(BUILD)/translate-riscv-fp.o: $(LATR)/translator/translate-riscv-fp.c \
                               $(LATR)/include/latr.h \
                               $(LATR)/include/ir1.h \
                               $(LATR)/include/riscv-env.h \
                               $(LATA)/include/la-ir2.h \
                               $(LATA)/include/la-append.h \
                               $(LATR)/ir1/rv64gc-opcodes.def | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/translate-riscv-fp-helper.o: \
		$(LATR)/translator/translate-riscv-fp-helper.c \
		$(LATR)/include/riscv-env.h $(LATR)/include/ir1.h \
		$(LATX)/upstream/include/fpu/softfloat.h \
		$(LATX)/upstream/include/fpu/softfloat-types.h \
		$(LATX)/upstream/include/fpu/softfloat-helpers.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/softfloat.o: $(LATX)/upstream/fpu/softfloat.c \
		$(LATX)/upstream/include/fpu/softfloat.h \
		$(LATX)/upstream/include/fpu/softfloat-types.h \
		$(LATX)/upstream/include/fpu/softfloat-helpers.h \
		$(LATX)/upstream/include/fpu/softfloat-macros.h \
		$(LATX)/upstream/fpu/softfloat-specialize.c.inc \
		$(LATX)/upstream/fpu/softfloat-parts.c.inc \
		$(LATX)/upstream/fpu/softfloat-parts-addsub.c.inc | $(BUILD)
# Keep QEMU SoftFloat's optional host-libm fast path out of freestanding LATR.
	$(CLANG) $(CFLAGS) -DTARGET_RISCV -D__FAST_MATH__ -c $< -o $@



$(BUILD)/uart16550.o: ../src/uart16550.c ../include/loongemu/uart16550.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/riscv_plic.o: ../src/riscv_plic.c ../include/loongemu/riscv_plic.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/riscv_sbi.o: ../src/riscv_sbi.c ../include/loongemu/riscv_sbi.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/phys_mem.o: ../src/phys_mem.c ../include/loongemu/phys_mem.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/interrupt.o: $(LATR)/translator/interrupt.c $(LATR)/include/latr.h \
                      $(LATR)/include/riscv-env.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/riscv_acpi.o: ../src/riscv_acpi.c ../include/loongemu/riscv_acpi.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/riscv_fw.o: ../src/riscv_fw.c ../include/loongemu/riscv_fw.h \
                     ../include/loongemu/ovmf_board.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/ovmf_board.o: ../src/ovmf_board.c ../include/loongemu/ovmf_board.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/latr_system.o: $(LATR)/system/latr_system.c \
                $(LATR)/include/latr-system.h $(LATR)/include/latr.h \
                $(LATR)/include/riscv-env.h ../include/loongemu/mmio_bus.h \
                ../include/loongemu/phys_mem.h ../include/loongemu/riscv_clint.h \
                ../include/loongemu/riscv_plic.h ../include/loongemu/uart16550.h \
                ../include/loongemu/vclock.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/riscv_clint.o: ../src/riscv_clint.c ../include/loongemu/riscv_clint.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/rvv.o: $(LATR)/translator/rvv.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/riscv-env.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pvir.o: $(LATA)/pvir.c $(LATA)/include/la-pvir.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pvir-exec.o: $(LATR)/translator/pvir-exec.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/riscv-env.h \
                $(LATA)/include/la-pvir.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pvir-lower-lsx.o: $(LATR)/translator/pvir-lower-lsx.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/riscv-env.h \
                $(LATA)/include/la-pvir.h $(LATX)/compat/qemu/cpu-features.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/cpu_features.o: $(LATX)/runtime/cpu_features.c \
                $(LATX)/compat/qemu/cpu-features.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/translate-riscv-rvv.o: $(LATR)/translator/translate-riscv-rvv.c \
                $(LATR)/include/latr.h $(LATR)/include/translate.h \
                $(LATR)/include/riscv-env.h $(LATR)/include/ir1.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/mmio.o: $(LATR)/translator/mmio.c $(LATR)/include/latr.h \
                 $(LATR)/include/mmu.h $(LATR)/include/riscv-env.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/mmio_bus.o: ../src/mmio_bus.c ../include/loongemu/mmio_bus.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/latr_fdt.o: $(LATR)/system/latr_fdt.c $(LATR)/include/latr-fdt.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/latr_machine.o: $(LATR)/system/latr_machine.c \
                $(LATR)/include/latr-machine.h $(LATR)/include/latr-fdt.h \
                $(LATR)/include/latr-system.h $(LATR)/include/latr.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/tb-cache.o: $(LATR)/tb-cache.c $(LATR)/include/latr.h \
                     $(LATR)/include/riscv-env.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/tb-insn-map.o: $(LATR)/translator/tb-insn-map.c $(LATR)/include/latr.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/mmu.o: $(LATR)/translator/mmu.c $(LATR)/include/mmu.h \
                $(LATR)/include/riscv-env.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/decode-riscv.o: $(LATR)/ir1/decode-riscv.c $(LATR)/include/ir1.h \
                         $(LATR)/ir1/rv64gc-opcodes.def $(DECODE32) $(DECODE16) $(TRANS_STUBS) | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/runtime.o: runtime.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(BUILD)/vclock.o: ../src/vclock.c ../include/loongemu/vclock.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/compiler-rt.o: $(LATX)/runtime/compiler_rt.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/liblatr_core.a: $(CORE_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/test-latr-decode: ../tests/test_latr_decode.c $(LATR)/ir1/decode-riscv.c \
                           $(LATR)/include/ir1.h $(LATR)/ir1/rv64gc-opcodes.def \
                           $(DECODE32) $(DECODE16) $(TRANS_STUBS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(LATR)/ir1/decode-riscv.c \
		../tests/test_latr_decode.c -o $@

clean:
	rm -rf $(BUILD)
