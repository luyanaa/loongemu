CLANG   ?= /opt/homebrew/Cellar/llvm/23.1.5_1/bin/clang
AR      := /opt/homebrew/Cellar/llvm/23.1.5_1/bin/llvm-ar
TARGET  ?= loongarch64-unknown-linux-gnu
RESINC  := $(shell $(CLANG) -print-resource-dir)/include
LATX    := ../latx
CAPSTONE := ../latx/capstone_git
QEMUSRC := ../latx/upstream/include

INCS := -I$(LATX)/compat -I$(LATX)/compat/libm -I$(LATX)/qemu -I$(LATX)/runtime -Igen/latx/include -I$(LATX)/include -I$(LATX) \
        -I$(CAPSTONE)/include -I$(QEMUSRC) -I../include -isystem $(RESINC)
DEFS := -DTARGET_X86_64 -DCONFIG_LATX -DCONFIG_LATX_FLAG_REDUCTION -DCONFIG_LATX_AVX_OPT \
        -DCONFIG_LATX_CAPSTONE_GIT -DCAPSTONE_HAS_X86 -DCAPSTONE_USE_SYS_DYN_MEM=1 \
        -D__thread=
BASE := --target=$(TARGET) -ffreestanding -nostdinc -fno-builtin \
        -fno-stack-protector -O2 -g -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize -Wno-unused-parameter -Wno-unused-variable
CFLAGS := $(BASE) $(INCS) $(DEFS) -include latx_bare_prelude.h

TRANSLATOR := reg-alloc tr-arith tr-eflag tr-eflag-process tr-farith tr-fcmovcc \
  tr-fctrl tr-fldst tr-jcc tr-logic tr-misc tr-mov tr-opnd-process tr-setcc \
  tr-btx tr-lock tr-lock-fast-atomic tr-simd tr-vpaes tr-simd-cmp tr-simd-cvt \
  tr-simd-mov tr-simd-shift tr-simd-fma tr-simd-sha tr-simd-f16c \
  tr-simd-f16c-lsx tr-simd-cvt-lsx tr-avx tr-avx-lsx tr-avx-fma-lsx tr-avx-cmp \
  tr-avx-cmp-lsx tr-avx-cvt tr-avx-cvt-lsx tr-avx-mov tr-avx-mov-lsx \
  tr-avx-shift tr-avx-shift-lsx tr-vpaes-lsx tr-bmi tr-adx tr-softfpu \
  tr-pattern translate runtime-trace mem-interface macro-helper

LIBM_SRC := $(wildcard $(LATX)/compat/libm/*.c)
CAP_ROOT  := $(CAPSTONE)
CAPSTONE_SRC := $(CAP_ROOT)/cs.c $(CAP_ROOT)/MCInst.c $(CAP_ROOT)/MCInstrDesc.c \
                $(CAP_ROOT)/MCRegisterInfo.c $(CAP_ROOT)/MCInstPrinter.c \
                $(CAP_ROOT)/Mapping.c $(CAP_ROOT)/SStream.c $(CAP_ROOT)/utils.c \
                $(CAP_ROOT)/arch/X86/X86Disassembler.c \
                $(CAP_ROOT)/arch/X86/X86DisassemblerDecoder.c \
                $(CAP_ROOT)/arch/X86/X86InstPrinterCommon.c \
                $(CAP_ROOT)/arch/X86/X86ATTInstPrinter.c \
                $(CAP_ROOT)/arch/X86/X86IntelInstPrinter.c \
                $(CAP_ROOT)/arch/X86/X86Mapping.c $(CAP_ROOT)/arch/X86/X86Module.c
RUNTIME_SRC := $(wildcard $(LATX)/runtime/*.c)
QEMU_SRC := $(LATX)/qemu/fpu_helper.c $(LATX)/qemu/int_helper.c \
            $(LATX)/qemu/misc_helper.c $(LATX)/qemu/cc_helper.c \
            $(LATX)/qemu/softfloat.c $(LATX)/qemu/aes.c

SRCS := ../src/x86_64_paging.c ../src/phys_mem.c ../src/vclock.c ../src/loongarch_smp.c ../src/smbios_tables.c ../src/ovmf_board.c ../src/pci_host.c $(LATX)/compat/miniglib.c $(LATX)/compat/libc_min.c $(LATX)/compat/ldmath.c $(LIBM_SRC) $(QEMU_SRC) $(RUNTIME_SRC) $(CAPSTONE_SRC) \
        $(addprefix $(LATX)/translator/,$(addsuffix .c,$(TRANSLATOR))) \
        $(LATX)/ir1/ir1.c \
        $(LATX)/ir2/ir2.c $(LATX)/ir2/ir2-assemble.c $(LATX)/ir2/ir2-relocate.c \
        $(LATX)/ir2/ir2-sw-lbt.c \
        gen/latx/ir2/la-append.c $(LATX)/ir2/la-decode.c $(LATX)/ir2/la-disasm.c \
        $(LATX)/optimization/flag-lbt.c $(LATX)/optimization/flag-reduction.c \
        $(LATX)/optimization/ir1-optimization.c $(LATX)/optimization/ir2-optimization.c

GEN := gen/latx/include/format-table.h gen/latx/include/ir2-name.h \
       gen/latx/include/ir2-opcode.h gen/latx/include/la-append.h gen/latx/ir2/la-append.c

.PHONY: all gen probe clean
all: gen
	@mkdir -p build
	@for f in $(SRCS); do \
	  obj=build/$$(basename $$f .c).o; \
	  printf '  CC  %s\n' "$$f"; \
	  $(CLANG) $(CFLAGS) -c $$f -o $$obj || exit 1; \
	done
	@$(AR) rcs build/liblatx_core.a build/*.o
	@printf '  AR  build/liblatx_core.a (%s members)\n' "$$(ls build/*.o | wc -l | tr -d ' ')"

gen: $(GEN)

$(GEN):
	@mkdir -p gen/latx/include gen/latx/ir2
	@ln -sfn ../../../$(LATX)/inst_template.json gen/latx/inst_template.json
	@cd gen/latx && python3 ../../../$(LATX)/convert.py >/dev/null

probe: gen
	@mkdir -p build/probe
	@for f in $(SRCS); do \
	  if $(CLANG) $(CFLAGS) -fsyntax-only $$f 2>build/probe/$$(basename $$f).log; then \
	    printf 'OK    %s\n' "$$f"; \
	  else \
	    printf 'FAIL  %s :: %s\n' "$$(basename $$f)" "$$(grep -m1 -E 'error:' build/probe/$$(basename $$f).log | sed 's/.*error: //')"; \
	  fi; \
	done

clean:
	rm -rf build gen
