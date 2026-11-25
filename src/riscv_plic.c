/* SiFive PLIC v1.0 model. See include/loongemu/riscv_plic.h. */
#include <string.h>
#include "loongemu/riscv_plic.h"

#define PENDING_WORDS ((LOONGEMU_PLIC_MAX_SOURCES + 31) / 32)

static int source_valid(const LoongemuPlic *plic, uint32_t source)
{
    return source >= 1 && source <= plic->num_sources;
}

static void plic_changed(LoongemuPlic *plic)
{
    plic->state_seq++;
}

static void set_pending(LoongemuPlic *plic, uint32_t source, int pending)
{
    uint32_t value = pending ? 1u : 0u;

    if (plic->pending[source] != value) {
        plic->pending[source] = value;
        plic_changed(plic);
    }
}

int loongemu_plic_init(LoongemuPlic *plic, uint32_t num_sources,
                       uint32_t num_contexts)
{
    if (plic == 0 || num_sources == 0 ||
        num_sources > LOONGEMU_PLIC_MAX_SOURCES || num_contexts == 0 ||
        num_contexts > LOONGEMU_PLIC_MAX_CONTEXTS) {
        return 0;
    }
    memset(plic, 0, sizeof(*plic));
    plic->num_sources = num_sources;
    plic->num_contexts = num_contexts;
    plic->state_seq = 1; /* a zero-initialised cache must miss */
    return 1;
}

void loongemu_plic_set_irq(LoongemuPlic *plic, uint32_t source, int level)
{
    if (plic == 0 || !source_valid(plic, source)) {
        return;
    }
    plic->level[source] = level != 0;
    if (!level) {
        /* Deasserting a line clears the gateway's pending bit. */
        set_pending(plic, source, 0);
        return;
    }
    /* A claimed gateway does not re-pend until completion. */
    if (!plic->claimed[source]) {
        set_pending(plic, source, 1);
    }
}

static int plic_scan_pending(const LoongemuPlic *plic, uint32_t context);

int loongemu_plic_pending(LoongemuPlic *plic, uint32_t context)
{
    int level;

    if (plic == 0 || context >= plic->num_contexts) {
        return 0;
    }
    if (plic->pending_cache[context].seq == plic->state_seq) {
        return plic->pending_cache[context].level;
    }
    level = plic_scan_pending(plic, context);
    plic->pending_cache[context].seq = plic->state_seq;
    plic->pending_cache[context].level = level;
    return level;
}

static int plic_scan_pending(const LoongemuPlic *plic, uint32_t context)
{
    uint32_t threshold = plic->threshold[context];

    for (uint32_t source = 1; source <= plic->num_sources; source++) {
        uint32_t bit = source & 31;
        uint32_t word = source >> 5;
        if (!plic->pending[source]) {
            continue;
        }
        if ((plic->enable[context][word] & (1u << bit)) == 0) {
            continue;
        }
        if (plic->priority[source] > threshold) {
            return 1;
        }
    }
    return 0;
}

/* Highest-priority claimable source; ties break to the lowest source id. */
static uint32_t best_claim(const LoongemuPlic *plic, uint32_t context)
{
    uint32_t best = 0;
    uint32_t best_priority = plic->threshold[context];

    for (uint32_t source = 1; source <= plic->num_sources; source++) {
        uint32_t bit = source & 31;
        uint32_t word = source >> 5;
        if (!plic->pending[source]) {
            continue;
        }
        if ((plic->enable[context][word] & (1u << bit)) == 0) {
            continue;
        }
        if (plic->priority[source] > best_priority) {
            best = source;
            best_priority = plic->priority[source];
        }
    }
    return best;
}

static uint32_t claim(LoongemuPlic *plic, uint32_t context)
{
    uint32_t source = best_claim(plic, context);

    if (source == 0) {
        return 0;
    }
    set_pending(plic, source, 0);
    plic->claimed[source] = 1;
    return source;
}

static void complete(LoongemuPlic *plic, uint32_t context, uint32_t source)
{
    (void)context;
    if (!source_valid(plic, source) || !plic->claimed[source]) {
        return;
    }
    plic->claimed[source] = 0;
    /* A still-asserted level line raises a new pending request. */
    if (plic->level[source]) {
        set_pending(plic, source, 1);
    }
}

uint32_t loongemu_plic_read32(LoongemuPlic *plic, uint32_t offset, int *error)
{
    if (error != 0) {
        *error = 0;
    }
    if (plic == 0) {
        if (error != 0) {
            *error = 1;
        }
        return 0;
    }

    if (offset < LOONGEMU_PLIC_PENDING_BASE) {
        uint32_t source = offset / 4;
        if (offset % 4 != 0 || !source_valid(plic, source)) {
            if (error != 0) {
                *error = 1;
            }
            return 0;
        }
        return plic->priority[source];
    }

    if (offset < LOONGEMU_PLIC_ENABLE_BASE) {
        uint32_t index = offset - LOONGEMU_PLIC_PENDING_BASE;
        if (index % 4 != 0 || index / 4 >= PENDING_WORDS) {
            if (error != 0) {
                *error = 1;
            }
            return 0;
        }
        uint32_t word = index / 4;
        uint32_t value = 0;
        for (uint32_t bit = 0; bit < 32; bit++) {
            uint32_t source = word * 32 + bit;
            if (source == 0 || source > plic->num_sources) {
                continue;
            }
            if (plic->pending[source]) {
                value |= 1u << bit;
            }
        }
        return value;
    }

    if (offset < LOONGEMU_PLIC_CONTEXT_BASE) {
        uint32_t index = offset - LOONGEMU_PLIC_ENABLE_BASE;
        uint32_t context = index / LOONGEMU_PLIC_ENABLE_STRIDE;
        uint32_t within = index % LOONGEMU_PLIC_ENABLE_STRIDE;
        if (context >= plic->num_contexts || within % 4 != 0 ||
            within / 4 >= PENDING_WORDS) {
            if (error != 0) {
                *error = 1;
            }
            return 0;
        }
        return plic->enable[context][within / 4];
    }

    {
        uint32_t index = offset - LOONGEMU_PLIC_CONTEXT_BASE;
        uint32_t context = index / LOONGEMU_PLIC_CONTEXT_STRIDE;
        uint32_t within = index % LOONGEMU_PLIC_CONTEXT_STRIDE;
        if (context >= plic->num_contexts) {
            if (error != 0) {
                *error = 1;
            }
            return 0;
        }
        if (within == 0) {
            return plic->threshold[context];
        }
        if (within == 4) {
            return claim(plic, context);
        }
        if (error != 0) {
            *error = 1;
        }
        return 0;
    }
}

void loongemu_plic_write32(LoongemuPlic *plic, uint32_t offset,
                           uint32_t value, int *error)
{
    if (error != 0) {
        *error = 0;
    }
    if (plic == 0) {
        if (error != 0) {
            *error = 1;
        }
        return;
    }

    if (offset < LOONGEMU_PLIC_PENDING_BASE) {
        uint32_t source = offset / 4;
        if (offset % 4 != 0 || !source_valid(plic, source)) {
            if (error != 0) {
                *error = 1;
            }
            return;
        }
        plic->priority[source] = value & LOONGEMU_PLIC_MAX_PRIORITY;
        plic_changed(plic);
        return;
    }

    if (offset < LOONGEMU_PLIC_ENABLE_BASE) {
        /* Pending bits are read-only. */
        if (error != 0) {
            *error = 1;
        }
        return;
    }

    if (offset < LOONGEMU_PLIC_CONTEXT_BASE) {
        uint32_t index = offset - LOONGEMU_PLIC_ENABLE_BASE;
        uint32_t context = index / LOONGEMU_PLIC_ENABLE_STRIDE;
        uint32_t within = index % LOONGEMU_PLIC_ENABLE_STRIDE;
        if (context >= plic->num_contexts || within % 4 != 0 ||
            within / 4 >= PENDING_WORDS) {
            if (error != 0) {
                *error = 1;
            }
            return;
        }
        uint32_t word = within / 4;
        plic->enable[context][word] = value;
        /* Bits for nonexistent sources are hardwired to zero. */
        for (uint32_t bit = 0; bit < 32; bit++) {
            uint32_t source = word * 32 + bit;
            if (source == 0 || source > plic->num_sources) {
                plic->enable[context][word] &= ~(1u << bit);
            }
        }
        plic_changed(plic);
        return;
    }

    {
        uint32_t index = offset - LOONGEMU_PLIC_CONTEXT_BASE;
        uint32_t context = index / LOONGEMU_PLIC_CONTEXT_STRIDE;
        uint32_t within = index % LOONGEMU_PLIC_CONTEXT_STRIDE;
        if (context >= plic->num_contexts) {
            if (error != 0) {
                *error = 1;
            }
            return;
        }
        if (within == 0) {
            plic->threshold[context] = value & LOONGEMU_PLIC_MAX_PRIORITY;
            plic_changed(plic);
            return;
        }
        if (within == 4) {
            complete(plic, context, value);
            return;
        }
        if (error != 0) {
            *error = 1;
        }
    }
}

/* --- MMIO bus attachment --- */

static uint64_t plic_bus_read(void *opaque, uint64_t offset, unsigned size)
{
    int error = 0;

    if (size != 4) {
        return 0; /* the PLIC only defines 32-bit register accesses */
    }
    return loongemu_plic_read32((LoongemuPlic *)opaque, (uint32_t)offset,
                                &error);
}

static void plic_bus_write(void *opaque, uint64_t offset, uint64_t value,
                           unsigned size)
{
    int error = 0;

    if (size != 4) {
        return;
    }
    loongemu_plic_write32((LoongemuPlic *)opaque, (uint32_t)offset,
                          (uint32_t)value, &error);
}

int loongemu_plic_attach(LoongemuMmioBus *bus, LoongemuPlic *plic,
                         uint64_t base)
{
    if (bus == 0 || plic == 0) {
        return 0;
    }
    return loongemu_mmio_bus_add(bus, base, LOONGEMU_PLIC_MMIO_SIZE,
                                 plic_bus_read, plic_bus_write, plic, "plic");
}
