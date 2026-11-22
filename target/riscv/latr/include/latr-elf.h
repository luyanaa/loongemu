#ifndef LATR_ELF_H
#define LATR_ELF_H

#include <stdint.h>

/*
 * ELF64 RISC-V ET_EXEC loader, deliberately free of any machine-model
 * dependency so it can be unit-tested on the host.
 *
 * Segments are placed at p_paddr (offset 24 in the program header -- offset 16
 * is p_vaddr). Firmware such as OpenSBI is linked with p_vaddr == p_paddr, so
 * the old code that read p_vaddr happened to work; a real kernel or a
 * position-dependent payload need not, which is why the two fields are kept
 * apart here.
 *
 * Every PT_LOAD segment is validated before a single byte is written, so a
 * corrupt image can never leave RAM half-updated.
 */
#define LATR_ELF_MAX_LOADS 16
#define LATR_ELF_HEADER_SIZE 64
#define LATR_ELF_PHDR_SIZE 56

typedef struct LatrElfImage {
    uint64_t entry;
    /* Highest p_paddr + p_memsz over all loaded segments. */
    uint64_t end;
    /* Number of bytes actually copied from the file. */
    uint64_t file_bytes;
    /* Number of zero-fill bytes for the bss/tail of every segment. */
    uint64_t bss_bytes;
    uint32_t loads;
} LatrElfImage;

/*
 * Loads `elf` into the RAM window [ram, ram + ram_size) whose guest base is
 * `ram_base`. Returns 0 and writes nothing at all when anything is wrong:
 * bad magic/class/endianness, e_machine != EM_RISCV, type != ET_EXEC,
 * p_filesz > p_memsz, a segment outside RAM, p_paddr+p_memsz overflow, two
 * overlapping segments, an unaligned or out-of-image entry point.
 */
int latr_elf_load(const uint8_t *elf, uint64_t len, uint8_t *ram,
                  uint64_t ram_base, uint64_t ram_size, LatrElfImage *out);

#endif
