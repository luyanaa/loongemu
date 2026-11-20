#include "loongemu/x86_64_paging.h"

#define ENTRY_PRESENT  (UINT64_C(1) << 0)
#define ENTRY_WRITE    (UINT64_C(1) << 1)
#define ENTRY_USER     (UINT64_C(1) << 2)
#define ENTRY_PWT      (UINT64_C(1) << 3)
#define ENTRY_PCD      (UINT64_C(1) << 4)
#define ENTRY_ACCESSED (UINT64_C(1) << 5)
#define ENTRY_DIRTY    (UINT64_C(1) << 6)
#define ENTRY_PS       (UINT64_C(1) << 7)
#define ENTRY_GLOBAL   (UINT64_C(1) << 8)
#define ENTRY_NX       (UINT64_C(1) << 63)
#define ENTRY_PKEY_MASK (UINT64_C(0xf) << 59)
#define ENTRY_SOFTWARE_MASK (UINT64_C(0x7f) << 52)
#define CR3_PCID_MASK  UINT64_C(0xfff)
#define RFLAGS_AC      (UINT64_C(1) << 18)

static uint64_t physical_address_mask(uint8_t maxphyaddr)
{
    return ((UINT64_C(1) << maxphyaddr) - 1) & ~UINT64_C(0xfff);
}

static uint32_t access_error_bits(const X86PagingState *state,
                                  X86PagingAccess access)
{
    uint32_t error = state->cpl == 3 ? X86_PF_USER : 0;

    if (access == X86_PAGING_WRITE) {
        error |= X86_PF_WRITE;
    } else if (access == X86_PAGING_EXECUTE) {
        error |= X86_PF_INSN;
    }
    return error;
}

static X86PagingResult page_fault(const X86PagingState *state,
                                  uint64_t linear_address,
                                  X86PagingAccess access,
                                  uint32_t extra,
                                  X86PageFault *fault)
{
    if (fault != 0) {
        fault->linear_address = linear_address;
        fault->error_code = access_error_bits(state, access) | extra;
    }
    return X86_PAGING_PAGE_FAULT;
}

static int read_entry(const X86PagingMemory *memory, uint64_t address,
                      uint64_t *entry)
{
    return memory->read_u64(memory->opaque, address, entry);
}

static int update_entry(const X86PagingMemory *memory, uint64_t address,
                        uint64_t bits, uint64_t *entry)
{
    return memory->atomic_or_u64(memory->opaque, address, bits, entry);
}

static int is_canonical_48(uint64_t address)
{
    uint64_t upper = address >> 48;
    return (address & (UINT64_C(1) << 47)) ? upper == UINT64_C(0xffff)
                                            : upper == 0;
}

static int cr3_is_valid(const X86PagingState *state, uint64_t address_mask)
{
    uint64_t allowed_low = (state->cr4 & X86_CR4_PCIDE)
                               ? CR3_PCID_MASK
                               : (ENTRY_PWT | ENTRY_PCD);
    uint64_t allowed = address_mask | allowed_low;

    return (state->cr3 & ~allowed) == 0;
}

static int entry_has_reserved_bits(const X86PagingState *state,
                                   uint64_t entry,
                                   uint8_t level,
                                   int large_page,
                                   uint64_t address_mask)
{
    uint64_t reserved_address =
        ((UINT64_C(1) << 52) - 1) & ~address_mask & ~UINT64_C(0xfff);
    uint64_t pkey_enabled = state->cr4 & (X86_CR4_PKE | X86_CR4_PKS);
    uint64_t page_shift = level == 3 ? 30 : 21;
    uint64_t alignment_reserved;

    if ((entry & ENTRY_NX) && !(state->efer & X86_EFER_NXE)) {
        return 1;
    }
    if ((entry & reserved_address) != 0) {
        return 1;
    }
    if ((entry & ENTRY_PKEY_MASK) &&
        !(pkey_enabled && (level == 1 || large_page))) {
        return 1;
    }
    if (level == 4 && (entry & ENTRY_PS)) {
        return 1;
    }
    if (level == 3 && (entry & ENTRY_PS) &&
        !(state->features & X86_PAGING_FEATURE_1G_PAGES)) {
        return 1;
    }
    if (level == 1 || !large_page) {
        return 0;
    }

    /* Bit 12 is the large-page PAT bit; bits 13 through page_shift-1
     * must be zero so that the translated physical base is aligned. */
    alignment_reserved = ((UINT64_C(1) << page_shift) - 1) &
                         ~UINT64_C(0x1fff);
    return (entry & alignment_reserved) != 0;
}

static int protection_key_denies(const X86PagingState *state,
                                 uint64_t leaf,
                                 X86PagingAccess access,
                                 int user_page)
{
    uint32_t rights;
    uint32_t key;
    uint32_t permissions;

    if (access == X86_PAGING_EXECUTE) {
        return 0;
    }

    if (state->cpl == 3 && user_page && (state->cr4 & X86_CR4_PKE)) {
        permissions = state->pkru;
    } else if (state->cpl != 3 && !user_page &&
               (state->cr4 & X86_CR4_PKS)) {
        permissions = state->pkrs;
    } else {
        return 0;
    }

    key = (uint32_t)((leaf & ENTRY_PKEY_MASK) >> 59);
    rights = (permissions >> (key * 2)) & 3;
    return (rights & 1) ||
           (access == X86_PAGING_WRITE && (rights & 2));
}

X86PagingResult x86_64_walk_page_tables(
    const X86PagingState *state,
    const X86PagingMemory *memory,
    uint64_t linear_address,
    X86PagingAccess access,
    X86Translation *translation,
    X86PageFault *fault)
{
    static const uint8_t shifts[4] = { 39, 30, 21, 12 };
    uint64_t address_mask;
    uint64_t table_address;
    uint64_t entry = 0;
    uint64_t leaf_address = 0;
    uint64_t effective = UINT64_MAX;
    uint8_t level;
    int effective_nx = 0;
    int large_page = 0;

    if (state == 0 || memory == 0 || translation == 0 ||
        memory->read_u64 == 0 || memory->atomic_or_u64 == 0 ||
        state->cpl > 3 || state->maxphyaddr < 32 ||
        state->maxphyaddr > 52 || access < X86_PAGING_READ ||
        access > X86_PAGING_EXECUTE) {
        return X86_PAGING_INVALID_STATE;
    }
    if (state->cr4 & X86_CR4_LA57) {
        return X86_PAGING_UNSUPPORTED;
    }
    if (!(state->cr0 & X86_CR0_PG) || !(state->cr4 & X86_CR4_PAE) ||
        !(state->efer & X86_EFER_LMA)) {
        return X86_PAGING_UNSUPPORTED;
    }
    if (!is_canonical_48(linear_address)) {
        if (fault != 0) {
            fault->linear_address = linear_address;
            fault->error_code = 0;
        }
        return X86_PAGING_GENERAL_PROTECTION;
    }

    address_mask = physical_address_mask(state->maxphyaddr);
    if (!cr3_is_valid(state, address_mask)) {
        return X86_PAGING_INVALID_STATE;
    }
    table_address = state->cr3 & address_mask;

    for (level = 4; level != 0; --level) {
        uint64_t index = (linear_address >> shifts[4 - level]) & 0x1ff;
        uint64_t entry_address = table_address + index * sizeof(uint64_t);

        if (read_entry(memory, entry_address, &entry) != 0) {
            return X86_PAGING_MEMORY_ERROR;
        }
        if (!(entry & ENTRY_PRESENT)) {
            return page_fault(state, linear_address, access, 0, fault);
        }

        large_page = (level == 3 || level == 2) && (entry & ENTRY_PS);
        if (entry_has_reserved_bits(state, entry, level, large_page,
                                    address_mask)) {
            return page_fault(state, linear_address, access,
                              access_error_bits(state, access) |
                                  X86_PF_PRESENT | X86_PF_RESERVED,
                              fault);
        }

        if (update_entry(memory, entry_address, ENTRY_ACCESSED, &entry) != 0) {
            return X86_PAGING_MEMORY_ERROR;
        }
        effective &= entry & ~ENTRY_NX;
        effective_nx |= (entry & ENTRY_NX) != 0;

        if (level == 1 || large_page) {
            leaf_address = entry_address;
            break;
        }
        table_address = entry & address_mask;
    }

    {
        int user_page = (effective & ENTRY_USER) != 0;
        int writable = (effective & ENTRY_WRITE) != 0;
        int executable = !effective_nx;
        int user_access = state->cpl == 3;
        int denied = user_access && !user_page;

        if (access == X86_PAGING_WRITE && !writable &&
            (user_access || (state->cr0 & X86_CR0_WP))) {
            denied = 1;
        }
        if (access == X86_PAGING_EXECUTE && !executable) {
            denied = 1;
        }
        if (access == X86_PAGING_EXECUTE && !user_access && user_page &&
            (state->cr4 & X86_CR4_SMEP)) {
            denied = 1;
        }
        if (access != X86_PAGING_EXECUTE && !user_access && user_page &&
            (state->cr4 & X86_CR4_SMAP) &&
            !(state->rflags & RFLAGS_AC)) {
            denied = 1;
        }
        if (protection_key_denies(state, entry, access, user_page)) {
            if (fault != 0) {
                fault->linear_address = linear_address;
                fault->error_code = access_error_bits(state, access) |
                                    X86_PF_PRESENT | X86_PF_PROTECTION_KEY;
            }
            return X86_PAGING_PAGE_FAULT;
        }
        if (denied) {
            return page_fault(state, linear_address, access,
                              X86_PF_PRESENT, fault);
        }
        if (access == X86_PAGING_WRITE) {
            if (update_entry(memory, leaf_address, ENTRY_DIRTY, &entry) != 0) {
                return X86_PAGING_MEMORY_ERROR;
            }
        }

        {
            uint64_t page_shift = large_page ? (level == 3 ? 30 : 21) : 12;
            uint64_t page_size = UINT64_C(1) << page_shift;
            uint64_t page_base = (entry & address_mask) & ~(page_size - 1);

            translation->guest_physical_address =
                page_base | (linear_address & (page_size - 1));
            translation->page_size = page_size;
            translation->leaf_pte = entry;
            translation->writable = (uint8_t)writable;
            translation->user_accessible = (uint8_t)user_page;
            translation->executable = (uint8_t)executable;
            translation->global = (uint8_t)(((state->cr4 & X86_CR4_PGE) != 0) &&
                                            ((entry & ENTRY_GLOBAL) != 0));
            translation->pkey = (uint8_t)((entry & ENTRY_PKEY_MASK) >> 59);
            translation->dirty = (uint8_t)((entry & ENTRY_DIRTY) != 0);
        }
    }

    return X86_PAGING_OK;
}
