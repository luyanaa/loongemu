#include "latm-llsc.h"

void latm_llsc_init(LatmLlscTable *t)
{
    if (!t) {
        return;
    }
    for (unsigned i = 0; i < LATM_LLSC_MAX_CPUS; i++) {
        t->res[i].pa = 0;
        t->res[i].size = 0;
        t->res[i].valid = 0;
    }
}

void latm_llsc_reserve(LatmLlscTable *t, unsigned cpu, uint64_t pa,
                       unsigned size)
{
    if (!t || cpu >= LATM_LLSC_MAX_CPUS || size == 0) {
        return;
    }
    t->res[cpu].pa = pa;
    t->res[cpu].size = size;
    t->res[cpu].valid = 1;
}

void latm_llsc_clear(LatmLlscTable *t, unsigned cpu)
{
    if (!t || cpu >= LATM_LLSC_MAX_CPUS) {
        return;
    }
    t->res[cpu].valid = 0;
}

void latm_llsc_store(LatmLlscTable *t, unsigned cpu, uint64_t pa, unsigned size)
{
    uint64_t end;

    if (!t || size == 0) {
        return;
    }
    end = pa + size;
    for (unsigned i = 0; i < LATM_LLSC_MAX_CPUS; i++) {
        LatmLlscEntry *r = &t->res[i];

        (void)cpu; /* the issuer's own reservation is dropped as well */
        if (!r->valid) {
            continue;
        }
        if (pa < r->pa + r->size && r->pa < end) {
            r->valid = 0;
        }
    }
}

int latm_llsc_valid(const LatmLlscTable *t, unsigned cpu, uint64_t pa,
                    unsigned size)
{
    if (!t || cpu >= LATM_LLSC_MAX_CPUS) {
        return 0;
    }
    return t->res[cpu].valid && t->res[cpu].pa == pa &&
           t->res[cpu].size == size;
}
