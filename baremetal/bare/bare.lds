ENTRY(_start)
PHDRS {
  text      PT_LOAD FLAGS(5); /* r-x */
  rodata    PT_LOAD FLAGS(4); /* r-- */
  data      PT_LOAD FLAGS(6); /* rw- */
  guestram  PT_LOAD FLAGS(6); /* rw-, isolated so it never merges with .bss */
}
SECTIONS {
  /*
   * The VMM begins at 128 MiB, immediately above the fixed guest RAM window.
   * Keep its writable fallback framebuffer inside the protected image extent
   * so the guest cannot alias VMM-owned pages.
   */
  . = 0x8000000;
  _stext = .;
  .text   : { *(.text*) } :text
  _etext = .;
  .rodata : { *(.rodata*) } :rodata
  .data   : { *(.data*) } :data
  .bss    : { *(.bss*) *(COMMON) } :data
  /*
   * Orphan sections with PC-relative references (.got in particular) must
   * stay within pcalau12i reach of .text (+-2 GiB). The .guest_ram backing
   * store below sits at 0x80000000; if the orphan .got were placed after it,
   * cross-TU function pointers (latm_mmio_ram_read et al.) would resolve to
   * garbage and any indirect mmio call would wedge. Pin them here.
   */
  .got    : { *(.got*) } :data
  .sdata  : { *(.sdata*) } :data
  .sbss   : { *(.sbss*) } :data
  /*
   * Guest-visible backing buffers must sit outside _end: the VMM image
   * range is registered as host-protected and may not alias guest RAM/ROM.
   */
  _end = .;
  . = ALIGN(4096);
  .fb_fallback (NOLOAD) : { *(.fb_fallback*) } :data
  . = ALIGN(4096);
  .ovmf_store (NOLOAD) : { *(.ovmf_store*) } :data
  _fb_fallback_end = .;
  /*
   * Guest RAM backing for the mips64 system boots lives in the highmem RAM
   * window. The lowmem window (0-256 MiB) cannot hold a >= 147 MiB backing
   * store alongside the VMM image (.text starts at 128 MiB), because qemu's
   * load_elf writes every segment into the lowmem RAM and a BSS top past
   * 0x10000000 lands outside RAM and wedges the loader. Placing the backing
   * store at VIRT_HIGHMEM_BASE puts it in real highmem RAM where there is
   * room to grow. NOLOAD: the guest kernel initializes its own memory.
   */
  .guest_ram 0x80000000 (NOLOAD) : { *(.guest_ram*) } :guestram
  _guest_ram_end = .;
}
