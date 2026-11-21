#ifndef LOONGEMU_PHYS_MEM_H
#define LOONGEMU_PHYS_MEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Three-layer VMM physical address map:
 *   Guest VA --(Guest MMU)--> Guest PA --(LoongemuMemMap)--> Host PA
 *
 * Guest physical addresses are explicitly partitioned into RAM, ROM, MMIO,
 * host-protected RESERVED regions, and UNMAPPED holes so guest page tables
 * can never blindly alias arbitrary host memory or VMM internal state.
 */
typedef enum LoongemuMemKind {
    LOONGEMU_MEM_UNMAPPED = 0,
    LOONGEMU_MEM_RAM,
    LOONGEMU_MEM_ROM,
    LOONGEMU_MEM_MMIO,
    LOONGEMU_MEM_RESERVED,
} LoongemuMemKind;

/* LoongArch PTE Memory Access Type (MAT bits [5:4]) */
typedef enum LoongemuHostMat {
    LOONGEMU_MAT_SUC = 0, /* Strongly-ordered UnCached (MMIO / safe 7A fallback) */
    LOONGEMU_MAT_CC  = 1, /* Coherent Cached (normal RAM / VMM fallback FB) */
    LOONGEMU_MAT_WUC = 2, /* Weakly-ordered UnCached (Write-Combining VRAM) */
} LoongemuHostMat;

typedef enum LoongemuFbOwner {
    LOONGEMU_FB_OWNER_NONE      = 0,
    LOONGEMU_FB_OWNER_HOST_GOP  = 1, /* Host UEFI GOP linear framebuffer handover */
    LOONGEMU_FB_OWNER_VMM_ALLOC = 2, /* Dedicated VMM physical page fallback pool */
} LoongemuFbOwner;

#define LOONGEMU_MEM_MAX_REGIONS 32

typedef struct LoongemuMemRegion {
    uint64_t gpa_base;
    uint64_t size;
    uint64_t hpa_base;
    LoongemuMemKind kind;
    uint8_t host_mat; /* LoongemuHostMat */
    uint8_t reserved[3];
    const char *name;
} LoongemuMemRegion;

typedef struct LoongemuMemMap {
    LoongemuMemRegion regions[LOONGEMU_MEM_MAX_REGIONS];
    uint32_t count;
} LoongemuMemMap;

typedef struct LoongemuMemLookup {
    LoongemuMemKind kind;
    uint8_t host_mat; /* LoongemuHostMat */
    uint8_t reserved[3];
    uint64_t hpa;
    uint64_t offset;
    uint64_t avail;
    const char *name;
} LoongemuMemLookup;

void loongemu_mem_map_init(LoongemuMemMap *map);

/*
 * Register a physical region (default MAT: CC for RAM/ROM, SUC for MMIO/RESERVED).
 * Returns 1 on success, 0 if full, zero-sized, overflowing, or overlapping an
 * existing region.
 */
int loongemu_mem_map_add(LoongemuMemMap *map,
                         uint64_t gpa_base,
                         uint64_t size,
                         uint64_t hpa_base,
                         LoongemuMemKind kind,
                         const char *name);

/* Register a physical region with an explicit LoongemuHostMat attribute. */
int loongemu_mem_map_add_mat(LoongemuMemMap *map,
                             uint64_t gpa_base,
                             uint64_t size,
                             uint64_t hpa_base,
                             LoongemuMemKind kind,
                             uint8_t host_mat,
                             const char *name);

/* Unregister an existing region by exact gpa_base. Returns 1 if removed. */
int loongemu_mem_map_remove(LoongemuMemMap *map, uint64_t gpa_base);

/*
 * Resolve [gpa, gpa + size) into a region lookup.
 * If the range falls outside all regions or crosses a region boundary,
 * returns LOONGEMU_MEM_UNMAPPED (unless it hits a RESERVED region, which
 * returns LOONGEMU_MEM_RESERVED).
 */
LoongemuMemKind loongemu_mem_resolve(const LoongemuMemMap *map,
                                     uint64_t gpa,
                                     uint64_t size,
                                     LoongemuMemLookup *out);

/*
 * Returns 1 if [hpa, hpa + size) overlaps any LOONGEMU_MEM_RESERVED region.
 */
int loongemu_mem_is_host_protected_hpa(const LoongemuMemMap *map,
                                       uint64_t hpa,
                                       uint64_t size);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_PHYS_MEM_H */
