#ifndef LATR_FDT_H
#define LATR_FDT_H

#include <stdbool.h>
#include <stdint.h>

/* Flattened device tree (DTB) writer. Values are big-endian per the FDT
 * specification; callers build the tree depth-first and finish it once. */
#define LATR_FDT_HEADER_SIZE 40u
#define LATR_FDT_RSVMAP_SIZE 16u
#define LATR_FDT_MAX_STRINGS 2048u

typedef struct LatrFdt {
    uint8_t *buf;
    uint32_t cap;
    uint32_t struct_off;
    uint32_t struct_len;
    uint32_t depth;
    char strings[LATR_FDT_MAX_STRINGS];
    uint32_t strings_len;
    bool failed;
} LatrFdt;

void latr_fdt_init(LatrFdt *fdt, uint8_t *buf, uint32_t cap,
                   uint32_t boot_cpuid);
void latr_fdt_begin_node(LatrFdt *fdt, const char *name);
void latr_fdt_end_node(LatrFdt *fdt);
void latr_fdt_prop_u32(LatrFdt *fdt, const char *name, uint32_t value);
void latr_fdt_prop_u64(LatrFdt *fdt, const char *name, uint64_t value);
void latr_fdt_prop_cells(LatrFdt *fdt, const char *name,
                         const uint32_t *cells, uint32_t count);
void latr_fdt_prop_string(LatrFdt *fdt, const char *name, const char *value);
void latr_fdt_prop_empty(LatrFdt *fdt, const char *name);
void latr_fdt_prop_strings(LatrFdt *fdt, const char *name,
                           const char *const *values, uint32_t count);
/* Appends FDT_END and writes the header. Returns the DTB size, 0 on error. */
uint32_t latr_fdt_finish(LatrFdt *fdt);

#endif
