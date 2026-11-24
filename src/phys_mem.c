#include "loongemu/phys_mem.h"

static int ranges_overlap(uint64_t a_start, uint64_t a_size,
                          uint64_t b_start, uint64_t b_size)
{
    uint64_t a_end = a_start + a_size;
    uint64_t b_end = b_start + b_size;

    if (a_size == 0 || b_size == 0 || a_end < a_start || b_end < b_start) {
        return 0;
    }
    return a_start < b_end && b_start < a_end;
}

void loongemu_mem_map_init(LoongemuMemMap *map)
{
    if (map == 0) {
        return;
    }
    map->count = 0;
    map->generation++;
}

static uint8_t default_mat_for_kind(LoongemuMemKind kind)
{
    if (kind == LOONGEMU_MEM_RAM || kind == LOONGEMU_MEM_ROM) {
        return (uint8_t)LOONGEMU_MAT_CC;
    }
    return (uint8_t)LOONGEMU_MAT_SUC;
}

int loongemu_mem_map_add_mat(LoongemuMemMap *map,
                             uint64_t gpa_base,
                             uint64_t size,
                             uint64_t hpa_base,
                             LoongemuMemKind kind,
                             uint8_t host_mat,
                             const char *name)
{
    if (map == 0 || size == 0 || map->count >= LOONGEMU_MEM_MAX_REGIONS) {
        return 0;
    }
    if (gpa_base + size < gpa_base) {
        return 0;
    }
    if ((kind == LOONGEMU_MEM_RAM || kind == LOONGEMU_MEM_ROM ||
         kind == LOONGEMU_MEM_RESERVED) &&
        hpa_base + size < hpa_base) {
        return 0;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        if (ranges_overlap(gpa_base, size,
                           map->regions[i].gpa_base,
                           map->regions[i].size)) {
            return 0;
        }
    }
    if (kind == LOONGEMU_MEM_RAM || kind == LOONGEMU_MEM_ROM) {
        if (loongemu_mem_is_host_protected_hpa(map, hpa_base, size)) {
            return 0;
        }
    }
    if (host_mat > (uint8_t)LOONGEMU_MAT_WUC) {
        host_mat = default_mat_for_kind(kind);
    }
    map->regions[map->count].gpa_base = gpa_base;
    map->regions[map->count].size = size;
    map->regions[map->count].hpa_base = hpa_base;
    map->regions[map->count].kind = kind;
    map->regions[map->count].host_mat = host_mat;
    map->regions[map->count].reserved[0] = 0;
    map->regions[map->count].reserved[1] = 0;
    map->regions[map->count].reserved[2] = 0;
    map->regions[map->count].name = name;
    map->count++;
    map->generation++;
    return 1;
}

int loongemu_mem_map_add(LoongemuMemMap *map,
                         uint64_t gpa_base,
                         uint64_t size,
                         uint64_t hpa_base,
                         LoongemuMemKind kind,
                         const char *name)
{
    return loongemu_mem_map_add_mat(map, gpa_base, size, hpa_base, kind,
                                    default_mat_for_kind(kind), name);
}

int loongemu_mem_map_remove(LoongemuMemMap *map, uint64_t gpa_base)
{
    if (map == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->regions[i].gpa_base == gpa_base) {
            for (uint32_t j = i + 1u; j < map->count; j++) {
                map->regions[j - 1u] = map->regions[j];
            }
            map->count--;
            map->generation++;
            return 1;
        }
    }
    return 0;
}

int loongemu_mem_is_host_protected_hpa(const LoongemuMemMap *map,
                                       uint64_t hpa,
                                       uint64_t size)
{
    if (map == 0 || size == 0 || hpa + size < hpa) {
        return 1;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->regions[i].kind == LOONGEMU_MEM_RESERVED &&
            ranges_overlap(hpa, size,
                           map->regions[i].hpa_base,
                           map->regions[i].size)) {
            return 1;
        }
    }
    return 0;
}

LoongemuMemKind loongemu_mem_resolve(const LoongemuMemMap *map,
                                     uint64_t gpa,
                                     uint64_t size,
                                     LoongemuMemLookup *out)
{
    if (out != 0) {
        out->kind = LOONGEMU_MEM_UNMAPPED;
        out->host_mat = (uint8_t)LOONGEMU_MAT_SUC;
        out->hpa = 0;
        out->offset = 0;
        out->avail = 0;
        out->name = 0;
    }
    if (map == 0 || size == 0 || gpa + size < gpa) {
        return LOONGEMU_MEM_UNMAPPED;
    }
    for (uint32_t i = 0; i < map->count; i++) {
        const LoongemuMemRegion *r = &map->regions[i];
        uint64_t r_end = r->gpa_base + r->size;

        if (gpa >= r->gpa_base && gpa < r_end) {
            uint64_t off = gpa - r->gpa_base;
            uint64_t avail = r->size - off;

            if (r->kind == LOONGEMU_MEM_RESERVED) {
                if (out != 0) {
                    out->kind = LOONGEMU_MEM_RESERVED;
                    out->host_mat = (uint8_t)LOONGEMU_MAT_SUC;
                    out->hpa = 0;
                    out->offset = off;
                    out->avail = avail;
                    out->name = r->name;
                }
                return LOONGEMU_MEM_RESERVED;
            }
            if (size > avail) {
                return LOONGEMU_MEM_UNMAPPED;
            }
            if (out != 0) {
                out->kind = r->kind;
                out->host_mat = r->host_mat;
                out->hpa = (r->kind == LOONGEMU_MEM_RAM ||
                            r->kind == LOONGEMU_MEM_ROM)
                               ? (r->hpa_base + off)
                               : 0;
                out->offset = off;
                out->avail = avail;
                out->name = r->name;
            }
            return r->kind;
        }
    }
    return LOONGEMU_MEM_UNMAPPED;
}
