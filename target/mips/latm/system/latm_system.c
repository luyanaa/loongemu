#include "qemu/osdep.h"
#include "latm-system-board.h"

static uint64_t sys_mailbox_read(void *opaque, uint64_t off, unsigned size)
{
    LatmSystem *sys = opaque;
    uint64_t v = 0;
    latm_uncore_mailbox_read(&sys->uncore,
                             LATM_MAILBOX_BASE + (uint32_t)off, size, &v);
    return v;
}

static void sys_mailbox_write(void *opaque, uint64_t off, uint64_t value,
                              unsigned size)
{
    LatmSystem *sys = opaque;
    latm_uncore_mailbox_write(&sys->uncore,
                              LATM_MAILBOX_BASE + (uint32_t)off, size, value);
}

static uint64_t sys_pic_read(void *opaque, uint64_t off, unsigned size)
{
    LatmSystem *sys = opaque;
    uint64_t v = 0;
    latm_uncore_pic_read(&sys->uncore, (uint32_t)off, size, &v);
    return v;
}

static void sys_pic_write(void *opaque, uint64_t off, uint64_t value,
                          unsigned size)
{
    LatmSystem *sys = opaque;
    latm_uncore_pic_write(&sys->uncore, (uint32_t)off, size, value);
}

static uint64_t sys_pci_cfg_read_one(LatmSystem *sys, uint64_t off,
                                     unsigned size, int type1)
{
    unsigned bus = type1 ? (unsigned)((off >> 16) & 0xffu) : 0;
    unsigned dev = (unsigned)((off >> 11) & 0x1fu);
    unsigned fn = (unsigned)((off >> 8) & 0x07u);
    unsigned reg = (unsigned)(((off >> 16) & 0xf00u) | (off & 0xffu));
    (void)size;
    return latm_pci_cfg_read(&sys->pci, (uint8_t)bus, (uint8_t)dev,
                             (uint8_t)fn, (uint16_t)reg, 4);
}

static uint64_t sys_pci_cfg0_read(void *opaque, uint64_t off, unsigned size)
{
    return sys_pci_cfg_read_one(opaque, off, size, 0);
}

static uint64_t sys_pci_cfg1_read(void *opaque, uint64_t off, unsigned size)
{
    return sys_pci_cfg_read_one(opaque, off, size, 1);
}

static void sys_pci_cfg_write_one(LatmSystem *sys, uint64_t off,
                                  uint64_t value, unsigned size, int type1)
{
    unsigned bus = type1 ? (unsigned)((off >> 16) & 0xffu) : 0;
    unsigned dev = (unsigned)((off >> 11) & 0x1fu);
    unsigned fn = (unsigned)((off >> 8) & 0x07u);
    unsigned reg = (unsigned)(((off >> 16) & 0xf00u) | (off & 0xffu));
    latm_pci_cfg_write(&sys->pci, (uint8_t)bus, (uint8_t)dev, (uint8_t)fn,
                       (uint16_t)reg, (uint32_t)value, size > 4 ? 4 : size);
}

static void sys_pci_cfg0_write(void *opaque, uint64_t off, uint64_t value,
                               unsigned size)
{
    sys_pci_cfg_write_one(opaque, off, value, size, 0);
}

static void sys_pci_cfg1_write(void *opaque, uint64_t off, uint64_t value,
                               unsigned size)
{
    sys_pci_cfg_write_one(opaque, off, value, size, 1);
}

static uint64_t sys_msi_read(void *opaque, uint64_t off, unsigned size)
{
    (void)opaque; (void)off; (void)size;
    return 0;
}

static void sys_msi_write(void *opaque, uint64_t off, uint64_t value,
                          unsigned size)
{
    LatmSystem *sys = opaque;
    latm_uncore_msi_write(&sys->uncore, LATM_PCH_MSI_BASE + (uint32_t)off,
                          size, value);
}

int latm_system_init_3a4000(LatmSystem *sys, uint8_t *ram, uint64_t ram_hpa,
                            uint64_t ram_size, uint64_t vmm_base,
                            uint64_t vmm_size, uint64_t code_base,
                            uint64_t code_size)
{
    if (!sys || !ram || !ram_size) return 0;
    memset(sys, 0, sizeof(*sys));
    sys->ram = ram;
    sys->ram_hpa = ram_hpa;
    sys->ram_size = ram_size;
    sys->cfg_type0_base = LOONGSON_7A_CFG_TYPE0_BASE;
    sys->cfg_type1_base = LOONGSON_7A_CFG_TYPE1_BASE;
    latm_uncore_init(&sys->uncore);
    latm_uart_init(&sys->uart);
    latm_mmio_init(&sys->mmio);
    if (!latm_memory_setup_3a4000(&sys->memory, ram_hpa, ram_size,
                                  vmm_base, vmm_size, code_base, code_size)) {
        return 0;
    }
    uint64_t low = sys->memory.low_ram_size;
    uint64_t high = sys->memory.high_ram_size;
    if (!latm_mmio_add(&sys->mmio, 0, low, LATM_MMIO_RAM, 1,
                       latm_mmio_ram_read, latm_mmio_ram_write, ram,
                       "low-ram")) return 0;
    if (high && !latm_mmio_add(&sys->mmio, 0x90000000ull, high,
                               LATM_MMIO_RAM, 1, latm_mmio_ram_read,
                               latm_mmio_ram_write, ram + low, "high-ram")) {
        return 0;
    }
    latm_mmio_add(&sys->mmio, LATM_MAILBOX_BASE,
                  LATM_MAX_CPUS * LATM_MAILBOX_CORE_STRIDE, LATM_MMIO_DEVICE,
                  0, sys_mailbox_read, sys_mailbox_write, sys, "mailbox");
    latm_mmio_add(&sys->mmio, 0x1fe001e0ull, 0x100, LATM_MMIO_DEVICE, 0,
                  latm_uart_read, latm_uart_write, &sys->uart, "uart0");
    latm_mmio_add(&sys->mmio, LATM_PCH_PIC_BASE, 0x1000, LATM_MMIO_DEVICE,
                  0, sys_pic_read, sys_pic_write, sys, "pch-pic");
    latm_mmio_add(&sys->mmio, LATM_PCH_MSI_BASE, 0x100000,
                  LATM_MMIO_DEVICE, 0, sys_msi_read, sys_msi_write, sys,
                  "pch-msi");
    latm_pci_init(&sys->pci, NULL);
    latm_pci_bind_bus(&sys->pci, &sys->mmio);
    latm_mmio_add(&sys->mmio, sys->cfg_type0_base, 0x10000000ull,
                  LATM_MMIO_DEVICE, 0, sys_pci_cfg0_read, sys_pci_cfg0_write,
                  sys, "7a-cfg-type0");
    latm_mmio_add(&sys->mmio, sys->cfg_type1_base, 0x10000000ull,
                  LATM_MMIO_DEVICE, 0, sys_pci_cfg1_read, sys_pci_cfg1_write,
                  sys, "7a-cfg-type1");
    latm_pci_map_bochs_mmio(&sys->pci, &sys->mmio);
    latm_boot_info_init(&sys->boot);
    latm_system_bind_platform(sys);
    return 1;
}

int latm_system_init_from_handoff(LatmSystem *sys,
                                  const LoongemuVmmHandoff *handoff,
                                  uint8_t *ram)
{
    if (!sys || !handoff || !ram ||
        handoff->magic != LOONGEMU_VMM_HANDOFF_MAGIC ||
        handoff->version != LOONGEMU_VMM_HANDOFF_VERSION ||
        !handoff->guest_ram_hpa || !handoff->guest_ram_size) {
        return 0;
    }
    if (!latm_system_init_3a4000(sys, ram, handoff->guest_ram_hpa,
                                 handoff->guest_ram_size,
                                 handoff->vmm_image_base,
                                 handoff->vmm_image_size,
                                 handoff->code_cache_hpa,
                                 handoff->code_cache_size)) {
        return 0;
    }
    if (handoff->ls7a_cfg_type0_base &&
        handoff->ls7a_cfg_type0_base != sys->cfg_type0_base) {
        latm_mmio_remove(&sys->mmio, sys->cfg_type0_base);
        sys->cfg_type0_base = handoff->ls7a_cfg_type0_base;
        latm_mmio_add(&sys->mmio, sys->cfg_type0_base, 0x10000000ull,
                      LATM_MMIO_DEVICE, 0, sys_pci_cfg0_read,
                      sys_pci_cfg0_write, sys, "7a-cfg-type0");
    }
    if (handoff->ls7a_cfg_type1_base &&
        handoff->ls7a_cfg_type1_base != sys->cfg_type1_base) {
        latm_mmio_remove(&sys->mmio, sys->cfg_type1_base);
        sys->cfg_type1_base = handoff->ls7a_cfg_type1_base;
        latm_mmio_add(&sys->mmio, sys->cfg_type1_base, 0x10000000ull,
                      LATM_MMIO_DEVICE, 0, sys_pci_cfg1_read,
                      sys_pci_cfg1_write, sys, "7a-cfg-type1");
    }
    return 1;
}


static uint8_t *sys_ram_host(LatmSystem *sys, uint64_t gpa, unsigned size)
{
    if (!sys || (size != 4 && size != 8) || (gpa & (size - 1))) return NULL;
    const LatmMemoryRegion *r = latm_memory_find(&sys->memory, gpa, size);
    if (!r || r->kind != LATM_MMIO_RAM || r->hpa < sys->ram_hpa) return NULL;
    uint64_t roff = r->hpa - sys->ram_hpa;
    uint64_t goff = gpa - r->base;
    if (roff > sys->ram_size || goff > sys->ram_size - roff ||
        size > sys->ram_size - roff - goff) return NULL;
    return sys->ram + roff + goff;
}

static int sys_mem_read(CPUMIPSState *env, uint64_t gpa, uint64_t *value,
                        uint32_t size)
{
    (void)env;
    LatmSystem *sys = latm_platform_get()->opaque;
    uint64_t v;
    uint8_t *host = sys_ram_host(sys, gpa, size);
    if (!sys || !value || !size) return LATM_MEM_FAULT;
    if (host) {
        *value = size == 4 ? __atomic_load_n((uint32_t *)host, __ATOMIC_ACQUIRE)
                           : __atomic_load_n((uint64_t *)host, __ATOMIC_ACQUIRE);
        return LATM_MEM_OK;
    }
    if (!latm_system_mmio_read(sys, gpa, size, &v)) {
        return LATM_MEM_FAULT;
    }
    *value = v;
    return LATM_MEM_OK;
}

static int sys_mem_write(CPUMIPSState *env, uint64_t gpa, uint64_t value,
                         uint32_t size)
{
    (void)env;
    LatmSystem *sys = latm_platform_get()->opaque;
    uint8_t *host = sys_ram_host(sys, gpa, size);
    if (!sys || !size) return LATM_MEM_FAULT;
    if (host) {
        if (size == 4) {
            __atomic_store_n((uint32_t *)host, (uint32_t)value,
                             __ATOMIC_RELEASE);
        } else {
            __atomic_store_n((uint64_t *)host, value, __ATOMIC_RELEASE);
        }
        return LATM_MEM_OK;
    }
    if (!latm_system_mmio_write(sys, gpa, size, value)) {
        return LATM_MEM_FAULT;
    }
    return LATM_MEM_OK;
}

static int sys_fetch32(CPUMIPSState *env, uint64_t gpa, uint32_t *value)
{
    uint64_t v;
    if (!value) return LATM_MEM_FAULT;
    int status = sys_mem_read(env, gpa, &v, 4);
    if (status != LATM_MEM_OK) return status;
    *value = (uint32_t)v;
    return LATM_MEM_OK;
}

static int sys_mem_probe(CPUMIPSState *env, uint64_t gpa, uint32_t size)
{
    (void)env;
    LatmSystem *sys = latm_platform_get()->opaque;
    if (!sys || !size) return LATM_MEM_FAULT;
    return latm_mmio_find(&sys->mmio, gpa, size) ? LATM_MEM_OK :
                                                   LATM_MEM_FAULT;
}

static uint64_t sys_iocsr_read(CPUMIPSState *env, uint64_t addr, unsigned size)
{
    LatmSystem *sys = latm_platform_get()->opaque;
    unsigned cpu = env ? env->latm_cpu_index : 0;
    return latm_uncore_iocsr_read_cpu(sys ? &sys->uncore : NULL, addr, size,
                                      cpu);
}

static void sys_iocsr_write(CPUMIPSState *env, uint64_t addr, uint64_t value,
                            unsigned size)
{
    LatmSystem *sys = latm_platform_get()->opaque;
    unsigned cpu = env ? env->latm_cpu_index : 0;
    if (sys) latm_uncore_iocsr_write_cpu(&sys->uncore, addr, value, size, cpu);
}

static int sys_atomic_ram(LatmSystem *sys, uint64_t gpa, unsigned size,
                          void **host)
{
    if (!sys || !host || (size != 4 && size != 8) || (gpa & (size - 1))) {
        return LATM_MEM_FAULT;
    }
    const LatmMemoryRegion *r = latm_memory_find(&sys->memory, gpa, size);
    if (!r || r->kind != LATM_MMIO_RAM) {
        return LATM_MEM_RESERVED;
    }
    if (r->hpa < sys->ram_hpa) return LATM_MEM_FAULT;
    uint64_t roff = r->hpa - sys->ram_hpa;
    uint64_t goff = gpa - r->base;
    if (roff > sys->ram_size || goff > sys->ram_size - roff ||
        size > sys->ram_size - roff - goff) {
        return LATM_MEM_FAULT;
    }
    *host = sys->ram + roff + goff;
    return LATM_MEM_OK;
}

static int sys_atomic_ll(CPUMIPSState *env, uint64_t gpa, unsigned size,
                         uint64_t *value)
{
    (void)env;
    LatmSystem *sys = latm_platform_get()->opaque;
    void *host;
    if (!value) return LATM_MEM_FAULT;
    int status = sys_atomic_ram(sys, gpa, size, &host);
    if (status != LATM_MEM_OK) return status;
    *value = size == 4 ? __atomic_load_n((uint32_t *)host, __ATOMIC_ACQUIRE)
                       : __atomic_load_n((uint64_t *)host, __ATOMIC_ACQUIRE);
    return LATM_MEM_OK;
}

static int sys_atomic_sc(CPUMIPSState *env, uint64_t gpa, unsigned size,
                         uint64_t expected, uint64_t desired, int *ok)
{
    (void)env;
    LatmSystem *sys = latm_platform_get()->opaque;
    void *host;
    int status;
    if (!ok) return LATM_MEM_FAULT;
    status = sys_atomic_ram(sys, gpa, size, &host);
    if (status != LATM_MEM_OK) return status;
    if (size == 4) {
        uint32_t old = (uint32_t)expected;
        *ok = __atomic_compare_exchange_n((uint32_t *)host, &old,
                                          (uint32_t)desired, false,
                                          __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST);
    } else {
        uint64_t old = expected;
        *ok = __atomic_compare_exchange_n((uint64_t *)host, &old, desired,
                                          false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST);
    }
    return LATM_MEM_OK;
}

void latm_system_bind_platform(LatmSystem *sys)
{
    if (!sys) return;
    memset(&sys->platform, 0, sizeof(sys->platform));
    sys->platform.mem_read = sys_mem_read;
    sys->platform.mem_write = sys_mem_write;
    sys->platform.fetch32 = sys_fetch32;
    sys->platform.mem_probe = sys_mem_probe;
    sys->platform.iocsr_read = sys_iocsr_read;
    sys->platform.iocsr_write = sys_iocsr_write;
    sys->platform.atomic_ll = sys_atomic_ll;
    sys->platform.atomic_sc = sys_atomic_sc;
    sys->platform.opaque = sys;
    latm_platform_set(&sys->platform);
}

const LatmPlatform *latm_system_platform(LatmSystem *sys)
{
    return sys ? &sys->platform : NULL;
}

int latm_system_mmio_read(LatmSystem *sys, uint64_t gpa, unsigned size,
                          uint64_t *value)
{
    return sys && latm_mmio_read(&sys->mmio, gpa, size, value);
}

int latm_system_mmio_write(LatmSystem *sys, uint64_t gpa, unsigned size,
                           uint64_t value)
{
    return sys && latm_mmio_write(&sys->mmio, gpa, size, value);
}

size_t latm_system_build_dtb(LatmSystem *sys, uint8_t *out, size_t cap,
                             uint8_t *struct_buf, size_t struct_cap,
                             uint8_t *strings_buf, size_t strings_cap)
{
    return sys ? latm_dtb_build_3a4000(out, cap, struct_buf, struct_cap,
                                       strings_buf, strings_cap,
                                       &sys->memory, &sys->boot) : 0;
}

void latm_system_set_boot_args(LatmSystem *sys, uint64_t dtb_gpa,
                               uint64_t initrd_start, uint64_t initrd_end,
                               const char *cmdline)
{
    if (!sys) return;
    latm_boot_info_set_dtb(&sys->boot, dtb_gpa, 0);
    latm_boot_info_set_initrd(&sys->boot, initrd_start, initrd_end);
    latm_boot_info_set_cmdline(&sys->boot, cmdline);
}

void latm_system_apply_boot_args(LatmSystem *sys, void *env)
{
    if (sys) latm_boot_info_apply_args(&sys->boot, env);
}


static uint16_t elf_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t elf_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t elf_u64(const uint8_t *p)
{
    return (uint64_t)elf_u32(p) | ((uint64_t)elf_u32(p + 4) << 32);
}

static uint64_t elf_load_gpa(uint64_t addr)
{
    /* MIPS64 KSEG0 vmlinux links at 0xffffffff8xxxxxxx; a 32-bit KSEG0 kernel
     * (NetBSD evbmips ELF32) links at 0x8xxxxxxx. Both alias physical memory in
     * the low 512 MiB. */
    if (addr >= UINT64_C(0xffffffff80000000)) {
        return addr & UINT64_C(0x1fffffff);
    }
    if (addr >= UINT64_C(0x80000000) && addr < UINT64_C(0xc0000000)) {
        return addr - UINT64_C(0x80000000);
    }
    return addr;
}

int latm_system_load_elf(LatmSystem *sys, const uint8_t *elf, size_t len,
                         uint64_t *entry)
{
    if (!sys || !elf || len < 52 || elf[0] != 0x7f || elf[1] != 'E' ||
        elf[2] != 'L' || elf[3] != 'F' || elf[5] != 1 ||
        (elf[4] != 1 && elf[4] != 2) /* ELFCLASS32/64 */ ||
        elf_u16(elf + 18) != 8 /* EM_MIPS */ ||
        (elf_u16(elf + 16) != 2 && elf_u16(elf + 16) != 3)) {
        return 0;
    }
    int is64 = elf[4] == 2;
    uint64_t phoff = is64 ? elf_u64(elf + 32) : elf_u32(elf + 28);
    uint64_t entry_pc = is64 ? elf_u64(elf + 24) : elf_u32(elf + 24);
    uint16_t phentsize = elf_u16(elf + (is64 ? 54 : 42));
    uint16_t phnum = elf_u16(elf + (is64 ? 56 : 44));
    uint16_t phentsize_min = is64 ? 56 : 32;
    if (!phnum || phentsize < phentsize_min || phoff > len ||
        (uint64_t)phnum * phentsize > len - phoff) {
        return 0;
    }
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (uint64_t)i * phentsize;
        if (elf_u32(ph) != 1) continue; /* PT_LOAD */
        uint64_t off = is64 ? elf_u64(ph + 8) : elf_u32(ph + 4);
        uint64_t vaddr = is64 ? elf_u64(ph + 16) : elf_u32(ph + 8);
        uint64_t paddr = is64 ? elf_u64(ph + 24) : elf_u32(ph + 12);
        uint64_t filesz = is64 ? elf_u64(ph + 32) : elf_u32(ph + 16);
        uint64_t memsz = is64 ? elf_u64(ph + 40) : elf_u32(ph + 20);
        uint64_t dst = elf_load_gpa(paddr ? paddr : vaddr);
        if (off > len || filesz > len - off || memsz < filesz ||
            memsz == 0 || memsz > UINT32_MAX || dst + memsz < dst) return 0;
        const LatmMemoryRegion *r = latm_memory_find(&sys->memory, dst,
                                                     (unsigned)memsz);
        if (!r || r->kind != LATM_MMIO_RAM || !sys->ram) return 0;
        uint64_t hpa = r->hpa + (dst - r->base);
        if (hpa < sys->ram_hpa || hpa - sys->ram_hpa > sys->ram_size ||
            memsz > sys->ram_size - (hpa - sys->ram_hpa)) return 0;
        uint8_t *host = sys->ram + (hpa - sys->ram_hpa);
        memcpy(host, elf + off, filesz);
        memset(host + filesz, 0, memsz - filesz);
    }
    sys->boot.entry_pc = entry_pc;
    if (entry) *entry = entry_pc;
    return 1;
}


int latm_system_load_initrd(LatmSystem *sys, const uint8_t *data, size_t len,
                            uint64_t gpa)
{
    if (!sys || !data || !len) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!latm_system_mmio_write(sys, gpa + i, 1, data[i])) return 0;
    }
    latm_boot_info_set_initrd(&sys->boot, gpa, gpa + len);
    return 1;
}


int latm_system_boot(LatmSystem *sys, const uint8_t *elf, size_t elf_len,
                     const uint8_t *initrd, size_t initrd_len,
                     uint64_t dtb_gpa, uint64_t initrd_gpa,
                     const char *cmdline, CPUMIPSState *env)
{
    uint64_t entry;
    if (!sys || !env || !latm_system_load_elf(sys, elf, elf_len, &entry)) {
        return 0;
    }
    if (initrd && initrd_len &&
        !latm_system_load_initrd(sys, initrd, initrd_len, initrd_gpa)) {
        return 0;
    }
    latm_system_set_boot_args(sys, dtb_gpa, initrd_gpa,
                              initrd_gpa + (initrd ? initrd_len : 0),
                              cmdline);
    latm_system_apply_boot_args(sys, env);
    return env->active_tc.PC == entry;
}
