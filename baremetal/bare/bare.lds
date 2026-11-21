ENTRY(_start)
SECTIONS {
  /*
   * The VMM begins at 128 MiB, immediately above the fixed guest RAM window.
   * Keep its writable fallback framebuffer inside the protected image extent
   * so the guest cannot alias VMM-owned pages.
   */
  . = 0x8000000;
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
  /*
   * Guest-visible backing buffers must sit outside _end: the VMM image
   * range is registered as host-protected and may not alias guest RAM/ROM.
   */
  _end = .;
  . = ALIGN(4096);
  .fb_fallback (NOLOAD) : { *(.fb_fallback*) }
  . = ALIGN(4096);
  .ovmf_store (NOLOAD) : { *(.ovmf_store*) }
  _fb_fallback_end = .;
}
