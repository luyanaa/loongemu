#ifndef LATM_SYSTEM_H
#define LATM_SYSTEM_H

#include <stdint.h>
#include <stddef.h>

#define LATM_MMIO_MAX_REGIONS 32u
#define LATM_MEMORY_MAX_REGIONS 16u

typedef enum LatmMmioKind {
    LATM_MMIO_RAM = 0,
    LATM_MMIO_ROM,
    LATM_MMIO_DEVICE,
    LATM_MMIO_RESERVED,
} LatmMmioKind;

typedef uint64_t (*LatmMmioReadFn)(void *opaque, uint64_t offset, unsigned size);
typedef void (*LatmMmioWriteFn)(void *opaque, uint64_t offset, uint64_t value,
                                unsigned size);

typedef struct LatmMmioRegion {
    uint64_t base;
    uint64_t size;
    LatmMmioKind kind;
    uint8_t mat;                 /* 0=SUC, 1=CC, 2=WUC */
    uint8_t used;
    uint16_t reserved;
    LatmMmioReadFn read;
    LatmMmioWriteFn write;
    void *opaque;
    const char *name;
} LatmMmioRegion;

typedef struct LatmMmioBus {
    uint32_t count;
    LatmMmioRegion regions[LATM_MMIO_MAX_REGIONS];
} LatmMmioBus;

void latm_mmio_init(LatmMmioBus *bus);
int latm_mmio_add(LatmMmioBus *bus, uint64_t base, uint64_t size,
                  LatmMmioKind kind, uint8_t mat, LatmMmioReadFn read,
                  LatmMmioWriteFn write, void *opaque, const char *name);
int latm_mmio_remove(LatmMmioBus *bus, uint64_t base);
LatmMmioRegion *latm_mmio_find(LatmMmioBus *bus, uint64_t addr, unsigned size);
int latm_mmio_read(LatmMmioBus *bus, uint64_t addr, unsigned size,
                   uint64_t *value);
int latm_mmio_write(LatmMmioBus *bus, uint64_t addr, unsigned size,
                    uint64_t value);

/* RAM callbacks used by the 3A board and tests. */
uint64_t latm_mmio_ram_read(void *opaque, uint64_t offset, unsigned size);
void latm_mmio_ram_write(void *opaque, uint64_t offset, uint64_t value,
                         unsigned size);

typedef struct LatmMemoryRegion {
    uint64_t base;
    uint64_t size;
    uint64_t hpa;
    LatmMmioKind kind;
    uint8_t mat;
    uint8_t reserved[7];
    const char *name;
} LatmMemoryRegion;

typedef struct LatmMemoryLayout {
    uint32_t count;
    LatmMemoryRegion regions[LATM_MEMORY_MAX_REGIONS];
    uint64_t low_ram_base;
    uint64_t low_ram_size;
    uint64_t high_ram_base;
    uint64_t high_ram_size;
} LatmMemoryLayout;

void latm_memory_init(LatmMemoryLayout *layout);
int latm_memory_add(LatmMemoryLayout *layout, uint64_t base, uint64_t size,
                    uint64_t hpa, LatmMmioKind kind, uint8_t mat,
                    const char *name);
int latm_memory_add_reserved(LatmMemoryLayout *layout, uint64_t base,
                             uint64_t size, const char *name);
int latm_memory_setup_3a4000(LatmMemoryLayout *layout, uint64_t ram_hpa,
                             uint64_t ram_size, uint64_t vmm_base,
                             uint64_t vmm_size, uint64_t code_base,
                             uint64_t code_size);
int latm_memory_overlaps(const LatmMemoryLayout *layout, uint64_t base,
                         uint64_t size);
const LatmMemoryRegion *latm_memory_find(const LatmMemoryLayout *layout,
                                         uint64_t addr, unsigned size);

typedef struct LatmBootInfo {
    uint64_t dtb_gpa;
    uint64_t initrd_start;
    uint64_t initrd_end;
    uint64_t entry_pc;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    char cmdline[256];
} LatmBootInfo;

void latm_boot_info_init(LatmBootInfo *info);
void latm_boot_info_set_dtb(LatmBootInfo *info, uint64_t dtb_gpa,
                            uint64_t dtb_size);
void latm_boot_info_set_initrd(LatmBootInfo *info, uint64_t start, uint64_t end);
void latm_boot_info_set_cmdline(LatmBootInfo *info, const char *cmdline);
void latm_boot_info_set_argv(LatmBootInfo *info, uint64_t argc,
                             uint64_t argv_gpa);
void latm_boot_info_apply_args(LatmBootInfo *info, void *env);

/* FDT builder.  The caller owns the output buffer. */
typedef struct LatmDtbBuilder {
    uint8_t *out;
    size_t cap;
    uint8_t *structs;
    size_t struct_cap;
    uint8_t *strings;
    size_t strings_cap;
    size_t struct_len;
    size_t strings_len;
    int error;
} LatmDtbBuilder;

void latm_dtb_init(LatmDtbBuilder *b, uint8_t *out, size_t out_cap,
                   uint8_t *struct_buf, size_t struct_cap,
                   uint8_t *strings_buf, size_t strings_cap);
void latm_dtb_begin_node(LatmDtbBuilder *b, const char *name);
void latm_dtb_end_node(LatmDtbBuilder *b);
void latm_dtb_prop(LatmDtbBuilder *b, const char *name, const void *data,
                   size_t len);
void latm_dtb_prop_u32(LatmDtbBuilder *b, const char *name, uint32_t value);
void latm_dtb_prop_u64(LatmDtbBuilder *b, const char *name, uint64_t value);
void latm_dtb_prop_string(LatmDtbBuilder *b, const char *name,
                          const char *value);
void latm_dtb_prop_cells(LatmDtbBuilder *b, const char *name,
                         const uint32_t *cells, size_t count);
size_t latm_dtb_finish(LatmDtbBuilder *b);
size_t latm_dtb_build_3a4000(uint8_t *out, size_t cap,
                             uint8_t *struct_buf, size_t struct_cap,
                             uint8_t *strings_buf, size_t strings_cap,
                             const LatmMemoryLayout *mem,
                             const LatmBootInfo *boot);

#endif
