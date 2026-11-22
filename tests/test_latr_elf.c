/*
 * Host unit tests for the LATR ELF64 loader.
 *
 * The bug this file exists to prevent: p_paddr lives at offset 24 of an ELF64
 * program header and p_vaddr at offset 16. Firmware linked at a single address
 * makes them equal, so a loader that reads the wrong field still boots -- until
 * a payload with a distinct vaddr/paddr pair is loaded and lands in the wrong
 * place. Every case below builds a synthetic image so both fields are checked
 * independently.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "latr-elf.h"

static int checks;
static int failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what);            \
        }                                                                    \
    } while (0)

#define RAM_BASE UINT64_C(0x80000000)
#define RAM_SIZE (64u * 1024u)

static uint8_t ram[RAM_SIZE];
static uint8_t image[4096];

static void put(uint8_t *p, unsigned off, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        p[off + i] = (uint8_t)(value >> (8 * i));
    }
}

struct seg {
    uint64_t paddr;
    uint64_t vaddr;
    uint64_t off;
    uint64_t filesz;
    uint64_t memsz;
};

static uint64_t build(const struct seg *segs, unsigned n, uint64_t entry,
                      uint16_t machine, uint16_t type)
{
    memset(image, 0, sizeof(image));
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2; /* ELFCLASS64 */
    image[5] = 1; /* little endian */
    put(image, 16, type, 2);
    put(image, 18, machine, 2);
    put(image, 24, entry, 8);
    put(image, 32, 64, 8);  /* e_phoff */
    put(image, 52, 64, 2);  /* e_ehsize */
    put(image, 54, 56, 2);  /* e_phentsize */
    put(image, 56, n, 2);   /* e_phnum */

    for (unsigned i = 0; i < n; i++) {
        uint8_t *ph = image + 64 + i * 56;

        put(ph, 0, 1, 4); /* PT_LOAD */
        put(ph, 8, segs[i].off, 8);
        put(ph, 16, segs[i].vaddr, 8);
        put(ph, 24, segs[i].paddr, 8);
        put(ph, 32, segs[i].filesz, 8);
        put(ph, 40, segs[i].memsz, 8);
    }
    /* Payload bytes go after the program header table. */
    return 64 + (uint64_t)n * 56;
}

static int load(const struct seg *segs, unsigned n, uint64_t entry,
                LatrElfImage *out)
{
    uint64_t used = build(segs, n, entry, 243, 2);

    for (unsigned i = 0; i < n; i++) {
        memset(image + segs[i].off, (int)(0x40 + i), (size_t)segs[i].filesz);
        if (segs[i].off + segs[i].filesz > used) {
            used = segs[i].off + segs[i].filesz;
        }
    }
    if (used > sizeof(image)) {
        return 0;
    }
    memset(ram, 0, sizeof(ram));
    return latr_elf_load(image, used, ram, RAM_BASE, RAM_SIZE, out);
}

int main(void)
{
    LatrElfImage info;

    /* 1. p_paddr is honoured, not p_vaddr. */
    {
        struct seg s = { RAM_BASE + 0x1000, RAM_BASE + 0x9000, 200,
                         16, 32 };

        CHECK(load(&s, 1, RAM_BASE + 0x1000, &info), "paddr/vaddr split loads");
        CHECK(ram[0x1000] == 0x40, "payload lands at p_paddr");
        CHECK(ram[0x1008] == 0x40, "whole filesz copied");
        CHECK(ram[0x1010] == 0, "tail beyond filesz is zero-filled (bss)");
        CHECK(ram[0x9000] == 0, "p_vaddr is NOT used as the load address");
        CHECK(info.entry == RAM_BASE + 0x1000, "entry reported");
        CHECK(info.end == RAM_BASE + 0x1020, "end covers p_memsz");
        CHECK(info.file_bytes == 16, "file bytes counted");
        CHECK(info.bss_bytes == 16, "bss bytes counted");
        CHECK(info.loads == 1, "one load segment");
    }

    /* 2. Entry outside every segment is rejected. */
    {
        struct seg s = { RAM_BASE + 0x1000, RAM_BASE + 0x1000, 200, 8, 8 };

        CHECK(!load(&s, 1, RAM_BASE + 0x2000, &info),
              "entry outside the image is rejected");
    }
    {
        struct seg s = { RAM_BASE + 0x1000, RAM_BASE + 0x1000, 200, 8, 8 };

        CHECK(load(&s, 1, RAM_BASE + 0x1000, &info),
              "entry at segment start is accepted");
    }

    /* 3. p_filesz > p_memsz is a corrupt segment. */
    {
        struct seg s = { RAM_BASE + 0x1000, RAM_BASE + 0x1000, 200, 32, 16 };

        CHECK(!load(&s, 1, RAM_BASE + 0x1000, &info),
              "filesz > memsz is rejected");
    }

    /* 4. Segment outside RAM, and paddr + memsz overflowing RAM. */
    {
        struct seg s = { RAM_BASE - 0x1000, 0, 200, 8, 8 };

        CHECK(!load(&s, 1, RAM_BASE - 0x1000, &info),
              "segment below RAM is rejected");
    }
    {
        struct seg s = { RAM_BASE + RAM_SIZE - 8, 0, 200, 8, 64 };

        CHECK(!load(&s, 1, RAM_BASE + RAM_SIZE - 8, &info),
              "segment running past RAM is rejected");
    }
    {
        struct seg s = { UINT64_MAX - 8, 0, 200, 8, 16 };

        CHECK(!load(&s, 1, UINT64_MAX - 8, &info),
              "paddr + memsz overflow is rejected");
    }

    /* 5. Overlapping segments. */
    {
        struct seg s[2] = {
            { RAM_BASE + 0x1000, 0, 200, 32, 64 },
            { RAM_BASE + 0x1020, 0, 300, 8, 8 },
        };

        CHECK(!load(s, 2, RAM_BASE + 0x1000, &info),
              "overlapping load segments are rejected");
    }
    {
        struct seg s[2] = {
            { RAM_BASE + 0x1000, 0, 200, 16, 16 },
            { RAM_BASE + 0x1010, 0, 300, 16, 16 },
        };

        CHECK(load(s, 2, RAM_BASE + 0x1000, &info),
              "abutting segments are legal");
        CHECK(info.loads == 2, "both abutting segments counted");
    }

    /* 6. Header-level rejections. */
    {
        struct seg s = { RAM_BASE + 0x1000, 0, 200, 8, 8 };

        (void)build(&s, 1, RAM_BASE + 0x1000, 243, 2);
        CHECK(latr_elf_load(image, sizeof(image), ram, RAM_BASE, RAM_SIZE,
                            &info),
              "baseline image for the header tests loads");
        put(image, 18, 62 /* EM_X86_64 */, 2);
        CHECK(!latr_elf_load(image, sizeof(image), ram, RAM_BASE, RAM_SIZE,
                             &info),
              "e_machine != EM_RISCV is rejected");
        put(image, 18, 243, 2);
        put(image, 16, 3 /* ET_DYN */, 2);
        CHECK(!latr_elf_load(image, sizeof(image), ram, RAM_BASE, RAM_SIZE,
                             &info),
              "ET_DYN is rejected");
        put(image, 16, 2, 2);
        image[4] = 1; /* ELFCLASS32 */
        CHECK(!latr_elf_load(image, sizeof(image), ram, RAM_BASE, RAM_SIZE,
                             &info),
              "ELFCLASS32 is rejected");
        image[4] = 2;
        image[5] = 2; /* big endian */
        CHECK(!latr_elf_load(image, sizeof(image), ram, RAM_BASE, RAM_SIZE,
                             &info),
              "big-endian image is rejected");
        image[5] = 1;
    }

    /* 7. Truncated program-header table and truncated file data. */
    {
        struct seg s = { RAM_BASE + 0x1000, 0, 200, 8, 8 };
        uint64_t used = build(&s, 1, RAM_BASE + 0x1000, 243, 2);

        CHECK(!latr_elf_load(image, used - 8, ram, RAM_BASE, RAM_SIZE,
                             &info),
              "truncated program headers are rejected");
        CHECK(!latr_elf_load(image, used, ram, RAM_BASE, RAM_SIZE, &info),
              "segment data beyond EOF is rejected");
    }

    /* 8. Unaligned entry. */
    {
        struct seg s = { RAM_BASE + 0x1000, 0, 200, 8, 8 };

        CHECK(!load(&s, 1, RAM_BASE + 0x1001, &info),
              "odd entry address is rejected");
    }

    /* 9. Nothing is written when validation fails. */
    {
        struct seg s[2] = {
            { RAM_BASE + 0x1000, 0, 200, 16, 16 },
            { RAM_BASE + 0x80000000, 0, 300, 8, 8 },
        };
        uint64_t used = build(s, 2, RAM_BASE + 0x1000, 243, 2);

        memset(image + 200, 0x5a, 16);
        memset(ram, 0, sizeof(ram));
        CHECK(!latr_elf_load(image, used, ram, RAM_BASE, RAM_SIZE, &info),
              "segment outside RAM aborts the load");
        CHECK(ram[0x1000] == 0, "no partial write after a later invalid segment");
    }

    /* 10. No loadable segment at all. */
    {
        struct seg s = { RAM_BASE + 0x1000, 0, 200, 8, 8 };
        uint64_t used = build(&s, 1, RAM_BASE + 0x1000, 243, 2);

        put(image, 64, 4 /* PT_NOTE */, 4);
        CHECK(!latr_elf_load(image, used, ram, RAM_BASE, RAM_SIZE, &info),
              "image with no PT_LOAD is rejected");
    }

    printf("test-latr-elf: %d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("test-latr-elf: PASS\n");
        return 0;
    }
    return 1;
}
