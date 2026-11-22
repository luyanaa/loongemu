/*
 * ELF64 RISC-V ET_EXEC loader.
 *
 * This file intentionally depends on nothing but <string.h> and the C library
 * subset LATR already requires (memcpy/memset), so the same code runs in the
 * freestanding LoongArch build and in host unit tests.
 */
#include <string.h>
#include <stdbool.h>
#include "latr-elf.h"

#define ELF_MAGIC0 0x7f
#define ELF_CLASS64 2
#define ELF_DATA_LSB 1
#define ELF_ET_EXEC 2
#define ELF_EM_RISCV 243
#define ELF_PT_LOAD 1

/* Program header field offsets (ELF64). */
#define PH_TYPE 0
#define PH_OFFSET 8
#define PH_VADDR 16
#define PH_PADDR 24
#define PH_FILESZ 32
#define PH_MEMSZ 40

#define EH_TYPE 16
#define EH_MACHINE 18
#define EH_ENTRY 24
#define EH_PHOFF 32
#define EH_EHSIZE 52
#define EH_PHENTSIZE 54
#define EH_PHNUM 56

static uint64_t rd(const uint8_t *p, unsigned off, unsigned size)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)p[off + i] << (8 * i);
    }
    return v;
}

int latr_elf_load(const uint8_t *elf, uint64_t len, uint8_t *ram,
                  uint64_t ram_base, uint64_t ram_size, LatrElfImage *out)
{
    struct {
        uint64_t start;
        uint64_t end;
    } loads[LATR_ELF_MAX_LOADS];
    unsigned nloads = 0;
    uint64_t phoff;
    uint64_t phentsize;
    uint64_t phnum;
    uint64_t entry;
    uint64_t ehsize;
    LatrElfImage info;

    if (elf == NULL || ram == NULL || ram_size == 0 || len < LATR_ELF_HEADER_SIZE) {
        return 0;
    }
    if (elf[0] != ELF_MAGIC0 || elf[1] != 'E' || elf[2] != 'L' ||
        elf[3] != 'F' || elf[4] != ELF_CLASS64 || elf[5] != ELF_DATA_LSB) {
        return 0;
    }
    if (rd(elf, EH_TYPE, 2) != ELF_ET_EXEC ||
        rd(elf, EH_MACHINE, 2) != ELF_EM_RISCV) {
        return 0;
    }
    ehsize = rd(elf, EH_EHSIZE, 2);
    if (ehsize < LATR_ELF_HEADER_SIZE || ehsize > len) {
        return 0;
    }
    entry = rd(elf, EH_ENTRY, 8);
    phoff = rd(elf, EH_PHOFF, 8);
    phentsize = rd(elf, EH_PHENTSIZE, 2);
    phnum = rd(elf, EH_PHNUM, 2);

    if (phentsize < LATR_ELF_PHDR_SIZE || phnum == 0 ||
        phnum > LATR_ELF_MAX_LOADS) {
        return 0;
    }
    /* Division instead of multiplication: this cannot overflow. */
    if (phoff > len || phnum > (len - phoff) / phentsize) {
        return 0;
    }

    memset(&info, 0, sizeof(info));
    info.entry = entry;

    /* Pass 1: validate every PT_LOAD segment and record its RAM range. */
    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + i * phentsize;
        uint64_t off;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t delta;
        uint64_t end;

        if (rd(ph, PH_TYPE, 4) != ELF_PT_LOAD) {
            continue;
        }
        off = rd(ph, PH_OFFSET, 8);
        paddr = rd(ph, PH_PADDR, 8);
        filesz = rd(ph, PH_FILESZ, 8);
        memsz = rd(ph, PH_MEMSZ, 8);
        if (filesz > memsz || memsz == 0) {
            return 0;
        }
        if (off > len || filesz > len - off) {
            return 0;
        }
        if (paddr < ram_base) {
            return 0;
        }
        delta = paddr - ram_base;
        if (delta > ram_size || memsz > ram_size - delta) {
            return 0; /* also rejects paddr + memsz overflow */
        }
        end = paddr + memsz;
        for (unsigned j = 0; j < nloads; j++) {
            if (paddr < loads[j].end && loads[j].start < end) {
                return 0; /* overlapping load segments */
            }
        }
        if (nloads == LATR_ELF_MAX_LOADS) {
            return 0;
        }
        loads[nloads].start = paddr;
        loads[nloads].end = end;
        nloads++;
        info.file_bytes += filesz;
        info.bss_bytes += memsz - filesz;
        if (end > info.end) {
            info.end = end;
        }
    }
    if (nloads == 0) {
        return 0;
    }
    /* c.relax expands to 16-bit instructions, so 2-byte alignment is the
     * weakest entry alignment that can still be executed. */
    if ((entry & 1u) != 0) {
        return 0;
    }
    {
        bool entry_ok = false;

        for (unsigned j = 0; j < nloads; j++) {
            if (entry >= loads[j].start && entry < loads[j].end) {
                entry_ok = true;
            }
        }
        if (!entry_ok) {
            return 0;
        }
    }
    info.loads = nloads;

    /* Pass 2: write. Validation above makes every range in-bounds. */
    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + i * phentsize;
        uint64_t off;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint8_t *dst;

        if (rd(ph, PH_TYPE, 4) != ELF_PT_LOAD) {
            continue;
        }
        off = rd(ph, PH_OFFSET, 8);
        paddr = rd(ph, PH_PADDR, 8);
        filesz = rd(ph, PH_FILESZ, 8);
        memsz = rd(ph, PH_MEMSZ, 8);
        dst = ram + (paddr - ram_base);
        if (filesz != 0) {
            memcpy(dst, elf + off, (size_t)filesz);
        }
        if (memsz > filesz) {
            memset(dst + filesz, 0, (size_t)(memsz - filesz));
        }
    }
    if (out != NULL) {
        *out = info;
    }
    return 1;
}
