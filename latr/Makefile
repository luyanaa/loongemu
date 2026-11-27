# Freestanding RISC-V -> LoongArch translation core. QEMU's decoder tables are
# generated from the adjacent QEMU-LVZ checkout, which is also the golden ref.
LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
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
        -I$(LATA)/include -I$(LATA) -I../shared/pvir/include -I../shared/backend/loongarch/include -I../shared/perf/include -I$(LATX)/compat -I$(LATX)/compat/include \
        -I$(LATX)/upstream/include -I../include
RESINC := $(shell $(CLANG) -print-resource-dir)/include
DEFS := -DTARGET_RISCV64 -DCONFIG_LATR -DCONFIG_LATR_STANDALONE \
        -DCONFIG_LATX -D__WORDSIZE=64 -D__thread=

# `make PERF=1` turns on the shared perf counters (TB compile/execute and
# guest access events) so a change can be compared against a build with them
# off. Off by default: the counters are atomics on the hot path.
PERF ?= 0
ifeq ($(PERF),1)
DEFS += -DLOONGEMU_PERF
endif

# Per-opcode IR2 accounting. Off by default: it costs a counter per instruction
# and only exists to answer "which guest opcode emits this code".
LATR_OPCODE_STATS ?= 0
ifeq ($(LATR_OPCODE_STATS),1)
DEFS += -DLATR_OPCODE_STATS=1
endif

# Dump the IR2 stream of the first N blocks (0 = off).
ifneq ($(LATR_IR2_DUMP_BLOCKS),)
DEFS += -DLATR_IR2_DUMP_BLOCKS=$(LATR_IR2_DUMP_BLOCKS)
endif
BASE := --target=$(TARGET) -ffreestanding -nostdinc -fno-builtin \
        -fno-stack-protector -O2 -g -mno-lsx -mno-lasx -fno-vectorize \
        -fno-slp-vectorize -Wno-unused-parameter -Wno-unused-variable
DEFS_HOST := $(DEFS) -D__thread=
CFLAGS := $(BASE) $(INCS) -isystem $(RESINC) $(DEFS)
# `-idirafter ../include`: the repo's include/ is incomplete (it has no
# exec/tb-context.h, and its exec-all.h pulls one in), so the compat shims under
# $(LATX)/compat must be found first. Only the loongemu/* headers are needed from
# ../include, and nothing shadows those.
HOST_CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
               -Wno-unused-function -I$(LATR)/include -I$(BUILD) \
               -Icompat -I.. -I$(LATA)/include -I$(LATA) -I../shared/pvir/include -I../shared/backend/loongarch/include -I../shared/perf/include \
               -idirafter $(LATX)/compat -idirafter $(LATX)/compat/include \
               -idirafter $(LATX)/upstream/include -idirafter ../include

# QEMU-LVZ is an optional override; a checked-in copy under ir1/qemu and
# scripts/decodetree.py keeps the freestanding core buildable on its own.
QEMU_INSNS := $(if $(wildcard $(QEMU_SRC)/target/riscv/insn32.decode),$(QEMU_SRC)/target/riscv,$(LATR)/ir1/qemu)
QEMU_DECODETREE := $(if $(wildcard $(QEMU_SRC)/scripts/decodetree.py),$(QEMU_SRC)/scripts/decodetree.py,$(LATR)/scripts/decodetree.py)

# Hosted build of the same core, for things the freestanding images cannot do:
# pthreads, and a dispatcher harness. Generated code is LoongArch, so the host
# harness scripts block execution instead of jumping into it (see
# baremetal/host/latr_host_smp.c). Kept as a separate build directory on
# purpose: reusing $(BUILD) would mix objects built with different flags.
HOST_BUILD := build-host
HOST_CORE_SRCS := \
            ../shared/backend/loongarch/ir2.c \
            ../shared/backend/loongarch/ir2-peephole.c \
            ../shared/backend/loongarch/ir2-assemble.c \
            ../shared/backend/loongarch/la-append.c \
            ../shared/perf/src/perf.c \
            mem-latr.c \
            ../target/arm/lata/translator/macro-helper.c \
            ../target/riscv/latr/latr.c \
            ../target/riscv/latr/translator/reg-alloc.c \
            ../target/riscv/latr/translator/translate-riscv.c \
            ../target/riscv/latr/translator/translate-riscv-csr.c \
            ../target/riscv/latr/translator/translate-riscv-fp.c \
            ../target/riscv/latr/translator/translate-riscv-fp-helper.c \
            ../target/riscv/latr/ir1/decode-riscv.c \
            ../shared/pvir/src/pvir.c \
            ../shared/pvir/src/pvir-plan.c \
            ../target/riscv/latr/translator/pvir-exec.c \
            ../target/riscv/latr/translator/pvir-lower-lsx.c \
            ../latx/runtime/cpu_features.c \
            ../target/riscv/latr/translator/mmu.c \
            ../target/riscv/latr/translator/interrupt.c \
            ../target/riscv/latr/translator/mmio.c \
            ../target/riscv/latr/translator/rvv.c \
            ../target/riscv/latr/translator/rvv-legality.c \
            ../target/riscv/latr/translator/translate-riscv-rvv.c \
            ../target/riscv/latr/tb-cache.c \
            ../target/riscv/latr/translator/tb-insn-map.c \
            ../target/riscv/latr/hart-sync.c \
            ../target/riscv/latr/chain.c \
            ../src/soft_ras.c \
            ../src/phys_mem.c \
            ../src/mmio_bus.c \
            ../src/guest_dma.c \
            ../src/riscv_virtio_blk.c \
            ../target/riscv/latr/system/latr_system.c \
            ../target/riscv/latr/system/latr_elf.c \
            ../target/riscv/latr/system/latr_fdt.c \
            ../target/riscv/latr/system/latr_machine.c \
            ../src/uart16550.c \
            ../src/riscv_clint.c \
            ../src/riscv_plic.c \
            ../src/riscv_acpi.c \
            ../src/riscv_fw.c \
            ../src/ovmf_board.c \
            ../src/riscv_sbi.c \
            runtime.c \
            ../src/vclock.c

DECODE32 := $(BUILD)/decode-insn32.c.inc
DECODE16 := $(BUILD)/decode-insn16.c.inc
TRANS_STUBS := $(BUILD)/decode-trans-stubs.inc

CORE_OBJS := $(BUILD)/ir2.o $(BUILD)/ir2-peephole.o $(BUILD)/ir2-assemble.o $(BUILD)/la-append.o $(BUILD)/perf.o \
             $(BUILD)/mem.o $(BUILD)/macro-helper.o $(BUILD)/latr.o \
             $(BUILD)/reg-alloc.o $(BUILD)/translate-riscv.o \
             $(BUILD)/translate-riscv-csr.o $(BUILD)/translate-riscv-fp.o \
             $(BUILD)/translate-riscv-fp-helper.o $(BUILD)/softfloat.o \
             $(BUILD)/decode-riscv.o $(BUILD)/pvir.o $(BUILD)/pvir-plan.o $(BUILD)/pvir-exec.o $(BUILD)/pvir-lower-lsx.o $(BUILD)/cpu_features.o $(BUILD)/mmu.o $(BUILD)/interrupt.o $(BUILD)/mmio.o $(BUILD)/rvv.o $(BUILD)/rvv-legality.o $(BUILD)/translate-riscv-rvv.o $(BUILD)/tb-cache.o $(BUILD)/tb-insn-map.o $(BUILD)/hart-sync.o $(BUILD)/chain.o $(BUILD)/soft_ras.o \
             $(BUILD)/phys_mem.o $(BUILD)/mmio_bus.o $(BUILD)/guest_dma.o $(BUILD)/riscv_virtio_blk.o $(BUILD)/latr_system.o $(BUILD)/latr_elf.o $(BUILD)/latr_fdt.o $(BUILD)/latr_machine.o $(BUILD)/uart16550.o $(BUILD)/riscv_clint.o $(BUILD)/riscv_plic.o $(BUILD)/riscv_acpi.o $(BUILD)/riscv_fw.o $(BUILD)/ovmf_board.o $(BUILD)/riscv_sbi.o \
             $(BUILD)/runtime.o \
             $(BUILD)/vclock.o $(BUILD)/compiler-rt.o

# Every core object depends on every core header. The per-object rules list only
# the headers they obviously use, but several headers embed others (LatrSystem
# embeds LoongemuPlic, LatrMachineHart embeds LatrChainControl/SoftRas), so a
# struct change in a transitively included header otherwise leaves objects with
# the old layout linked against new ones - which shows up at run time as a NULL
# code buffer or a zero dtb address, not as a build error. Recompiling 45 files
# on a header edit is a few seconds and removes that class of bug.
LATR_CORE_HEADERS := $(wildcard $(LATR)/include/*.h) \
                     $(wildcard ../shared/pvir/include/*.h) \
                     $(wildcard ../shared/backend/loongarch/include/*.h) \
                     $(wildcard ../include/loongemu/*.h)
$(CORE_OBJS): $(LATR_CORE_HEADERS)

.PHONY: all test-decode test-legality test-elf test-tb-smp test-fence-ack test-hart-sync test-host-machine test-host-machine-stress test-machine-trap check probe clean
all: $(BUILD)/liblatr_core.a

test-decode: $(BUILD)/test-latr-decode
	./$(BUILD)/test-latr-decode

test-legality: $(BUILD)/test-latr-rvv-legality
	./$(BUILD)/test-latr-rvv-legality

test-elf: $(BUILD)/test-latr-elf
	./$(BUILD)/test-latr-elf

test-tb-smp: $(BUILD)/test-latr-tb-smp
	./$(BUILD)/test-latr-tb-smp

$(HOST_BUILD):
	@mkdir -p $@

# The staged core is not a warning gate: it is 45 translator files compiled for
# a host whose compiler (gcc on the CI runner) has never been validated with
# -Werror, and a spurious -Wmaybe-uninitialized there would fail a job for no
# defect. The freestanding build stays the warning gate; the harness file itself
# is compiled with -Werror below so our own code is held to the same bar.
HOST_CORE_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra \
                    -Wno-unused-parameter -Wno-unused-function \
                    -Wno-unused-variable -Wno-sign-compare \
                    -Wno-unused-but-set-variable -pthread -DLATR_SMP_THREADS -DLATR_ENABLE_TLS \
                    $(INCS) $(DEFS_HOST)
HOST_STRICT_CFLAGS := $(HOST_CORE_CFLAGS) -Werror

# Same flags as the freestanding object: the compat include paths supply
# qemu/osdep.h, and __FAST_MATH__ must not leak into any other file.
$(HOST_BUILD)/softfloat-host.o: $(LATX)/upstream/fpu/softfloat.c | $(HOST_BUILD)
	$(HOST_CC) $(HOST_CORE_CFLAGS) -DTARGET_RISCV -D__FAST_MATH__ \
		-fno-strict-aliasing -c $< -o $@

# Depends on every header it can pull in: the harness is compiled once and then
# linked against a freshly compiled core, so a stale object with old struct
# layouts (LatrSystem, LatrMachineHart) crashes at run time instead of failing
# the build. Header mtime is a cheap proxy for "recompile it".
HARNESS_HEADERS := $(wildcard $(LATR)/include/*.h) $(wildcard ../include/loongemu/*.h) \
                   $(wildcard ../shared/pvir/include/*.h)

$(HOST_BUILD)/latr_host_smp.o: ../baremetal/host/latr_host_smp.c \
                               $(HARNESS_HEADERS) | $(HOST_BUILD)
	$(HOST_CC) $(HOST_STRICT_CFLAGS) -c $< -o $@

$(HOST_BUILD)/latr-host-smp: $(HOST_BUILD)/latr_host_smp.o $(HOST_CORE_SRCS) \
                             $(HOST_BUILD)/softfloat-host.o \
                             $(DECODE32) $(DECODE16) $(TRANS_STUBS) | $(HOST_BUILD)
	$(HOST_CC) $(HOST_CORE_CFLAGS) $(HOST_BUILD)/latr_host_smp.o \
		$(HOST_CORE_SRCS) $(HOST_BUILD)/softfloat-host.o -o $@

test-host-machine: $(HOST_BUILD)/latr-host-smp
	./$(HOST_BUILD)/latr-host-smp

# A threaded harness passes once by winning a race; it is only trustworthy if it
# passes every time. Repeat it HOST_SMP_STRESS times and require both a zero exit
# and an identical summary line, so a lost wake, a lost fence ack or a check that
# silently stops running cannot hide in a lucky interleaving.
HOST_SMP_STRESS ?= 100

test-host-machine-stress: $(HOST_BUILD)/latr-host-smp
	@first=''; i=1; \
	while [ $$i -le $(HOST_SMP_STRESS) ]; do \
	    out=`./$(HOST_BUILD)/latr-host-smp` || { \
	        echo "latr-host-smp: iteration $$i FAILED (exit $$?)"; exit 1; }; \
	    sig=`printf '%s\n' "$$out" | tail -1`; \
	    if [ -z "$$first" ]; then \
	        first="$$sig"; \
	    elif [ "$$sig" != "$$first" ]; then \
	        echo "latr-host-smp: iteration $$i differs"; \
	        echo "  first: $$first"; \
	        echo "  now:   $$sig"; \
	        exit 1; \
	    fi; \
	    i=`expr $$i + 1`; \
	done; \
	echo "latr-host-smp: PASS $(HOST_SMP_STRESS) iterations: $$first"

test-fence-ack: $(BUILD)/test-latr-fence-ack
	./$(BUILD)/test-latr-fence-ack

test-hart-sync: $(BUILD)/test-latr-hart-sync
	./$(BUILD)/test-latr-hart-sync

test-chain: $(BUILD)/test-latr-chain
	./$(BUILD)/test-latr-chain

test-machine-trap: $(BUILD)/test-latr-machine-trap
	./$(BUILD)/test-latr-machine-trap

check: all test-legality test-elf test-tb-smp test-fence-ack test-hart-sync test-chain test-host-machine test-machine-trap
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

$(BUILD)/ir2.o: ../shared/backend/loongarch/ir2.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/ir2-peephole.o: ../shared/backend/loongarch/ir2-peephole.c \
                         ../shared/backend/loongarch/include/ir2.h \
                         ../shared/backend/loongarch/include/ir2-sideeffect.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/ir2-assemble.o: ../shared/backend/loongarch/ir2-assemble.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/la-append.o: ../shared/backend/loongarch/la-append.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/perf.o: ../shared/perf/src/perf.c ../shared/perf/include/loongemu-perf.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(BUILD)/mem.o: mem-latr.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/macro-helper.o: $(LATA)/translator/macro-helper.c | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/latr.o: $(LATR)/latr.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/ir1.h \
                $(LATR)/include/riscv-env.h \
                ../shared/perf/include/loongemu-perf.h \
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
                               ../shared/backend/loongarch/include/la-ir2.h \
                               ../shared/backend/loongarch/include/la-append.h \
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

$(BUILD)/guest_dma.o: ../src/guest_dma.c ../include/loongemu/guest_dma.h \
                ../include/loongemu/phys_mem.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/riscv_virtio_blk.o: ../src/riscv_virtio_blk.c \
                ../include/loongemu/riscv_virtio_blk.h \
                ../include/loongemu/guest_dma.h \
                ../include/loongemu/mmio_bus.h ../include/loongemu/phys_mem.h | $(BUILD)
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

$(BUILD)/latr_elf.o: $(LATR)/system/latr_elf.c $(LATR)/include/latr-elf.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/latr_system.o: $(LATR)/system/latr_system.c \
                $(LATR)/include/latr-elf.h \
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

$(BUILD)/pvir.o: ../shared/pvir/src/pvir.c ../shared/pvir/include/la-pvir.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pvir-plan.o: ../shared/pvir/src/pvir-plan.c \
                ../shared/pvir/include/pvir-plan.h ../shared/pvir/include/la-pvir.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pvir-exec.o: $(LATR)/translator/pvir-exec.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/riscv-env.h \
                ../shared/pvir/include/la-pvir.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/pvir-lower-lsx.o: $(LATR)/translator/pvir-lower-lsx.c $(LATR)/include/latr.h \
                $(LATR)/include/translate.h $(LATR)/include/riscv-env.h \
                ../shared/pvir/include/la-pvir.h $(LATX)/compat/qemu/cpu-features.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/cpu_features.o: $(LATX)/runtime/cpu_features.c \
                $(LATX)/compat/qemu/cpu-features.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/translate-riscv-rvv.o: $(LATR)/translator/translate-riscv-rvv.c \
                $(LATR)/include/latr.h $(LATR)/include/translate.h \
                $(LATR)/include/riscv-env.h $(LATR)/include/ir1.h \
                $(LATR)/include/latr-rvv-legality.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/rvv-legality.o: $(LATR)/translator/rvv-legality.c \
                $(LATR)/include/latr-rvv-legality.h $(LATR)/include/ir1.h \
                $(LATR)/ir1/rv64gc-opcodes.def | $(BUILD)
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

$(BUILD)/hart-sync.o: $(LATR)/hart-sync.c $(LATR)/include/latr-hart-sync.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/chain.o: $(LATR)/chain.c $(LATR)/include/latr-chain.h \
                  ../include/loongemu/soft_ras.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/soft_ras.o: ../src/soft_ras.c ../include/loongemu/soft_ras.h | $(BUILD)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD)/tb-cache.o: $(LATR)/tb-cache.c $(LATR)/include/latr-tb-cache.h | $(BUILD)
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

$(BUILD)/test-latr-rvv-legality: ../tests/test_latr_rvv_legality.c \
                           $(LATR)/translator/rvv-legality.c \
                           $(LATR)/ir1/decode-riscv.c \
                           $(LATR)/include/latr-rvv-legality.h \
                           $(LATR)/include/ir1.h $(LATR)/ir1/rv64gc-opcodes.def \
                           $(DECODE32) $(DECODE16) $(TRANS_STUBS) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(LATR)/translator/rvv-legality.c \
		$(LATR)/ir1/decode-riscv.c ../tests/test_latr_rvv_legality.c -o $@

$(BUILD)/test-latr-chain: ../tests/test_latr_chain.c \
                          $(LATR)/chain.c $(LATR)/tb-cache.c \
                          $(LATR)/hart-sync.c $(LATR)/include/latr-chain.h \
                          ../src/soft_ras.c ../include/loongemu/soft_ras.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(LATR)/chain.c $(LATR)/tb-cache.c \
		$(LATR)/hart-sync.c ../src/soft_ras.c \
		../tests/test_latr_chain.c -o $@

$(BUILD)/test-latr-hart-sync: ../tests/test_latr_hart_sync.c \
                           $(LATR)/hart-sync.c $(LATR)/include/latr-hart-sync.h | $(BUILD)
	$(HOST_CC) -pthread $(HOST_CFLAGS) $(LATR)/hart-sync.c \
		../tests/test_latr_hart_sync.c -o $@

$(BUILD)/test-latr-fence-ack: ../tests/test_latr_fence_ack.c \
                           $(LATR)/tb-cache.c $(LATR)/hart-sync.c \
                           $(LATR)/include/latr-tb-cache.h $(LATR)/include/latr-hart-sync.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(LATR)/tb-cache.c $(LATR)/hart-sync.c \
		../tests/test_latr_fence_ack.c -o $@

$(BUILD)/test-latr-tb-smp: ../tests/test_latr_tb_smp.c \
                           $(LATR)/tb-cache.c $(LATR)/hart-sync.c \
                           $(LATR)/include/latr-tb-cache.h $(LATR)/include/latr-hart-sync.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(LATR)/tb-cache.c $(LATR)/hart-sync.c \
		../tests/test_latr_tb_smp.c -o $@

# Runtime synchronous-exception entry (native fault -> guest trap handler).
$(BUILD)/test-latr-machine-trap: ../tests/test_latr_machine_trap.c \
                          $(LATR)/translator/interrupt.c \
                          ../src/vclock.c ../src/riscv_plic.c ../src/mmio_bus.c \
                          $(LATR)/include/latr.h $(LATR)/include/riscv-env.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(DEFS_HOST) \
		$(LATR)/translator/interrupt.c ../src/vclock.c \
		../src/riscv_plic.c ../src/mmio_bus.c \
		../tests/test_latr_machine_trap.c -o $@

$(BUILD)/test-latr-elf: ../tests/test_latr_elf.c $(LATR)/system/latr_elf.c \
                        $(LATR)/include/latr-elf.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(LATR)/system/latr_elf.c \
		../tests/test_latr_elf.c -o $@

clean:
	rm -rf $(BUILD)
