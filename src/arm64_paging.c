/*
 * ARMv8-A Stage-1 walker, 4 KiB granule.
 *
 * Descriptor format (ARM DDI 0487, D8.3): each level is 512 entries of 8 bytes
 * indexed by 9 bits of the VA, from L0 down to L3.
 *
 *   bit  0     valid
 *   bits 1:0   01 = block (L1/L2 only), 11 = table (L0..L2) or page (L3)
 *   bits 11:2  attributes (MAIR index in 4:2, AP in 7:6, SH in 9:8, AF in 10)
 *   bit 10     AF -- an unfollowed access faults even on a valid entry
 *   bits 47:12 output address (block entries carry a size-aligned base)
 *   bits 53:54 PXN / UXN at the block and page levels
 *
 * The walker is deliberately policy-free: it reports what the hardware would
 * do and leaves installing mappings, filling TLBs and raising exceptions to
 * the caller.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "loongemu/arm64_paging.h"

#define ARM64_PTE_VALID   (UINT64_C(1) << 0)
#define ARM64_PTE_TABLE   (UINT64_C(1) << 1)   /* 0b11 with VALID */
#define ARM64_PTE_PAGE    (UINT64_C(1) << 1)
#define ARM64_PTE_AF      (UINT64_C(1) << 10)
#define ARM64_PTE_AP_MASK  (UINT64_C(3) << 6)
#define ARM64_PTE_AP_RW_EL1 (UINT64_C(0) << 6)
#define ARM64_PTE_AP_RW_ALL (UINT64_C(1) << 6)
#define ARM64_PTE_AP_RO_EL1 (UINT64_C(2) << 6)
#define ARM64_PTE_AP_RO_ALL (UINT64_C(3) << 6)
#define ARM64_PTE_PXN     (UINT64_C(1) << 53)   /* privileged execute-never */
#define ARM64_PTE_UXN     (UINT64_C(1) << 54)   /* unprivileged execute-never */
#define ARM64_PTE_ADDR_MASK UINT64_C(0x0000fffffffff000)

/*
 * Table descriptor attributes are accumulated over the walk (ARM DDI 0487
 * D8.3.1.1). Bits 59:63 map to PXNTable, UXNTable, APTable[0], APTable[1],
 * NSTable respectively; the first four propagate as restrictions, with zero
 * meaning no effect.
 */
#define ARM64_TABLE_ATTR_SHIFT 59
#define ARM64_TABLE_ATTR_MASK  UINT64_C(0x1f)
#define ARM64_TABLE_PXN_SHIFT  0   /* descriptor bit 59 */
#define ARM64_TABLE_UXN_SHIFT  1   /* descriptor bit 60 */
#define ARM64_TABLE_AP_SHIFT   2   /* APTable[0], APTable[1] */

#define ARM64_SCTLR_WXN   (UINT64_C(1) << 19)

/* TCR_EL1 fields used here. */
#define TCR_T0SZ_SHIFT  0
#define TCR_EPD0        (UINT64_C(1) << 7)
#define TCR_TG0_SHIFT   14
#define TCR_SH0_SHIFT   12
#define TCR_T1SZ_SHIFT  16
#define TCR_EPD1        (UINT64_C(1) << 23)
#define TCR_HPD0       (UINT64_C(1) << 41)
#define TCR_HPD1       (UINT64_C(1) << 42)
#define TCR_TG1_SHIFT   30
#define TCR_SH1_SHIFT   28

/*
 * TG0 and TG1 encode the granule differently -- this is not a typo in either
 * place (ARM DDI 0487, D17.2.4/D17.2.5):
 *
 *   TG0: 0b00 = 4 KiB, 0b01 = 64 KiB, 0b10 = 16 KiB
 *   TG1: 0b10 = 4 KiB, 0b01 = 16 KiB, 0b11 = 64 KiB
 *
 * TTBR1 has no 0b00, because the value it would encode is what a 52-bit VA
 * uses; treating both halves as "0 means 4 KiB" rejects every TTBR1 mapping.
 */
#define ARM64_TG0_4K 0u
#define ARM64_TG1_4K 2u

/* Returns 0 on success.  A misaligned or unreadable address is a fault. */
static int read_desc(const Arm64PagingMemory *memory, uint64_t pa,
                     uint64_t *value)
{
    if (memory == NULL || memory->read_u64 == NULL || (pa & 7u) != 0) {
        return -1;
    }
    return memory->read_u64(memory->opaque, pa, value);
}

/* Bits of VA consumed per level for a 4 KiB granule: 9, always. */
#define ARM64_VA_BITS_PER_LEVEL 9
#define ARM64_VA_INDEX_MASK     0x1ffULL

uint32_t arm64_fault_status(Arm64WalkResult result, uint32_t access)
{
    /*
     * ESR_EL1: EC 0x25 (data abort, lower EL) occupies bits 31:26, and WnR is
     * bit 6.  DFSC/IFSC name the fault in bits 5:0.
     */
    uint32_t iss;

    switch (result) {
    case ARM64_WALK_OK:
        return 0;
    case ARM64_WALK_NOT_PRESENT:
        iss = 0x04;            /* translation fault, level 0 */
        break;
    case ARM64_WALK_PROTECTION:
        iss = 0x0d;            /* permission fault, level 3 */
        break;
    case ARM64_WALK_RESERVED:
        iss = 0x15;            /* synchronous external abort, level 0 */
        break;
    default:
        return 0;
    }
    if (access & ARM64_ACCESS_WRITE) {
        iss |= (1u << 6);
    }
    return iss;
}

Arm64WalkResult arm64_walk_ex(const Arm64PagingMemory *memory,
                              uint64_t ttbr0, uint64_t ttbr1, uint64_t tcr,
                              uint64_t sctlr, uint64_t va, uint32_t access,
                              Arm64Translation *out)
{
    uint32_t tableattrs = 0;
    uint64_t ttbr, tsz;
    uint32_t tg;
    bool hpd;

    if (out == NULL) {
        return ARM64_WALK_RESERVED;
    }

    /*
     * Bit 55 selects the half, the same bit the hardware uses: TTBR1 covers
     * 0xffff_8000_0000_0000 and above.
     */
    if ((va >> 55) & 1u) {
        if (tcr & TCR_EPD1) {
            return ARM64_WALK_UNSUPPORTED;    /* table walks disabled */
        }
        ttbr = ttbr1;
        hpd = (tcr & TCR_HPD1) != 0;
        tsz = (tcr >> TCR_T1SZ_SHIFT) & 0x3fu;
        tg = (uint32_t)((tcr >> TCR_TG1_SHIFT) & 0x3u);
        if (tg != ARM64_TG1_4K) {
            return ARM64_WALK_UNSUPPORTED;    /* 16 KiB / 64 KiB granules */
        }
    } else {
        if (tcr & TCR_EPD0) {
            return ARM64_WALK_UNSUPPORTED;
        }
        ttbr = ttbr0;
        hpd = (tcr & TCR_HPD0) != 0;
        tsz = (tcr >> TCR_T0SZ_SHIFT) & 0x3fu;
        tg = (uint32_t)((tcr >> TCR_TG0_SHIFT) & 0x3u);
        if (tg != ARM64_TG0_4K) {
            return ARM64_WALK_UNSUPPORTED;    /* 16 KiB / 64 KiB granules */
        }
    }
    (void)tg;

    /*
     * The number of levels follows from TnSZ: level 0 walks 48-TnSZ bits, and
     * each level below consumes 9.  A 4 KiB granule with 48-bit VA is the
     * four-level case (L0..L3), which is what this walker implements.
     */
    {
        uint32_t va_bits = 64u - (uint32_t)tsz;

        if (va_bits > 48u || va_bits <= 12u) {
            return ARM64_WALK_RESERVED;
        }

        {
            uint64_t table = ttbr & ARM64_PTE_ADDR_MASK;
            uint32_t levels = (va_bits - 12u) / ARM64_VA_BITS_PER_LEVEL;

            tableattrs = 0;
            int shift = (int)va_bits;

            for (uint32_t lvl = 0; lvl < levels; lvl++) {
                uint32_t index;
                uint64_t desc;

                shift -= ARM64_VA_BITS_PER_LEVEL;
                index = (uint32_t)((va >> (uint32_t)shift) &
                                   ARM64_VA_INDEX_MASK);
                if (index > 511u) {
                    return ARM64_WALK_RESERVED;
                }
                if (read_desc(memory, table + (uint64_t)index * 8u,
                              &desc) != 0) {
                    return ARM64_WALK_NOT_PRESENT;
                }
                if (!(desc & ARM64_PTE_VALID)) {
                    return ARM64_WALK_NOT_PRESENT;
                }

                {
                    uint32_t kind = (uint32_t)(desc & 0x3u);
                    uint32_t leaf_bits = 0;

                    if (kind == 1u) {
                        /* Block: legal at L1 (1 GiB) and L2 (2 MiB). */
                        if (lvl == 0u || lvl == levels - 1u) {
                            return ARM64_WALK_RESERVED;
                        }
                        /*
                         * A block at this level covers the bits below `shift`,
                         * so its size is exactly 2^shift -- 30 at L1 (1 GiB)
                         * and 21 at L2 (2 MiB).  Adding a level's worth of
                         * index bits would report a size that does not exist.
                         */
                        leaf_bits = (uint32_t)shift;
                    } else if (kind == 3u) {
                        if (lvl == levels - 1u) {
                            leaf_bits = 12u;              /* 4 KiB page */
                        } else {
                            /*
                             * A table: gather the attributes that propagate
                             * downward unless HPD disables hierarchical
                             * permissions for this TTBR range.
                             */
                            if (!hpd) {
                                tableattrs |= (uint32_t)
                                    ((desc >> ARM64_TABLE_ATTR_SHIFT) &
                                     ARM64_TABLE_ATTR_MASK);
                            }
                            table = desc & ARM64_PTE_ADDR_MASK;
                            continue;
                        }
                    } else {
                        return ARM64_WALK_RESERVED;       /* reserved 0b10 */
                    }

                    /* Leaf reached: decide permissions. */
                    {
                        uint32_t perm = 0;
                        uint32_t ap = (uint32_t)((desc & ARM64_PTE_AP_MASK) >> 6);
                        bool user = (access & ARM64_ACCESS_USER) != 0;
                        bool el0_readable, el0_writable;
                        bool el1_writable;

                        /*
                         * AP[2:1] is the Stage-1 permission matrix (Arm DDI
                         * 0487 D8.4.1):
                         *
                         *   AP   EL1   EL0
                         *   00   RW    none
                         *   01   RW    RW
                         *   10   RO    none
                         *   11   RO    RO
                         *
                         * APTable is hierarchical (D8.4.1.2.2). QEMU's
                         * target/arm/ptw.c folds accumulated APTable[0] into
                         * AP[1] by clearing it, and APTable[1] into AP[2] by
                         * setting it; apply that fold before this matrix.
                         */
                        {
                            uint32_t apt = (tableattrs >> ARM64_TABLE_AP_SHIFT) &
                                           0x3u;

                            if (apt & 1u) {
                                ap &= ~1u;      /* APTable[0]=1 -> AP[1]=0 */
                            }
                            if (apt & 2u) {
                                ap |= 2u;       /* APTable[1]=1 -> AP[2]=1 */
                            }
                        }

                        el0_readable = (ap == 1u || ap == 3u);
                        el0_writable = (ap == 1u);
                        el1_writable = (ap == 0u || ap == 1u);

                        if (user) {
                            if (el0_readable) {
                                perm |= ARM64_ACCESS_READ;
                            }
                            if (el0_writable) {
                                perm |= ARM64_ACCESS_WRITE;
                            }
                        } else {
                            perm |= ARM64_ACCESS_READ;
                            if (el1_writable) {
                                perm |= ARM64_ACCESS_WRITE;
                            }
                        }

                        if ((access & ARM64_ACCESS_READ) &&
                            !(perm & ARM64_ACCESS_READ)) {
                            return ARM64_WALK_PROTECTION;
                        }
                        if ((access & ARM64_ACCESS_WRITE) &&
                            !(perm & ARM64_ACCESS_WRITE)) {
                            return ARM64_WALK_PROTECTION;
                        }

                        {
                            bool xn = user ? (desc & ARM64_PTE_UXN) != 0
                                           : (desc & ARM64_PTE_PXN) != 0;
                            bool table_xn =
                                (tableattrs >> (user ? ARM64_TABLE_UXN_SHIFT :
                                                       ARM64_TABLE_PXN_SHIFT)) & 1u;
                            bool writable = user ? el0_writable : el1_writable;

                            /*
                             * In the EL1&0 regime, UnprivWrite removes
                             * PrivExecute even with WXN clear (DDI 0487
                             * D8.4.1.2.3). WXN separately removes execute
                             * permission for the current privilege when its
                             * data-write permission is present (D8.4.1.2.4).
                             */
                            if (!user && el0_writable) {
                                xn = true;
                            }
                            xn = xn || table_xn ||
                                 ((sctlr & ARM64_SCTLR_WXN) && writable);
                            if (!xn) {
                                perm |= ARM64_ACCESS_EXEC;
                            }
                        }
                        if ((access & ARM64_ACCESS_EXEC) &&
                            !(perm & ARM64_ACCESS_EXEC)) {
                            return ARM64_WALK_PROTECTION;
                        }
                        if (!(desc & ARM64_PTE_AF)) {
                            /*
                             * Access flag clear: the hardware faults on the
                             * first access even though the entry is valid, and
                             * software is expected to set AF and continue.
                             */
                            return ARM64_WALK_NOT_PRESENT;
                        }

                        out->pa = (desc & ARM64_PTE_ADDR_MASK) |
                                  (va & ((UINT64_C(1) << leaf_bits) - 1u));
                        out->desc = desc;
                        out->page_bits = leaf_bits;
                        out->perm = perm;
                        return ARM64_WALK_OK;
                    }
                }
            }
            return ARM64_WALK_NOT_PRESENT;            /* ran out of levels */
        }
    }
}

Arm64WalkResult arm64_walk(const Arm64PagingMemory *memory,
                           uint64_t ttbr0, uint64_t ttbr1, uint64_t tcr,
                           uint64_t va, uint32_t access,
                           Arm64Translation *out)
{
    /* Legacy API has no SCTLR input; treat WXN as disabled. */
    return arm64_walk_ex(memory, ttbr0, ttbr1, tcr, 0, va, access, out);
}
