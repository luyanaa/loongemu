# Host build for the portable LoongEMU pieces.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Iinclude
LBT_SRC := src/loongbt_semantics.c src/loongbt_cpu.c src/loongbt_hw.c src/loongbt_ir2.c

CLANG   ?= /opt/homebrew/Cellar/llvm/23.1.5_1/bin/clang
OBJDUMP ?= /opt/homebrew/Cellar/llvm/23.1.5_1/bin/llvm-objdump

.PHONY: all test test-cpu test-lapaging test-tbc test-p0 vmm-efi vmm-boot test-paging test-libc test-libm test-loongarch test-loongarch-fallback efi-selftest efi-boot clean

all: test

build/test_loongbt: tests/test_loongbt.c $(LBT_SRC) include/loongemu/loongbt.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaremetal/gen/latx/include tests/test_loongbt.c $(LBT_SRC) -o $@

build/test_tb_cache: tests/test_tb_cache.c src/tb_cache.c include/loongemu/tb_cache.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/tb_cache.c tests/test_tb_cache.c -o $@

test-tbc: build/test_tb_cache
	./build/test_tb_cache

test: build/test_loongbt build/test_loongbt_cpu build/test_loongarch_paging \
      build/test_tb_cache
	./build/test_loongbt
	./build/test_loongbt_cpu
	./build/test_loongarch_paging
	./build/test_tb_cache

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
	/opt/homebrew/opt/lld/bin/ld.lld -T baremetal/bare/bare.lds -e _start \
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
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T build/efi.lds -e efi_main \
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
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T build/ir1_smoke.lds -e efi_main \
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
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T build/lvz_probe.lds -e efi_main build/lvz_probe.o -o $@

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
LATA_BARE_CFLAGS := --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
               -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
               -DTARGET_AARCH64 -DCONFIG_LATA -DCONFIG_LATA_TU \
               -D__WORDSIZE=64 -D__thread= \
               -I lata/compat -I . -I target/arm/lata/include -I target/arm/lata \
               -I latx/compat -I latx/include -I latx/upstream/include \
               -isystem $(shell $(CLANG) -print-resource-dir)/include

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
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/lata_isa_test.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a -o $@

build/bare_lata.elf: build/lata_smoke.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a baremetal/bare/bare.lds
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/lata_smoke.o build/lata_miniglib.o build/lata_libc_min.o lata/build/liblata_core.a -o $@

build/lbt_smoke.o: baremetal/bare/lbt_smoke.c
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
	         -c $< -o $@

build/lbt_smoke.elf: build/lbt_smoke.o baremetal/bare/bare.lds
	/opt/homebrew/opt/lld/bin/ld.lld -T baremetal/bare/bare.lds -e _start $< -o $@

QLVZ ?= $(HOME)/Documents/QEMU-LVZ/build/qemu-system-loongarch64
QLVZ_REPO ?= $(CURDIR)/qemu-system-loongarch64
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

build/bare_main.o: baremetal/bare/main.c baremetal/bare/lbt_emu.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Ibaremetal/bare -c $< -o $@

build/bare_latx.elf: build/bare_main.o build/lbt_emu.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/bare_main.o build/lbt_emu.o baremetal/build/liblatx_core.a -o $@

# ---- P1: multi-TB dynamic execution loop ----
build/tb_loop.o: baremetal/bare/tb_loop.c include/loongemu/tb_cache.h baremetal/bare/lbt_emu.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/lbt_emu.o: baremetal/bare/lbt_emu.c baremetal/bare/lbt_emu.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/tb_cache_bare.o: src/tb_cache.c include/loongemu/tb_cache.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -c $< -o $@

build/tb_harness.o: baremetal/bare/tb_harness.c baremetal/bare/tb_harness.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/tb_loop.elf: build/tb_loop.o build/tb_harness.o build/tb_cache_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/tb_loop.o build/tb_harness.o build/tb_cache_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a -o $@

tb-loop-image: build/tb_loop.elf

build/loongarch_paging_bare.o: src/loongarch_paging.c include/loongemu/loongarch_paging.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -c $< -o $@

build/acpi_tables_bare.o: src/acpi_tables.c
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/tb_sys.o: baremetal/bare/tb_sys.c include/loongemu/tb_cache.h include/loongemu/loongarch_paging.h baremetal/bare/lbt_emu.h baremetal/bare/tb_harness.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/vmlinux_text.o: baremetal/bare/vmlinux_text.S firmware/vmlinux-x86_64-text.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib -c $< -o $@

build/vmlinux_segs.o: baremetal/bare/vmlinux_segs.S firmware/vmlinux-x86_64-seg1.bin firmware/vmlinux-x86_64-seg2.bin firmware/vmlinux-x86_64-seg3.bin
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib -c $< -o $@

build/tb_sys.elf: build/tb_sys.o build/tb_harness.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o build/acpi_tables_bare.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/tb_sys.o build/tb_harness.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o build/acpi_tables_bare.o build/vmlinux_text.o build/vmlinux_segs.o baremetal/build/liblatx_core.a -o $@

tb-sys-image: build/tb_sys.elf

build/regress_x86.o: baremetal/bare/regress_x86.c include/loongemu/tb_cache.h include/loongemu/loongarch_paging.h baremetal/bare/lbt_emu.h baremetal/bare/tb_harness.h
	@mkdir -p build
	$(CLANG) $(BARE_CFLAGS) -Iinclude -Ibaremetal/bare -c $< -o $@

build/regress_x86.elf: build/regress_x86.o build/tb_harness.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a baremetal/bare/bare.lds
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T baremetal/bare/bare.lds -e _start \
	   build/regress_x86.o build/tb_harness.o build/tb_cache_bare.o build/loongarch_paging_bare.o build/lbt_emu.o baremetal/build/liblatx_core.a -o $@

regress-x86-image: build/regress_x86.elf

tb-sys-run: build/tb_sys.elf
	@rm -f build/tb_sys.log
	@timeout 15 $(QLVZ) -M virt -cpu max -m 512M -kernel build/tb_sys.elf \
	     -display none -monitor none -serial file:build/tb_sys.log -no-reboot 2>/dev/null || true
	@grep -a 'tb-sys:' build/tb_sys.log | tail -8
	@grep -qa 'tb-sys: done' build/tb_sys.log
	@grep -qa 'fails=0' build/tb_sys.log

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

# ---- P0 UEFI entry: ExitBootServices handoff and the 4 KiB bring-up ----
VMM_SRC := baremetal/efi/vmm_entry.c src/loongarch_paging.c
VMM_INC := -Iinclude -Ibaremetal/efi

build/vmm_entry.o: $(VMM_SRC) include/loongemu/loongarch_paging.h baremetal/efi/efi_min.h
	@mkdir -p build
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
	         $(VMM_INC) -c baremetal/efi/vmm_entry.c -o $@
	$(CLANG) --target=loongarch64-unknown-none-elf -ffreestanding -nostdlib \
	         -fno-builtin -O2 -mno-lsx -mno-lasx -fno-vectorize -fno-slp-vectorize \
	         $(VMM_INC) -c src/loongarch_paging.c -o build/vmm_loongarch_paging.o

build/vmm_entry.elf: build/vmm_entry.o build/vmm_loongarch_paging.o
	@printf 'ENTRY(efi_main)\nSECTIONS { . = 0x1000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n' > build/vmm.lds
	/opt/homebrew/opt/lld/bin/ld.lld --emit-relocs -T build/vmm.lds -e efi_main \
	   build/vmm_entry.o build/vmm_loongarch_paging.o -o $@

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
	     -display none -monitor none -serial file:build/vmm-serial.log -no-reboot & \
	   echo $$! > build/qemu_vmm.pid )
	@sleep 40; kill $$(cat build/qemu_vmm.pid) 2>/dev/null; true
	@grep -aE 'vmm:' build/vmm-serial.log | head -30
