#include <string.h>
#include "latr-fdt.h"

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_END 9u
#define FDT_VERSION 17u
#define FDT_LAST_COMP_VERSION 16u

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void put_be64(uint8_t *p, uint64_t value)
{
    put_be32(p, (uint32_t)(value >> 32));
    put_be32(p + 4, (uint32_t)value);
}

static uint32_t align4(uint32_t value)
{
    return (value + 3u) & ~3u;
}

static bool reserve(LatrFdt *fdt, uint32_t bytes)
{
    if (fdt->failed ||
        fdt->struct_off + fdt->struct_len + bytes > fdt->cap) {
        fdt->failed = true;
        return false;
    }
    return true;
}

static void append_bytes(LatrFdt *fdt, const void *data, uint32_t len)
{
    if (!reserve(fdt, align4(len))) {
        return;
    }
    memcpy(fdt->buf + fdt->struct_off + fdt->struct_len, data, len);
    if (len & 3u) {
        memset(fdt->buf + fdt->struct_off + fdt->struct_len + len, 0,
               4u - (len & 3u));
    }
    fdt->struct_len += align4(len);
}

static void append_cell(LatrFdt *fdt, uint32_t value)
{
    uint8_t cell[4];

    put_be32(cell, value);
    append_bytes(fdt, cell, sizeof(cell));
}

/* Raw copy without per-item alignment; used for string lists whose items are
 * packed and padded only once at the end of the property. */
static void append_raw(LatrFdt *fdt, const void *data, uint32_t len)
{
    if (!reserve(fdt, len)) {
        return;
    }
    memcpy(fdt->buf + fdt->struct_off + fdt->struct_len, data, len);
    fdt->struct_len += len;
}

static void pad_struct(LatrFdt *fdt)
{
    uint32_t pad = (4u - (fdt->struct_len & 3u)) & 3u;

    if (pad == 0) {
        return;
    }
    if (!reserve(fdt, pad)) {
        return;
    }
    memset(fdt->buf + fdt->struct_off + fdt->struct_len, 0, pad);
    fdt->struct_len += pad;
}

static uint32_t string_offset(LatrFdt *fdt, const char *name)
{
    uint32_t len = (uint32_t)strlen(name) + 1u;

    for (uint32_t i = 0; i < fdt->strings_len; i++) {
        if (strcmp(fdt->strings + i, name) == 0) {
            return i;
        }
        i += (uint32_t)strlen(fdt->strings + i);
    }
    if (fdt->strings_len + len > sizeof(fdt->strings)) {
        fdt->failed = true;
        return 0;
    }
    memcpy(fdt->strings + fdt->strings_len, name, len);
    uint32_t offset = fdt->strings_len;
    fdt->strings_len += len;
    return offset;
}

void latr_fdt_init(LatrFdt *fdt, uint8_t *buf, uint32_t cap,
                   uint32_t boot_cpuid)
{
    memset(fdt, 0, sizeof(*fdt));
    fdt->buf = buf;
    fdt->cap = cap;
    fdt->struct_off = LATR_FDT_HEADER_SIZE + LATR_FDT_RSVMAP_SIZE;
    if (buf == NULL || cap < fdt->struct_off + 16u) {
        fdt->failed = true;
        return;
    }
    memset(buf, 0, fdt->struct_off);
    put_be32(buf + 0, FDT_MAGIC);
    put_be32(buf + 16, LATR_FDT_HEADER_SIZE); /* off_mem_rsvmap */
    put_be32(buf + 20, FDT_VERSION);
    put_be32(buf + 24, FDT_LAST_COMP_VERSION);
    put_be32(buf + 28, boot_cpuid);
}

void latr_fdt_begin_node(LatrFdt *fdt, const char *name)
{
    uint32_t len = (uint32_t)strlen(name) + 1u;

    if (!reserve(fdt, 4u + align4(len))) {
        return;
    }
    append_cell(fdt, FDT_BEGIN_NODE);
    append_bytes(fdt, name, len);
    fdt->depth++;
}

void latr_fdt_end_node(LatrFdt *fdt)
{
    if (fdt->depth == 0) {
        fdt->failed = true;
        return;
    }
    if (!reserve(fdt, 4)) {
        return;
    }
    append_cell(fdt, FDT_END_NODE);
    fdt->depth--;
}

void latr_fdt_prop_cells(LatrFdt *fdt, const char *name,
                         const uint32_t *cells, uint32_t count)
{
    uint32_t name_off = string_offset(fdt, name);
    uint32_t bytes = count * 4u;

    if (fdt->failed || !reserve(fdt, 8u + align4(bytes))) {
        return;
    }
    append_cell(fdt, FDT_PROP);
    append_cell(fdt, bytes);
    append_cell(fdt, name_off);
    for (uint32_t i = 0; i < count; i++) {
        append_cell(fdt, cells[i]);
    }
}

void latr_fdt_prop_u32(LatrFdt *fdt, const char *name, uint32_t value)
{
    latr_fdt_prop_cells(fdt, name, &value, 1);
}

void latr_fdt_prop_u64(LatrFdt *fdt, const char *name, uint64_t value)
{
    uint32_t cells[2] = { (uint32_t)(value >> 32), (uint32_t)value };

    latr_fdt_prop_cells(fdt, name, cells, 2);
}

void latr_fdt_prop_string(LatrFdt *fdt, const char *name, const char *value)
{
    uint32_t name_off = string_offset(fdt, name);
    uint32_t len = (uint32_t)strlen(value) + 1u;

    if (fdt->failed || !reserve(fdt, 8u + align4(len))) {
        return;
    }
    append_cell(fdt, FDT_PROP);
    append_cell(fdt, len);
    append_cell(fdt, name_off);
    append_bytes(fdt, value, len);
}

void latr_fdt_prop_empty(LatrFdt *fdt, const char *name)
{
    uint32_t name_off = string_offset(fdt, name);

    if (fdt->failed || !reserve(fdt, 12)) {
        return;
    }
    append_cell(fdt, FDT_PROP);
    append_cell(fdt, 0);
    append_cell(fdt, name_off);
}

void latr_fdt_prop_strings(LatrFdt *fdt, const char *name,
                           const char *const *values, uint32_t count)
{
    uint32_t name_off = string_offset(fdt, name);
    uint32_t len = 0;

    for (uint32_t i = 0; i < count; i++) {
        len += (uint32_t)strlen(values[i]) + 1u;
    }
    if (fdt->failed || !reserve(fdt, 8u + align4(len))) {
        return;
    }
    append_cell(fdt, FDT_PROP);
    append_cell(fdt, len);
    append_cell(fdt, name_off);
    for (uint32_t i = 0; i < count; i++) {
        append_raw(fdt, values[i], (uint32_t)strlen(values[i]) + 1u);
    }
    pad_struct(fdt);
}

uint32_t latr_fdt_finish(LatrFdt *fdt)
{
    uint32_t strings_off;
    uint32_t total;

    if (fdt->failed || fdt->depth != 0) {
        return 0;
    }
    if (!reserve(fdt, 4)) {
        return 0;
    }
    append_cell(fdt, FDT_END);
    strings_off = align4(fdt->struct_off + fdt->struct_len);
    total = align4(strings_off + fdt->strings_len);
    if (total > fdt->cap) {
        fdt->failed = true;
        return 0;
    }
    memcpy(fdt->buf + strings_off, fdt->strings, fdt->strings_len);
    put_be32(fdt->buf + 4, total);
    put_be32(fdt->buf + 8, fdt->struct_off);
    put_be32(fdt->buf + 12, strings_off);
    put_be32(fdt->buf + 32, fdt->strings_len);
    put_be32(fdt->buf + 36, fdt->struct_len);
    return total;
}
