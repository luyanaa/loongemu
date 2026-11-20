# Freestanding LATM (MIPS64r2 -> LoongArch) translation core.
LLVM    ?= $(shell brew --prefix llvm)
CLANG   ?= $(LLVM)/bin/clang
AR      := $(LLVM)/bin/llvm-ar
TARGET  ?= loongarch64-unknown-linux-gnu
RESINC  := $(shell $(CLANG) -print-resource-dir)/include
LATM    := ../target/mips/latm
LATA    := ../target/arm/lata
LATX    := ../latx

INCS := -I$(LATM)/include -I$(LATM)/compat -I$(LATM) -I../target/mips \
        -I$(LATA)/include -I$(LATA) -Icompat -I.. -I$(LATX)/compat \
        -I$(LATX)/include -I$(LATX)/upstream/include -isystem $(RESINC)
DEFS := -DTARGET_MIPS64 -DCONFIG_LATM -DCONFIG_LATM_STANDALONE \
        -D__WORDSIZE=64 -DCONFIG_LATX -D__thread= -DNO_MSA
BASE := --target=$(TARGET) -ffreestanding -nostdinc -fno-builtin \
        -fno-stack-protector -O2 -g -mno-lsx -mno-lasx -fno-vectorize \
        -fno-slp-vectorize -Wno-unused-parameter -Wno-unused-variable \
        -Wno-initializer-overrides -ferror-limit=0
TLS ?= 0
ifeq ($(TLS),1)
DEFS += -DCONFIG_LATM_TRANSLATE_TLS
endif
CFLAGS := $(BASE) $(INCS) $(DEFS)

SHARED := $(LATA)/ir2/ir2.c $(LATA)/ir2/ir2-assemble.c $(LATA)/ir2/la-append.c \
          $(LATA)/mem.c $(LATA)/translator/macro-helper.c
CORE   := $(LATM)/latm.c $(LATM)/tb_insn_map.c $(LATM)/cpu/loongson-profile.c $(LATM)/runtime.c $(LATM)/translator/reg-alloc.c \
          $(LATM)/ir1/decode-mips.c $(LATM)/translator/translate-mips.c \
          ../target/mips/fpu.c
HELPERS := $(LATM)/helpers/qemu_fpu_helper.c $(LATM)/helpers/qemu_ldst_helper.c \
           $(LATM)/helpers/qemu_op_helper.c $(LATM)/helpers/fpu.c \
           $(LATM)/helpers/int.c $(LATM)/helpers/cp0.c \
           $(LATM)/helpers/exception.c $(LATM)/helpers/memory.c \
           $(LATM)/helpers/loongson.c $(LATM)/helpers/math_stubs.c
FPU    := ../fpu/softfloat.c
SRCS   := $(SHARED) $(CORE) $(HELPERS) $(FPU)

.PHONY: all probe clean
all: $(SRCS)
	@mkdir -p build
	@set -e; \
	for f in $(SRCS); do \
	  obj=build/$$(echo $$f | sed 's|^\.\./||; s|/|_|g; s|\.c$$|.o|'); \
	  $(CLANG) $(CFLAGS) -c $$f -o $$obj; \
	done
	@$(AR) rcs build/liblatm_core.a build/*.o
	@echo "  AR  build/liblatm_core.a"

probe:
	@mkdir -p build/probe
	@set -e; \
	for f in $(SRCS); do \
	  if $(CLANG) $(CFLAGS) -fsyntax-only $$f 2>build/probe/$$(basename $$f).log; then \
	    echo "OK    $$f"; \
	  else \
	    echo "FAIL  $$f :: $$(grep -m1 'error:' build/probe/$$(basename $$f).log | sed 's/.*error: //')"; \
	  fi; \
	done

clean:
	rm -rf build
