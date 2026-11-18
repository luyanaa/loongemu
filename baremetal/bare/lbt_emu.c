/*
 * Software emulation of the LBT x86 subset for the bare-metal harnesses.
 * See lbt_emu.h.
 */
#include <stdint.h>

#include "lbt_emu.h"

void latx_bare_uart_putc(int c);
void latx_bare_uart_puts(const char *s);

static void puthex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    int sh;

    latx_bare_uart_puts("0x");
    for (sh = 60; sh >= 0; sh -= 4) {
        latx_bare_uart_putc(d[(v >> sh) & 0xf]);
    }
}

SwLbt sw_lbt;


#define EF_CF (1u << 0)
#define EF_PF (1u << 2)
#define EF_AF (1u << 4)
#define EF_ZF (1u << 6)
#define EF_SF (1u << 7)
#define EF_OF (1u << 11)

static uint32_t pack_flags(uint32_t eflags, uint32_t mask)
{
    uint32_t r = 0;

    if (mask & 1u) r |= (eflags & EF_CF);
    if (mask & 2u) r |= (eflags & EF_PF);
    if (mask & 4u) r |= (eflags & EF_AF);
    if (mask & 8u) r |= (eflags & EF_ZF);
    if (mask & 16u) r |= (eflags & EF_SF);
    if (mask & 32u) r |= (eflags & EF_OF);
    return r;
}

static void unpack_flags(uint32_t *eflags, uint64_t v, uint32_t mask)
{
    uint32_t f = *eflags;

    if (mask & 1u)  f = (f & ~EF_CF) | (uint32_t)(v & EF_CF);
    if (mask & 2u)  f = (f & ~EF_PF) | (uint32_t)(v & EF_PF);
    if (mask & 4u)  f = (f & ~EF_AF) | (uint32_t)(v & EF_AF);
    if (mask & 8u)  f = (f & ~EF_ZF) | (uint32_t)(v & EF_ZF);
    if (mask & 16u) f = (f & ~EF_SF) | (uint32_t)(v & EF_SF);
    if (mask & 32u) f = (f & ~EF_OF) | (uint32_t)(v & EF_OF);
    *eflags = f;
}

static int is_lbt_word(uint32_t w)
{
    uint32_t b0 = w & 0xff, b1 = (w >> 8) & 0xff;
    uint32_t b2 = (w >> 16) & 0xff, b3 = (w >> 24) & 0xff;

    if (b3 != 0) return 0;
    if (b2 == 0x5c) return (b1 & 3) == 0;
    if (b2 != 0x00) return 0;
    if (b1 == 0x70) return (b0 & 0x1f) == 0;
    if (b1 == 0x74) return (b0 & 0xe0) == 0;
    if (b1 == 0x80) return b0 == 0x09 || b0 == 0x29 || b0 == 0x08 || b0 == 0x28;
    return 0;
}

extern void lbt_entry(void);

void lbt_handle(uint64_t *regs)
{
    uint64_t era, estat;
    uint32_t w, b0, b1, b2, rd, imm;
    int handled = 0;

    __asm__ volatile("csrrd %0, 0x6" : "=r"(era));
    __asm__ volatile("csrrd %0, 0x5" : "=r"(estat));
    if (((estat >> 16) & 0x3f) != 0x0d) {
        latx_bare_uart_puts("trap is=");
        puthex((estat >> 16) & 0x3f);
        puthex(estat);
        puthex(era);
        latx_bare_uart_puts("\n");
        for (;;) {
        }
    }
    w = *(volatile uint32_t *)(uintptr_t)era;
    b0 = w & 0xff; b1 = (w >> 8) & 0xff; b2 = (w >> 16) & 0xff;
    rd = b0 & 0x1f;
    imm = (b1 >> 2) & 0x3f;

    if (b2 == 0x5c) {
        if ((b0 & 0x20) == 0) { regs[rd] = pack_flags(sw_lbt.eflags, imm); handled = 1; }
        else { unpack_flags(&sw_lbt.eflags, regs[rd], imm); handled = 1; }
    } else if (b2 == 0x00 && b1 == 0x74) { regs[rd] = sw_lbt.top; handled = 1; }
    else if (b2 == 0x00 && b1 == 0x70) { sw_lbt.top = (b0 >> 5) & 7; handled = 1; }
    else if (b2 == 0x00 && b1 == 0x80) {
        if (b0 == 0x09) { sw_lbt.top = (sw_lbt.top + 1) & 7; handled = 1; }
        else if (b0 == 0x29) { sw_lbt.top = (sw_lbt.top - 1) & 7; handled = 1; }
        else if (b0 == 0x08) { sw_lbt.tm = 1; handled = 1; }
        else if (b0 == 0x28) { sw_lbt.tm = 0; handled = 1; }
    }
    if (!handled) {
        latx_bare_uart_puts("lbt unsupported era=");
        puthex(era);
        latx_bare_uart_puts(" w=");
        puthex(w);
        latx_bare_uart_puts("\n");
    }
    __asm__ volatile("csrwr %0, 0x6" :: "r"(era + 4));
}

__asm__(
".text\n.globl lbt_entry\nlbt_entry:\n"
"  addi.d $sp,$sp,-264\n"
"  st.d $ra,$sp,8\n  st.d $tp,$sp,16\n  st.d $r3,$sp,24\n  st.d $r4,$sp,32\n"
"  st.d $r5,$sp,40\n  st.d $r6,$sp,48\n  st.d $r7,$sp,56\n  st.d $r8,$sp,64\n"
"  st.d $r9,$sp,72\n  st.d $r10,$sp,80\n  st.d $r11,$sp,88\n  st.d $r12,$sp,96\n"
"  st.d $r13,$sp,104\n st.d $r14,$sp,112\n st.d $r15,$sp,120\n st.d $r16,$sp,128\n"
"  st.d $r17,$sp,136\n st.d $r18,$sp,144\n st.d $r19,$sp,152\n st.d $r20,$sp,160\n"
"  st.d $r21,$sp,168\n st.d $r22,$sp,176\n st.d $r23,$sp,184\n st.d $r24,$sp,192\n"
"  st.d $r25,$sp,200\n st.d $r26,$sp,208\n st.d $r27,$sp,216\n st.d $r28,$sp,224\n"
"  st.d $r29,$sp,232\n st.d $r30,$sp,240\n st.d $r31,$sp,248\n"
"  move $a0,$sp\n  bl lbt_handle\n"
"  ld.d $ra,$sp,8\n  ld.d $tp,$sp,16\n  ld.d $r3,$sp,24\n  ld.d $r4,$sp,32\n"
"  ld.d $r5,$sp,40\n  ld.d $r6,$sp,48\n  ld.d $r7,$sp,56\n  ld.d $r8,$sp,64\n"
"  ld.d $r9,$sp,72\n  ld.d $r10,$sp,80\n  ld.d $r11,$sp,88\n  ld.d $r12,$sp,96\n"
"  ld.d $r13,$sp,104\n ld.d $r14,$sp,112\n ld.d $r15,$sp,120\n ld.d $r16,$sp,128\n"
"  ld.d $r17,$sp,136\n ld.d $r18,$sp,144\n ld.d $r19,$sp,152\n ld.d $r20,$sp,160\n"
"  ld.d $r21,$sp,168\n ld.d $r22,$sp,176\n ld.d $r23,$sp,184\n ld.d $r24,$sp,192\n"
"  ld.d $r25,$sp,200\n ld.d $r26,$sp,208\n ld.d $r27,$sp,216\n ld.d $r28,$sp,224\n"
"  ld.d $r29,$sp,232\n ld.d $r30,$sp,240\n ld.d $r31,$sp,248\n"
"  addi.d $sp,$sp,264\n  ertn\n"
);


void lbt_emu_install(void)
{
    __asm__ volatile("csrwr %0, 0xc" :: "r"((uint64_t)lbt_entry));
}
