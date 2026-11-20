#include "qemu/osdep.h"
#include <stdio.h>
#include <string.h>
#include "latm-system.h"

#define DTB_HEADER_SIZE 40u
#define DTB_RSV_SIZE    16u
#define DTB_STRUCT_OFF  (DTB_HEADER_SIZE + DTB_RSV_SIZE)

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static void dtb_align(LatmDtbBuilder *b)
{
    while ((b->struct_len & 3u) != 0 && b->struct_len < b->struct_cap) {
        b->structs[b->struct_len++] = 0;
    }
}

static int dtb_put(LatmDtbBuilder *b, const void *data, size_t len)
{
    if (b->error || b->struct_len > b->struct_cap ||
        len > b->struct_cap - b->struct_len) {
        b->error = 1;
        return 0;
    }
    if (len) memcpy(b->structs + b->struct_len, data, len);
    b->struct_len += len;
    return 1;
}

static uint32_t dtb_string(LatmDtbBuilder *b, const char *s)
{
    size_t len = strlen(s) + 1;
    for (size_t off = 0; off < b->strings_len;) {
        if (strcmp((char *)b->strings + off, s) == 0) return (uint32_t)off;
        off += strlen((char *)b->strings + off) + 1;
    }
    if (b->strings_len + len > b->strings_cap) {
        b->error = 1;
        return 0;
    }
    uint32_t off = (uint32_t)b->strings_len;
    memcpy(b->strings + b->strings_len, s, len);
    b->strings_len += len;
    return off;
}

void latm_dtb_init(LatmDtbBuilder *b, uint8_t *out, size_t out_cap,
                   uint8_t *struct_buf, size_t struct_cap,
                   uint8_t *strings_buf, size_t strings_cap)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->out = out;
    b->cap = out_cap;
    b->structs = struct_buf;
    b->struct_cap = struct_cap;
    b->strings = strings_buf;
    b->strings_cap = strings_cap;
}

void latm_dtb_begin_node(LatmDtbBuilder *b, const char *name)
{
    uint32_t token = __builtin_bswap32(1);
    size_t len = strlen(name) + 1;
    if (!b || !dtb_put(b, &token, 4)) return;
    if (!dtb_put(b, name, len)) return;
    dtb_align(b);
}

void latm_dtb_end_node(LatmDtbBuilder *b)
{
    uint32_t token = __builtin_bswap32(2);
    (void)dtb_put(b, &token, 4);
}

void latm_dtb_prop(LatmDtbBuilder *b, const char *name, const void *data,
                   size_t len)
{
    uint32_t token = __builtin_bswap32(3);
    if (!b || len > UINT32_MAX) { if (b) b->error = 1; return; }
    uint32_t name_off = dtb_string(b, name);
    uint32_t be_name = __builtin_bswap32(name_off);
    uint32_t be_len = __builtin_bswap32((uint32_t)len);
    if (!dtb_put(b, &token, 4) || !dtb_put(b, &be_len, 4) ||
        !dtb_put(b, &be_name, 4) || !dtb_put(b, data, len)) return;
    dtb_align(b);
}

void latm_dtb_prop_u32(LatmDtbBuilder *b, const char *name, uint32_t value)
{
    uint32_t be = __builtin_bswap32(value);
    latm_dtb_prop(b, name, &be, sizeof(be));
}

void latm_dtb_prop_u64(LatmDtbBuilder *b, const char *name, uint64_t value)
{
    uint8_t be[8];
    put_be64(be, value);
    latm_dtb_prop(b, name, be, sizeof(be));
}

void latm_dtb_prop_string(LatmDtbBuilder *b, const char *name,
                          const char *value)
{
    latm_dtb_prop(b, name, value, strlen(value) + 1);
}

void latm_dtb_prop_cells(LatmDtbBuilder *b, const char *name,
                         const uint32_t *cells, size_t count)
{
    uint8_t buf[64];
    if (count * 4u > sizeof(buf)) { b->error = 1; return; }
    for (size_t i = 0; i < count; i++) put_be32(buf + i * 4u, cells[i]);
    latm_dtb_prop(b, name, buf, count * 4u);
}

size_t latm_dtb_finish(LatmDtbBuilder *b)
{
    if (!b || b->error || !b->out) return 0;
    size_t total = DTB_STRUCT_OFF + b->struct_len + b->strings_len;
    if (total > b->cap) return 0;
    memset(b->out, 0, total);
    put_be32(b->out + 0x00, 0xd00dfeedu);
    put_be32(b->out + 0x04, (uint32_t)total);
    put_be32(b->out + 0x08, DTB_STRUCT_OFF);
    put_be32(b->out + 0x0c, (uint32_t)(DTB_STRUCT_OFF + b->struct_len));
    put_be32(b->out + 0x10, DTB_HEADER_SIZE);
    put_be32(b->out + 0x14, 17); /* version */
    put_be32(b->out + 0x18, 16); /* last compatible */
    put_be32(b->out + 0x1c, 0);  /* boot CPU */
    put_be32(b->out + 0x20, (uint32_t)b->strings_len);
    put_be32(b->out + 0x24, (uint32_t)b->struct_len);
    /* reserve map: two 64-bit zero cells at 40 */
    memcpy(b->out + DTB_STRUCT_OFF, b->structs, b->struct_len);
    memcpy(b->out + DTB_STRUCT_OFF + b->struct_len, b->strings,
           b->strings_len);
    return total;
}

static void add_memory_nodes(LatmDtbBuilder *b, const LatmMemoryLayout *mem)
{
    for (unsigned i = 0; i < mem->count; i++) {
        const LatmMemoryRegion *r = &mem->regions[i];
        if (r->kind != LATM_MMIO_RAM) continue;
        char name[32];
        snprintf(name, sizeof(name), "memory@%llx",
                 (unsigned long long)r->base);
        latm_dtb_begin_node(b, name);
        latm_dtb_prop_string(b, "device_type", "memory");
        uint32_t cells[4] = {
            (uint32_t)(r->base >> 32), (uint32_t)r->base,
            (uint32_t)(r->size >> 32), (uint32_t)r->size,
        };
        latm_dtb_prop_cells(b, "reg", cells, 4);
        latm_dtb_end_node(b);
    }
}

static void add_reserved_nodes(LatmDtbBuilder *b, const LatmMemoryLayout *mem)
{
    latm_dtb_begin_node(b, "reserved-memory");
    latm_dtb_prop_u32(b, "#address-cells", 2);
    latm_dtb_prop_u32(b, "#size-cells", 2);
    latm_dtb_prop(b, "ranges", 0, 0);
    for (unsigned i = 0; i < mem->count; i++) {
        const LatmMemoryRegion *r = &mem->regions[i];
        if (r->kind != LATM_MMIO_RESERVED || r->name == NULL) continue;
        char name[48];
        snprintf(name, sizeof(name), "%s@%llx", r->name,
                 (unsigned long long)r->base);
        latm_dtb_begin_node(b, name);
        uint32_t cells[4] = {
            (uint32_t)(r->base >> 32), (uint32_t)r->base,
            (uint32_t)(r->size >> 32), (uint32_t)r->size,
        };
        latm_dtb_prop_cells(b, "reg", cells, 4);
        latm_dtb_prop(b, "no-map", 0, 0);
        latm_dtb_end_node(b);
    }
    latm_dtb_end_node(b);
}

size_t latm_dtb_build_3a4000(uint8_t *out, size_t cap,
                             uint8_t *struct_buf, size_t struct_cap,
                             uint8_t *strings_buf, size_t strings_cap,
                             const LatmMemoryLayout *mem,
                             const LatmBootInfo *boot)
{
    LatmDtbBuilder b;
    LatmBootInfo local_boot;

    if (!mem) return 0;
    if (!boot) { latm_boot_info_init(&local_boot); boot = &local_boot; }
    latm_dtb_init(&b, out, cap, struct_buf, struct_cap, strings_buf,
                  strings_cap);
    latm_dtb_begin_node(&b, "");
    latm_dtb_prop_u32(&b, "#address-cells", 2);
    latm_dtb_prop_u32(&b, "#size-cells", 2);
    latm_dtb_prop_string(&b, "compatible",
                         "loongson,loongson64v-4core-ls7a");
    latm_dtb_prop_string(&b, "model", "Loongson-3A4000 LS7A");
    add_memory_nodes(&b, mem);
    add_reserved_nodes(&b, mem);

    latm_dtb_begin_node(&b, "cpus");
    latm_dtb_prop_u32(&b, "#address-cells", 1);
    latm_dtb_prop_u32(&b, "#size-cells", 0);
    for (unsigned cpu = 0; cpu < 4; cpu++) {
        char name[16], core[16];
        snprintf(name, sizeof(name), "cpu@%u", cpu);
        latm_dtb_begin_node(&b, name);
        latm_dtb_prop_string(&b, "device_type", "cpu");
        latm_dtb_prop_string(&b, "compatible", "loongson,gs464v");
        latm_dtb_prop_u32(&b, "reg", cpu);
        latm_dtb_prop_u32(&b, "clock-frequency", 1500000000u);
        latm_dtb_prop_string(&b, "enable-method", "loongson,3a4000");
        latm_dtb_end_node(&b);
        snprintf(core, sizeof(core), "core%u", cpu);
    }
    latm_dtb_begin_node(&b, "cpu-map");
    latm_dtb_begin_node(&b, "cluster0");
    for (unsigned cpu = 0; cpu < 4; cpu++) {
        char core[16];
        snprintf(core, sizeof(core), "core%u", cpu);
        latm_dtb_begin_node(&b, core);
        latm_dtb_prop_u32(&b, "reg", cpu);
        latm_dtb_end_node(&b);
    }
    latm_dtb_end_node(&b);
    latm_dtb_end_node(&b);
    latm_dtb_end_node(&b);

    latm_dtb_begin_node(&b, "chosen");
    if (boot->cmdline[0]) latm_dtb_prop_string(&b, "bootargs", boot->cmdline);
    latm_dtb_prop_string(&b, "stdout-path",
                         "/soc/serial@1fe001e0");
    if (boot->initrd_end > boot->initrd_start) {
        uint32_t start_cells[2] = {
            (uint32_t)(boot->initrd_start >> 32), (uint32_t)boot->initrd_start,
        };
        uint32_t end_cells[2] = {
            (uint32_t)(boot->initrd_end >> 32), (uint32_t)boot->initrd_end,
        };
        latm_dtb_prop_cells(&b, "linux,initrd-start", start_cells, 2);
        latm_dtb_prop_cells(&b, "linux,initrd-end", end_cells, 2);
    }
    latm_dtb_end_node(&b);

    latm_dtb_begin_node(&b, "interrupt-controller");
    latm_dtb_prop_u32(&b, "#interrupt-cells", 1);
    latm_dtb_prop(&b, "interrupt-controller", 0, 0);
    latm_dtb_prop_u32(&b, "phandle", 1);
    latm_dtb_end_node(&b);

    latm_dtb_begin_node(&b, "soc");
    latm_dtb_prop_u32(&b, "#address-cells", 2);
    latm_dtb_prop_u32(&b, "#size-cells", 2);
    latm_dtb_prop(&b, "ranges", 0, 0);
    latm_dtb_begin_node(&b, "serial@1fe001e0");
    latm_dtb_prop_string(&b, "compatible", "ns16550a");
    uint32_t serial_reg[4] = { 0, 0x1fe001e0u, 0, 0x100 };
    latm_dtb_prop_cells(&b, "reg", serial_reg, 4);
    latm_dtb_prop_u32(&b, "interrupt-parent", 1);
    latm_dtb_prop_u32(&b, "interrupts", 2);
    latm_dtb_end_node(&b);

    latm_dtb_begin_node(&b, "pic@10000000");
    latm_dtb_prop_string(&b, "compatible", "loongson,ls7a-pic");
    uint32_t pic_reg[4] = { 0, 0x10000000u, 0, 0x1000 };
    latm_dtb_prop_cells(&b, "reg", pic_reg, 4);
    latm_dtb_prop_u32(&b, "#interrupt-cells", 2);
    latm_dtb_prop(&b, "interrupt-controller", 0, 0);
    latm_dtb_prop_u32(&b, "interrupt-parent", 1);
    latm_dtb_end_node(&b);

    latm_dtb_begin_node(&b, "pci@40000000");
    latm_dtb_prop_string(&b, "compatible", "loongson,ls7a-pci");
    uint32_t pci_reg[4] = { 0, 0x40000000u, 0, 0x10000000u };
    latm_dtb_prop_cells(&b, "reg", pci_reg, 4);
    latm_dtb_prop(&b, "linux,pci-probe-only", 0, 0);
    latm_dtb_end_node(&b);
    latm_dtb_end_node(&b);
    latm_dtb_end_node(&b);

    uint32_t end = __builtin_bswap32(9);
    (void)dtb_put(&b, &end, 4);
    return latm_dtb_finish(&b);
}
