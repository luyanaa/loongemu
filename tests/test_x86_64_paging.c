#include "loongemu/x86_64_paging.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_MEMORY_SIZE (UINT64_C(1) << 21)
#define TEST_PML4       0x1000
#define TEST_PDPT       0x2000
#define TEST_PD         0x3000
#define TEST_PT         0x4000
#define TEST_PAGE       0x5000
#define TEST_LARGE_2M   0x200000

#define PTE_PRESENT   (UINT64_C(1) << 0)
#define PTE_WRITE     (UINT64_C(1) << 1)
#define PTE_USER      (UINT64_C(1) << 2)
#define PTE_ACCESSED  (UINT64_C(1) << 5)
#define PTE_DIRTY     (UINT64_C(1) << 6)
#define PTE_PS        (UINT64_C(1) << 7)
#define PTE_NX        (UINT64_C(1) << 63)
#define PTE_PKEY_SHIFT 59

typedef struct TestMemory {
    unsigned char bytes[TEST_MEMORY_SIZE];
    unsigned read_count;
    unsigned update_count;
    int fail_once;
    uint64_t replacement_addr;
    uint64_t replacement_value;
} TestMemory;

static int memory_read_u64(void *opaque, uint64_t address, uint64_t *value)
{
    TestMemory *memory = opaque;

    if (address > sizeof(memory->bytes) - sizeof(*value) || address % 8 != 0) {
        return -1;
    }
    memcpy(value, memory->bytes + address, sizeof(*value));
    memory->read_count++;
    return 0;
}

static int memory_atomic_or_u64(void *opaque, uint64_t address,
                                uint64_t bits, uint64_t *new_value)
{
    TestMemory *memory = opaque;

    if (address > sizeof(memory->bytes) - sizeof(*new_value) || address % 8 != 0) {
        return -1;
    }
    memcpy(new_value, memory->bytes + address, sizeof(*new_value));
    *new_value |= bits;
    memcpy(memory->bytes + address, new_value, sizeof(*new_value));
    memory->update_count++;
    return 0;
}

static int memory_cmpxchg_u64(void *opaque, uint64_t address,
                              uint64_t *expected, uint64_t desired)
{
    TestMemory *memory = opaque;
    uint64_t current;

    if (address > sizeof(memory->bytes) - sizeof(current) || address % 8 != 0 ||
        expected == NULL) {
        return -1;
    }
    if (memory->fail_once && address == memory->replacement_addr) {
        memory->fail_once = 0;
        memcpy(memory->bytes + address, &memory->replacement_value,
               sizeof(memory->replacement_value));
        *expected = memory->replacement_value;
        return 1;
    }
    memcpy(&current, memory->bytes + address, sizeof(current));
    if (current != *expected) {
        *expected = current;
        return 1;
    }
    memcpy(memory->bytes + address, &desired, sizeof(desired));
    memory->update_count++;
    return 0;
}

static void store_entry(TestMemory *memory, uint64_t address, uint64_t value)
{
    assert(address <= sizeof(memory->bytes) - sizeof(value));
    memcpy(memory->bytes + address, &value, sizeof(value));
}

static uint64_t load_entry(TestMemory *memory, uint64_t address)
{
    uint64_t value;
    memcpy(&value, memory->bytes + address, sizeof(value));
    return value;
}

static X86PagingState default_state(void)
{
    X86PagingState state = {
        .cr0 = X86_CR0_PG | X86_CR0_WP,
        .cr3 = TEST_PML4,
        .cr4 = X86_CR4_PAE,
        .efer = X86_EFER_LMA | X86_EFER_NXE,
        .rflags = 2,
        .maxphyaddr = 48,
        .cpl = 0,
    };
    return state;
}

static void install_four_level_map(TestMemory *memory, uint64_t pml4e,
                                   uint64_t pdpte, uint64_t pde, uint64_t pte)
{
    memset(memory, 0, sizeof(*memory));
    store_entry(memory, TEST_PML4, pml4e);
    store_entry(memory, TEST_PDPT, pdpte);
    store_entry(memory, TEST_PD, pde);
    store_entry(memory, TEST_PT + 8, pte);
}

static X86PagingResult walk(TestMemory *memory, X86PagingState *state,
                            uint64_t linear, X86PagingAccess access,
                            X86Translation *translation, X86PageFault *fault)
{
    X86PagingMemory ops = {
        .opaque = memory,
        .read_u64 = memory_read_u64,
        .atomic_or_u64 = memory_atomic_or_u64,
    };
    return x86_64_walk_page_tables(state, &ops, linear, access, translation,
                                   fault);
}

static void test_four_level_translation_and_ad_bits(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;
    X86PagingResult result;

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PD | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE | PTE_USER);
    result = walk(&memory, &state, 0x1234, X86_PAGING_READ, &translation,
                  &fault);
    assert(result == X86_PAGING_OK);
    assert(translation.guest_physical_address == TEST_PAGE + 0x234);
    assert(translation.page_size == 4096);
    assert(translation.writable && translation.user_accessible &&
           translation.executable);
    assert(memory.read_count == 4);
    assert(memory.update_count == 4);
    assert(load_entry(&memory, TEST_PML4) & PTE_ACCESSED);
    assert(load_entry(&memory, TEST_PDPT) & PTE_ACCESSED);
    assert(load_entry(&memory, TEST_PD) & PTE_ACCESSED);
    assert(load_entry(&memory, TEST_PT + 8) & PTE_ACCESSED);

    result = walk(&memory, &state, 0x1234, X86_PAGING_WRITE, &translation,
                  &fault);
    assert(result == X86_PAGING_OK);
    assert(load_entry(&memory, TEST_PT + 8) & PTE_DIRTY);
}

static void test_two_megabyte_page_and_pat_bit(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;
    uint64_t pde = TEST_LARGE_2M | PTE_PRESENT | PTE_WRITE | PTE_PS;

    memset(&memory, 0, sizeof(memory));
    store_entry(&memory, TEST_PML4, TEST_PDPT | PTE_PRESENT | PTE_WRITE);
    store_entry(&memory, TEST_PDPT, TEST_PD | PTE_PRESENT | PTE_WRITE);
    store_entry(&memory, TEST_PD, pde | (UINT64_C(1) << 12));

    assert(walk(&memory, &state, 0x1abcde, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_OK);
    assert(translation.page_size == (UINT64_C(1) << 21));
    assert(translation.guest_physical_address == TEST_LARGE_2M + 0x1abcde);

    store_entry(&memory, TEST_PD, pde | (UINT64_C(1) << 13));
    assert(walk(&memory, &state, 0x1abcde, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code ==
           (X86_PF_PRESENT | X86_PF_RESERVED));
}

static void test_one_gigabyte_page_feature_and_alignment(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;
    uint64_t pdpte = (UINT64_C(1) << 30) | PTE_PRESENT | PTE_WRITE | PTE_PS;

    memset(&memory, 0, sizeof(memory));
    store_entry(&memory, TEST_PML4, TEST_PDPT | PTE_PRESENT | PTE_WRITE);
    store_entry(&memory, TEST_PDPT, pdpte);
    assert(walk(&memory, &state, 0x12345678, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code ==
           (X86_PF_PRESENT | X86_PF_RESERVED));

    state.features |= X86_PAGING_FEATURE_1G_PAGES;
    assert(walk(&memory, &state, 0x12345678, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_OK);
    assert(translation.page_size == (UINT64_C(1) << 30));
    assert(translation.guest_physical_address ==
           (UINT64_C(1) << 30) + 0x12345678);
}

static void test_nonpresent_and_permission_faults(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;

    install_four_level_map(&memory, TEST_PDPT | PTE_PRESENT,
                           TEST_PD | PTE_PRESENT, TEST_PT | PTE_PRESENT,
                           TEST_PAGE | PTE_WRITE);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code == 0);
    assert(fault.linear_address == 0x1000);

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE,
                           TEST_PD | PTE_PRESENT | PTE_WRITE,
                           TEST_PT | PTE_PRESENT | PTE_WRITE,
                           TEST_PAGE | PTE_PRESENT);
    state.cr0 &= ~X86_CR0_WP;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_WRITE, &translation,
                &fault) == X86_PAGING_OK);
    state.cr0 |= X86_CR0_WP;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_WRITE, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code == (X86_PF_PRESENT | X86_PF_WRITE));

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE,
                           TEST_PD | PTE_PRESENT | PTE_WRITE,
                           TEST_PT | PTE_PRESENT | PTE_WRITE,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE | PTE_NX);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_EXECUTE, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code == (X86_PF_PRESENT | X86_PF_INSN));

    state.efer &= ~X86_EFER_NXE;
    state.cpl = 3;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code ==
           (X86_PF_PRESENT | X86_PF_RESERVED | X86_PF_USER));
}

static void test_supervisor_user_permissions_and_smap_smep(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;
    uint64_t user_entries = TEST_PAGE | PTE_PRESENT | PTE_WRITE | PTE_USER;

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PD | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           user_entries);
    state.cr4 |= X86_CR4_SMEP;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_EXECUTE, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code == (X86_PF_PRESENT | X86_PF_INSN));

    state.cr4 &= ~X86_CR4_SMEP;
    state.cr4 |= X86_CR4_SMAP;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    state.rflags |= X86_RFLAGS_AC;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_OK);

    state.cpl = 3;
    state.rflags &= ~X86_RFLAGS_AC;
    state.cr4 &= ~X86_CR4_SMAP;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_OK);

    install_four_level_map(&memory, TEST_PDPT | PTE_PRESENT | PTE_WRITE,
                           TEST_PD | PTE_PRESENT | PTE_WRITE,
                           TEST_PT | PTE_PRESENT | PTE_WRITE,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code == (X86_PF_PRESENT | X86_PF_USER));
}

static void test_reserved_bits_and_key_permissions(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;
    uint64_t leaf;

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE,
                           TEST_PD | PTE_PRESENT | PTE_WRITE,
                           TEST_PT | PTE_PRESENT | PTE_WRITE,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE);
    leaf = load_entry(&memory, TEST_PT + 8) | (UINT64_C(1) << 52);
    store_entry(&memory, TEST_PT + 8, leaf);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_OK);

    state.cr4 |= X86_CR4_PKE;
    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PD | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE | PTE_USER |
                               (UINT64_C(1) << PTE_PKEY_SHIFT));
    state.cpl = 3;
    state.pkru = 1U << (2 * 1);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code ==
           (X86_PF_PRESENT | X86_PF_PROTECTION_KEY | X86_PF_USER));
    state.pkru = 2U << (2 * 1);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_WRITE, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code ==
           (X86_PF_PRESENT | X86_PF_WRITE | X86_PF_PROTECTION_KEY |
            X86_PF_USER));
    assert(walk(&memory, &state, 0x1000, X86_PAGING_EXECUTE, &translation,
                &fault) == X86_PAGING_OK);

    state.cpl = 0;
    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE,
                           TEST_PD | PTE_PRESENT | PTE_WRITE,
                           TEST_PT | PTE_PRESENT | PTE_WRITE,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE |
                               (UINT64_C(1) << PTE_PKEY_SHIFT));
    state.cr4 &= ~X86_CR4_PKE;
    state.cr4 |= X86_CR4_PKS;
    state.pkrs = 1U << (2 * 1);
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_PAGE_FAULT);
    assert(fault.error_code == (X86_PF_PRESENT | X86_PF_PROTECTION_KEY));
}

static void test_noncanonical_la57_and_cr3_states(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE,
                           TEST_PD | PTE_PRESENT | PTE_WRITE,
                           TEST_PT | PTE_PRESENT | PTE_WRITE,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE);
    assert(walk(&memory, &state, UINT64_C(0x0000800000000000),
                X86_PAGING_READ, &translation, &fault) ==
           X86_PAGING_GENERAL_PROTECTION);
    assert(fault.error_code == 0);

    state.cr4 |= X86_CR4_LA57;
    assert(walk(&memory, &state, 0, X86_PAGING_READ, &translation, &fault) ==
           X86_PAGING_UNSUPPORTED);
    state.cr4 &= ~X86_CR4_LA57;
    state.cr3 |= UINT64_C(1) << 60;
    assert(walk(&memory, &state, 0, X86_PAGING_READ, &translation, &fault) ==
           X86_PAGING_INVALID_STATE);

    state = default_state();
    state.cr3 = TEST_PML4 | 5;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_INVALID_STATE);
    state.cr4 |= X86_CR4_PCIDE;
    assert(walk(&memory, &state, 0x1000, X86_PAGING_READ, &translation,
                &fault) == X86_PAGING_OK);
}

static void test_physical_read_failures_are_not_guest_page_faults(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault = { .linear_address = UINT64_MAX, .error_code = UINT32_MAX };
    X86PagingMemory ops = {
        .opaque = &memory,
        .read_u64 = memory_read_u64,
        .atomic_or_u64 = memory_atomic_or_u64,
    };

    memset(&memory, 0, sizeof(memory));
    state.cr3 = TEST_MEMORY_SIZE;
    assert(x86_64_walk_page_tables(&state, &ops, 0x1000, X86_PAGING_READ,
                                   &translation, &fault) ==
           X86_PAGING_MEMORY_ERROR);
    assert(fault.linear_address == UINT64_MAX);
    assert(fault.error_code == UINT32_MAX);
}

static void test_ad_cmpxchg_restarts_from_cr3(void)
{
    TestMemory memory;
    X86PagingState state = default_state();
    X86Translation translation;
    X86PageFault fault;
    X86PagingMemory ops = {
        .opaque = &memory,
        .read_u64 = memory_read_u64,
        .cmpxchg_u64 = memory_cmpxchg_u64,
    };
    const uint64_t alt_pdpt = 0x6000;
    const uint64_t alt_pd = 0x7000;
    const uint64_t alt_pt = 0x8000;
    const uint64_t alt_page = 0x9000;

    install_four_level_map(&memory,
                           TEST_PDPT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PD | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PT | PTE_PRESENT | PTE_WRITE | PTE_USER,
                           TEST_PAGE | PTE_PRESENT | PTE_WRITE | PTE_USER);
    store_entry(&memory, alt_pdpt,
                alt_pd | PTE_PRESENT | PTE_WRITE | PTE_USER);
    store_entry(&memory, alt_pd,
                alt_pt | PTE_PRESENT | PTE_WRITE | PTE_USER);
    store_entry(&memory, alt_pt + 8,
                alt_page | PTE_PRESENT | PTE_WRITE | PTE_USER);
    memory.fail_once = 1;
    memory.replacement_addr = TEST_PML4;
    memory.replacement_value =
        alt_pdpt | PTE_PRESENT | PTE_WRITE | PTE_USER;

    assert(x86_64_walk_page_tables(&state, &ops, 0x1234, X86_PAGING_READ,
                                   &translation, &fault) == X86_PAGING_OK);
    assert(translation.guest_physical_address == alt_page + 0x234);
    assert((load_entry(&memory, TEST_PML4) & ~PTE_ACCESSED) ==
           memory.replacement_value);
    assert(load_entry(&memory, alt_pdpt) & PTE_ACCESSED);
    assert(load_entry(&memory, alt_pd) & PTE_ACCESSED);
}

int main(void)
{
    test_four_level_translation_and_ad_bits();
    test_two_megabyte_page_and_pat_bit();
    test_one_gigabyte_page_feature_and_alignment();
    test_nonpresent_and_permission_faults();
    test_supervisor_user_permissions_and_smap_smep();
    test_reserved_bits_and_key_permissions();
    test_noncanonical_la57_and_cr3_states();
    test_physical_read_failures_are_not_guest_page_faults();
    test_ad_cmpxchg_restarts_from_cr3();
    puts("x86_64 page walker: all tests passed");
    return 0;
}
