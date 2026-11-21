#include "loongemu/hyperv.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAST (UINT64_C(1) << 16)
#define REP(n) ((uint64_t)(n) << 32)
#define VAR(n) ((uint64_t)(n) << 17)

struct State {
    uint64_t memory[512], xmm[12];
    uint64_t mask, address, flags;
    unsigned flushes, ipis, vector;
};
static int read_guest(void *p, uint64_t gpa, uint64_t *out)
{
    struct State *s = p;
    if (gpa < 0x1000u || gpa > 0x1ff8u || (gpa & 7u)) return 0;
    *out = s->memory[(gpa - 0x1000u) / 8u];
    return 1;
}
static uint64_t read_xmm(void *p, unsigned n) { return ((struct State *)p)->xmm[n]; }
static void flush(void *p, uint64_t mask, uint64_t address, uint64_t flags)
{
    struct State *s = p;
    s->mask = mask; s->address = address; s->flags = flags; s->flushes++;
}
static void ipi(void *p, uint64_t mask, uint8_t vector)
{
    struct State *s = p;
    s->mask = mask; s->vector = vector; s->ipis++;
}
int main(void)
{
    struct State s = {0};
    LoongemuHypervOps ops = {&s, 4u, read_guest, read_xmm, flush, ipi};
    uint64_t r;
    assert(loongemu_hyperv_call(&ops, 0xffff, 0, 0) == 2);
    assert(loongemu_hyperv_call(&ops, UINT64_C(1) << 63 | 2, 0, 0) == 3);
    assert(loongemu_hyperv_call(&ops, 2, 0x1001, 0) == 4);
    assert(loongemu_hyperv_call(&ops, 2, 0x2000, 0) == 3);
    assert(loongemu_hyperv_call(&ops, 2, 0x1ff0, 0) == 4);
    s.memory[0] = 0x7000; s.memory[1] = 0; s.memory[2] = 0xa;
    assert(loongemu_hyperv_call(&ops, 2, 0x1000, 0) == 0);
    assert(s.flushes == 1 && s.mask == 0xa && s.address == 0x7000);
    s.memory[1] = 1; s.memory[2] = 0;
    assert(loongemu_hyperv_call(&ops, 2, 0x1000, 0) == 0 && s.mask == 0xf);
    s.memory[1] = 2; s.memory[2] = 1;
    assert(loongemu_hyperv_call(&ops, 2, 0x1000, 0) == 0 && s.mask == 1);
    s.memory[1] = 8;
    assert(loongemu_hyperv_call(&ops, 2, 0x1000, 0) == 5);
    s.memory[1] = 0; s.memory[2] = 0;
    assert(loongemu_hyperv_call(&ops, 2, 0x1000, 0) == 5);
    s.xmm[0] = 5;
    assert(loongemu_hyperv_call(&ops, 2 | FAST, 0x8000, 0) == 0);
    assert(s.address == 0x8000 && s.mask == 5);
    s.memory[0] = 0x9000; s.memory[1] = 0;
    s.memory[2] = 0; s.memory[3] = 1; s.memory[4] = 3;
    assert(loongemu_hyperv_call(&ops, 0x13 | VAR(1), 0x1000, 0) == 0 && s.mask == 3);
    s.memory[3] = 5;
    assert(loongemu_hyperv_call(&ops, 0x13 | VAR(2), 0x1000, 0) == 3);
    s.memory[2] = 1; s.memory[3] = 0;
    assert(loongemu_hyperv_call(&ops, 0x13, 0x1000, 0) == 0 && s.mask == 0xf);
    s.memory[3] = 1; s.memory[4] = 0;
    assert(loongemu_hyperv_call(&ops, 0x13 | VAR(1), 0x1000, 0) == 0 && s.mask == 0xf);
    assert(loongemu_hyperv_call(&ops, 0x13, 0x1000, 0) == 3);
    s.memory[2] = 0; s.memory[3] = 2; s.memory[4] = 0;
    assert(loongemu_hyperv_call(&ops, 0x13 | VAR(1), 0x1000, 0) == 3);
    s.memory[0] = 0; s.memory[1] = 0; s.memory[2] = 1;
    s.memory[3] = 0x1000; s.memory[4] = 0x2000;
    r = loongemu_hyperv_call(&ops, 3 | REP(2), 0x1000, 0);
    assert(r == REP(2) && s.mask == 1);
    assert(loongemu_hyperv_call(&ops, 3 | REP(2) | (UINT64_C(1) << 48),
                                0x1000, 0) == REP(2));
    assert(loongemu_hyperv_call(&ops, 3 | REP(2) | (UINT64_C(2) << 48),
                                0x1000, 0) == 3);
    s.memory[1] = 4;
    assert(loongemu_hyperv_call(&ops, 3 | REP(2), 0x1000, 0) == 5);
    s.memory[1] = 0;
    assert(loongemu_hyperv_call(&ops, 3, 0x1000, 0) == 3);
    assert(loongemu_hyperv_call(&ops, 0xb | FAST, 0x0f, 1) == 3);
    assert(loongemu_hyperv_call(&ops, 0xb | FAST, 0x100, 1) == 3);
    assert(loongemu_hyperv_call(&ops, 0xb | FAST, UINT64_C(0x11) << 32 | 0x61, 1) == 3);
    assert(loongemu_hyperv_call(&ops, 0xb | FAST, 0x10, 0x12) == 0);
    assert(s.vector == 0x10 && s.mask == 2);
    s.memory[0] = 0x62; s.memory[1] = UINT64_MAX;
    assert(loongemu_hyperv_call(&ops, 0xb, 0x1000, 0) == 0);
    assert(s.vector == 0x62 && s.mask == 0xf);
    assert(s.ipis == 2);
    assert(loongemu_hyperv_call(&ops, 0xb, 0x1ff8, 0) == 4);
    assert(s.ipis == 2);
    puts("Hyper-V ABI tests passed");
    return 0;
}
