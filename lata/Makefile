# Bare build for the LATA (ARM64 -> LoongArch) translation core.
CLANG   ?= /opt/homebrew/Cellar/llvm/23.1.5_1/bin/clang
AR      := /opt/homebrew/Cellar/llvm/23.1.5_1/bin/llvm-ar
TARGET  ?= loongarch64-unknown-linux-gnu
RESINC  := $(shell $(CLANG) -print-resource-dir)/include
LATA    := ../target/arm/lata
LATX    := ../latx

INCS := -Icompat -I.. -I$(LATA)/include -I$(LATA) -I$(LATX)/compat -I$(LATX)/upstream/include \
         -isystem $(RESINC)
DEFS := -DTARGET_AARCH64 -DCONFIG_LATA -DCONFIG_LATA_TU -D__WORDSIZE=64 \
        -DCONFIG_LATX -D__thread=
BASE := --target=$(TARGET) -ffreestanding -nostdinc -fno-builtin \
        -fno-stack-protector -O2 -g -mno-lsx -mno-lasx -fno-vectorize \
        -fno-slp-vectorize -Wno-unused-parameter -Wno-unused-variable -ferror-limit=0
CFLAGS := $(BASE) $(INCS) $(DEFS)

SRCS := $(LATA)/ir2/ir2.c $(LATA)/ir2/ir2-assemble.c $(LATA)/ir2/la-append.c \
        $(LATA)/mem.c $(LATA)/lata.c runtime.c \
        $(LATA)/optimization/clear-high.c $(LATA)/optimization/insts-pattern.c \
        $(LATA)/optimization/tu.c glib_tree.c \
        $(LATA)/ir1/ir1.c \
        $(LATA)/translator/reg-alloc.c $(LATA)/translator/macro-helper.c \
        $(LATA)/translator/translate-a64.c

.PHONY: all probe clean
all: $(SRCS)
	@mkdir -p build
	@for f in $(SRCS); do obj=build/$$(basename $$f .c).o; \
	  $(CLANG) $(CFLAGS) -c $$f -o $$obj || exit 1; done
	@$(AR) rcs build/liblata_core.a build/*.o
	@echo "  AR  build/liblata_core.a"

probe: 
	@mkdir -p build/probe
	@for f in $(SRCS); do \
	  if $(CLANG) $(CFLAGS) -fsyntax-only $$f 2>build/probe/$$(basename $$f).log; then \
	    echo "OK    $$f"; \
	  else \
	    echo "FAIL  $$(basename $$f) :: $$(grep -m1 'error:' build/probe/$$(basename $$f).log | sed 's/.*error: //')"; \
	  fi; \
	done

clean:
	rm -rf build
