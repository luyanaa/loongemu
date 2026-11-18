#!/bin/sh
# Build a LoongArch64 EFI application (PE32+) from a freestanding ELF object.
# Verified chain (2026-11-17): clang 23.1.5 object -> ld.lld ELF -> edk2 GenFw PE.
# lld-link rejects /machine:loongarch64, so GenFw is the only local PE producer.
set -eu

CLANG=${CLANG:-/opt/homebrew/Cellar/llvm/23.1.5_1/bin/clang}
LLD=${LLD:-/opt/homebrew/opt/lld/bin/ld.lld}
GENFW=${GENFW:-/tmp/edk2/BaseTools/Source/C/bin/GenFw}
READOBJ=${READOBJ:-/opt/homebrew/Cellar/llvm/23.1.5_1/bin/llvm-readobj}

src=$1
out=${2:-$(basename "$src" .c).efi}
work=$(dirname "$out")
obj=$work/app.o
elf=$work/app.elf
lds=$work/app.lds

cat >"$lds" <<'EOF'
ENTRY(efi_main)
SECTIONS {
  . = 0x1000;
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
}
EOF

"$CLANG" --target=loongarch64-unknown-none-elf -ffreestanding -fno-builtin \
         -nostdlib -O2 -c "$src" -o "$obj"
"$LLD" -T "$lds" -e efi_main "$obj" -o "$elf"
"$GENFW" -o "$out" -e UEFI_APPLICATION "$elf"
"$READOBJ" --file-headers "$out" | grep -E 'Machine|Subsystem'
