ENTRY(_start)
SECTIONS {
  /*
   * The VMM lives above 64 MiB so the low 64 MiB is guest-addressable: the
   * page-fault path maps guest physical addresses from 0, and a kernel image
   * needs far more than the 2 MiB that used to be available below the VMM.
   */
  . = 0x8000000;
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
  _end = .;
}
