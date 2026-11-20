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
#define EF_ALL (EF_CF | EF_PF | EF_AF | EF_ZF | EF_SF | EF_OF)

static uint32_t pack_flags(uint32_t eflags, uint32_t mask)
{
    uint32_t r = 0;

    if (mask & 1u)  r |= (eflags & EF_CF);
    if (mask & 2u)  r |= (eflags & EF_PF);
    if (mask & 4u)  r |= (eflags & EF_AF);
    if (mask & 8u)  r |= (eflags & EF_ZF);
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

static uint64_t zmask(unsigned w)
{
    return w == 64 ? UINT64_MAX : ((UINT64_C(1) << w) - 1);
}

static int64_t sext(uint64_t v, unsigned w)
{
    uint64_t sign;

    if (w == 64) {
        return (int64_t)v;
    }
    sign = UINT64_C(1) << (w - 1);
    v &= zmask(w);
    return (int64_t)((v ^ sign) - sign);
}

static inline uint32_t parity_flag(uint64_t r)
{
    uint32_t p = (uint32_t)r & 0xffu;
    p ^= p >> 4;
    return ((0x6996u >> (p & 0xfu)) & 1u) ? 0u : EF_PF;
}

static inline void set_szp_flags(unsigned w, uint64_t r, uint32_t extra)
{
    uint32_t f = extra & (EF_CF | EF_AF | EF_OF);

    if (r == 0) {
        f |= EF_ZF;
    }
    if (r & (UINT64_C(1) << (w - 1))) {
        f |= EF_SF;
    }
    sw_lbt.eflags = f | parity_flag(r);
}

static void lbt_add_adc(unsigned w, uint64_t a, uint64_t b, int with_carry,
                        int unsigned_form)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    uint64_t c = (with_carry && (sw_lbt.eflags & EF_CF)) ? 1u : 0u;
    uint64_t sum = x + y;
    uint64_t r = (sum + c) & m;
    uint32_t f = 0;

    if (w == 64) {
        if (sum < x || (c && r == 0)) {
            f |= EF_CF;
        }
    } else if (((sum + c) >> w) != 0) {
        f |= EF_CF;
    }
    if ((x ^ y ^ r) & 0x10) {
        f |= EF_AF;
    }
    if (!unsigned_form &&
        (~(x ^ y) & (x ^ r) & (UINT64_C(1) << (w - 1)))) {
        f |= EF_OF;
    }
    set_szp_flags(w, r, f);
}

static void lbt_sub_sbc(unsigned w, uint64_t a, uint64_t b, int with_borrow,
                        int unsigned_form)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    uint64_t c = (with_borrow && (sw_lbt.eflags & EF_CF)) ? 1u : 0u;
    uint64_t r = (x - y - c) & m;
    uint32_t f = 0;

    if (x < y || (c && x == y)) {
        f |= EF_CF;
    }
    if ((x ^ y ^ r) & 0x10) {
        f |= EF_AF;
    }
    if (!unsigned_form &&
        ((x ^ y) & (x ^ r) & (UINT64_C(1) << (w - 1)))) {
        f |= EF_OF;
    }
    set_szp_flags(w, r, f);
}

static void lbt_logic(unsigned w, uint64_t a, uint64_t b, int which)
{
    uint64_t m = zmask(w), x = a & m, y = b & m;
    uint64_t r = which == 0 ? (x & y) : which == 1 ? (x | y) : (x ^ y);

    set_szp_flags(w, r, 0);
}

static void lbt_incdec(unsigned w, uint64_t a, int dec)
{
    uint64_t m = zmask(w), x = a & m;
    uint64_t r = (dec ? (x - 1) : (x + 1)) & m;
    uint32_t saved_cf = sw_lbt.eflags & EF_CF;
    uint32_t f = 0;

    if ((x ^ 1u ^ r) & 0x10) {
        f |= EF_AF;
    }
    if (dec) {
        if (x == (UINT64_C(1) << (w - 1))) {
            f |= EF_OF;
        }
    } else if (x == (m >> 1)) {
        f |= EF_OF;
    }
    set_szp_flags(w, r, f);
    sw_lbt.eflags |= saved_cf;
}

static void lbt_mul(unsigned w, uint64_t a, uint64_t b, int is_signed)
{
    uint64_t m = zmask(w), x = a & m, y = b & m, r;
    uint32_t overflow;

    if (is_signed) {
        int64_t xs = sext(x, w), ys = sext(y, w);
        __int128 sp = (__int128)xs * ys;
        r = (uint64_t)sp & m;
        overflow = ((__int128)sext(r, w) != sp);
    } else {
        unsigned __int128 up = (unsigned __int128)x * y;
        r = (uint64_t)up & m;
        overflow = ((up >> w) != 0);
    }
    sw_lbt.eflags = overflow ? (EF_CF | EF_OF) : 0;
}

static void lbt_shift(unsigned w, uint64_t a, uint64_t b, int kind)
{
    /* kind: 0 = sll, 1 = srl, 2 = sra */
    uint64_t m = zmask(w), x = a & m;
    unsigned c = (unsigned)(b & (w == 64 ? 0x3f : 0x1f));
    uint64_t r;
    uint32_t cf;

    if (c == 0) {
        return;
    }
    if (kind == 0) {
        cf = (c <= w) ? (uint32_t)((x >> (w - c)) & 1) : 0;
        r = (c >= w) ? 0 : ((x << c) & m);
    } else if (kind == 1) {
        cf = (c <= w) ? (uint32_t)((x >> (c - 1)) & 1) : 0;
        r = (c >= w) ? 0 : (x >> c);
    } else {
        int64_t s = sext(x, w);
        unsigned sh = c >= w ? w - 1 : c;
        cf = (c > w) ? (uint32_t)((s < 0) ? 1 : 0)
                     : (uint32_t)((x >> (c - 1)) & 1);
        r = (uint64_t)(s >> sh) & m;
    }
    set_szp_flags(w, r, cf ? EF_CF : 0);
    if (c == 1 && kind != 2 && ((x ^ r) & (UINT64_C(1) << (w - 1)))) {
        sw_lbt.eflags |= EF_OF;
    }
}

static void lbt_rot(unsigned w, uint64_t a, uint64_t b, int left,
                    int through_carry)
{
    uint64_t m = zmask(w), x = a & m;
    unsigned ring = through_carry ? w + 1 : w;
    unsigned c = (unsigned)(b & (w == 64 ? 0x3f : 0x1f));
    unsigned n, bit;
    uint64_t r;

    if (c == 0) {
        return;
    }
    if (through_carry) {
        unsigned __int128 wide;
        unsigned cf = (sw_lbt.eflags & EF_CF) ? 1u : 0u;

        n = c % ring;
        if (n == 0) {
            return;
        }
        wide = ((unsigned __int128)cf << w) | x;
        if (left) {
            wide = ((wide << n) | (wide >> (ring - n))) &
                   ((((unsigned __int128)1) << ring) - 1);
        } else {
            wide = ((wide >> n) | (wide << (ring - n))) &
                   ((((unsigned __int128)1) << ring) - 1);
        }
        bit = (unsigned)((wide >> w) & 1);
        r = (uint64_t)wide & m;
    } else {
        n = c % w;
        if (n == 0) {
            return;
        }
        if (left) {
            r = ((x << n) | (x >> (w - n))) & m;
            bit = (unsigned)((x >> (w - n)) & 1);
        } else {
            r = ((x >> n) | (x << (w - n))) & m;
            bit = (unsigned)((x >> (n - 1)) & 1);
        }
    }
    sw_lbt.eflags &= ~(EF_CF | EF_OF);
    if (bit) {
        sw_lbt.eflags |= EF_CF;
    }
    if (c == 1 && ((x ^ r) & (UINT64_C(1) << (w - 1)))) {
        sw_lbt.eflags |= EF_OF;
    }
}

static uint64_t eval_setx86j(uint32_t f, unsigned cond)
{
    int cf = !!(f & EF_CF), pf = !!(f & EF_PF);
    int zf = !!(f & EF_ZF), sf = !!(f & EF_SF);
    int of = !!(f & EF_OF);

    switch (cond & 0xf) {
    case 0:  return !cf && !zf;              /* A  */
    case 1:  return !cf;                     /* AE */
    case 2:  return cf;                      /* B  */
    case 3:  return cf || zf;                /* BE */
    case 4:  return zf;                      /* E  */
    case 5:  return !zf;                     /* NE */
    case 6:  return !zf && (sf == of);       /* G  */
    case 7:  return (sf == of);              /* GE */
    case 8:  return (sf != of);              /* L  */
    case 9:  return zf || (sf != of);        /* LE */
    case 10: return sf;                      /* S  */
    case 11: return !sf;                     /* NS */
    case 12: return of;                      /* O  */
    case 13: return !of;                     /* NO */
    case 14: return pf;                      /* P  */
    default: return !pf;                     /* NP */
    }
}

extern void lbt_entry(void);

void lbt_handle_fast(uint64_t *regs, uint64_t era)
{
    uint32_t w = *(const volatile uint32_t *)(uintptr_t)era;
    uint32_t rd = w & 0x1fu;
    uint32_t rj = (w >> 5) & 0x1fu;
    uint32_t rk = (w >> 10) & 0x1fu;
    uint64_t next_era = era + 4;
    int handled = 1;

    regs[0] = 0;

    /* Hot-path dispatch ordered by dynamic frequency in Linux kernel TBs. */
    if ((w & 0xffff8000u) == 0x003f0000u) {
        /* x86add/sub/adc/sbc/sll/srl/sra rj, rk */
        uint32_t sub = w & 0x1fu;
        unsigned width = 8u << (sub & 3u);

        if (sub >= 0x08u && sub <= 0x0bu) {
            lbt_sub_sbc(width, regs[rj], regs[rk], 0, 0);
        } else if (sub >= 0x04u && sub <= 0x07u) {
            lbt_add_adc(width, regs[rj], regs[rk], 0, 0);
        } else if (sub == 0x01u) {
            lbt_add_adc(64, regs[rj], regs[rk], 0, 1);
        } else if (sub == 0x03u) {
            lbt_sub_sbc(64, regs[rj], regs[rk], 0, 1);
        } else if (sub == 0x00u) {
            lbt_add_adc(32, regs[rj], regs[rk], 0, 1);
        } else if (sub == 0x02u) {
            lbt_sub_sbc(32, regs[rj], regs[rk], 0, 1);
        } else if (sub >= 0x0cu && sub <= 0x0fu) {
            lbt_add_adc(width, regs[rj], regs[rk], 1, 0);
        } else if (sub >= 0x10u && sub <= 0x13u) {
            lbt_sub_sbc(width, regs[rj], regs[rk], 1, 0);
        } else if (sub >= 0x14u && sub <= 0x17u) {
            lbt_shift(width, regs[rj], regs[rk], 0);
        } else if (sub >= 0x18u && sub <= 0x1bu) {
            lbt_shift(width, regs[rj], regs[rk], 1);
        } else {
            lbt_shift(width, regs[rj], regs[rk], 2);
        }
    } else if ((w & 0xffff8000u) == 0x003f8000u) {
        /* x86and/or/xor/rotr/rotl/rcr/rcl rj, rk */
        uint32_t sub = w & 0x1fu;
        unsigned width = 8u << (sub & 3u);

        if (sub >= 0x10u && sub <= 0x13u) {
            lbt_logic(width, regs[rj], regs[rk], 0);
        } else if (sub >= 0x14u && sub <= 0x17u) {
            lbt_logic(width, regs[rj], regs[rk], 1);
        } else if (sub >= 0x18u && sub <= 0x1bu) {
            lbt_logic(width, regs[rj], regs[rk], 2);
        } else if (sub <= 0x03u) {
            if (sub == 0x02u) width = 64;
            else if (sub == 0x03u) width = 32;
            lbt_rot(width, regs[rj], regs[rk], 0, 0);
        } else if (sub >= 0x04u && sub <= 0x07u) {
            lbt_rot(width, regs[rj], regs[rk], 1, 0);
        } else if (sub >= 0x08u && sub <= 0x0bu) {
            lbt_rot(width, regs[rj], regs[rk], 0, 1);
        } else if (sub >= 0x0cu && sub <= 0x0fu) {
            lbt_rot(width, regs[rj], regs[rk], 1, 1);
        } else {
            handled = 0;
        }
    } else if ((w & 0xffffc000u) == 0x00368000u) {
        /* setx86j rd, condh */
        regs[rd] = eval_setx86j(sw_lbt.eflags, (w >> 10) & 0xfu);
    } else if ((w & 0xfffc03c0u) == 0x005c0000u) {
        /* x86mfflag / x86mtflag rd, imm_ui8 */
        uint32_t imm = (w >> 10) & 0x3fu;
        if ((w & 0x20u) == 0) {
            regs[rd] = pack_flags(sw_lbt.eflags, imm);
        } else {
            unpack_flags(&sw_lbt.eflags, regs[rd], imm);
        }
    } else if ((w & 0xfffffc18u) == 0x00008000u) {
        /* x86inc.{b,h,w,d} / x86dec.{b,h,w,d} rj */
        lbt_incdec(8u << (w & 3u), regs[rj], (int)((w >> 2) & 1u));
    } else if ((w & 0xfffe0000u) == 0x00540000u) {
        /* x86slli/srli/srai/rotri/rcri/rotli/rcli.{b,h,w,d} rj, imm */
        uint32_t sub = w & 0x1fu;
        uint32_t kind = sub >> 2;
        unsigned width = 8u << (sub & 3u);
        uint32_t imm_mask = (width == 8) ? 0x7u :
                            (width == 16) ? 0xfu :
                            (width == 32) ? 0x1fu : 0x3fu;
        uint32_t imm = (w >> 10) & imm_mask;

        if (kind <= 2u) {
            lbt_shift(width, regs[rj], imm, (int)kind);
        } else if (kind == 3u) {
            lbt_rot(width, regs[rj], imm, 0, 0);
        } else if (kind == 4u) {
            lbt_rot(width, regs[rj], imm, 0, 1);
        } else if (kind == 5u) {
            lbt_rot(width, regs[rj], imm, 1, 0);
        } else if (kind == 6u) {
            lbt_rot(width, regs[rj], imm, 1, 1);
        } else {
            handled = 0;
        }
    } else if ((w & 0xfffffc00u) == 0x00000800u) {
        /* gr2scr sd, rj */
        sw_lbt.scr[w & 3u] = regs[rj];
    } else if ((w & 0xfffffc00u) == 0x00000c00u) {
        /* scr2gr rd, sj */
        regs[rd] = sw_lbt.scr[(w >> 5) & 3u];
    } else if ((w & 0xfc0003e0u) == 0x48000200u) {
        /* jiscr0 imm_offl: jump to scr[0] + sign_ext(off21 << 2) */
        uint32_t off21 = ((w & 0x1fu) << 16) | ((w >> 10) & 0xffffu);
        int64_t disp = ((int64_t)(int32_t)(off21 << 11) >> 11) << 2;
        next_era = sw_lbt.scr[0] + (uint64_t)disp;
    } else if ((w & 0xfc0003e0u) == 0x48000300u) {
        /* jiscr1 imm_offl: scr[0] = era; jump to scr[1] + sign_ext(off21 << 2) */
        uint32_t off21 = ((w & 0x1fu) << 16) | ((w >> 10) & 0xffffu);
        int64_t disp = ((int64_t)(int32_t)(off21 << 11) >> 11) << 2;
        sw_lbt.scr[0] = era;
        next_era = sw_lbt.scr[1] + (uint64_t)disp;
    } else if ((w & 0xfffffc1fu) == 0x00007000u) {
        /* x86mttop imm_ptr */
        sw_lbt.top = (w >> 5) & 7u;
    } else if ((w & 0xfffffc00u) == 0x00007400u) {
        /* x86mftop rd */
        regs[rd] = sw_lbt.top;
    } else if ((w & 0xfffffc00u) == 0x00007800u) {
        /* x86loope rd, rj */
        regs[rd] = (regs[rj] != 0 && (sw_lbt.eflags & EF_ZF)) ? 1u : 0u;
    } else if ((w & 0xfffffc00u) == 0x00007c00u) {
        /* x86loopne rd, rj */
        regs[rd] = (regs[rj] != 0 && !(sw_lbt.eflags & EF_ZF)) ? 1u : 0u;
    } else if (w == 0x00008008u) {
        sw_lbt.tm = 1;
    } else if (w == 0x00008028u) {
        sw_lbt.tm = 0;
    } else if (w == 0x00008009u) {
        sw_lbt.top = (sw_lbt.top + 1u) & 7u;
    } else if (w == 0x00008029u) {
        sw_lbt.top = (sw_lbt.top - 1u) & 7u;
    } else if ((w & 0xfffc0000u) == 0x00300000u) {
        /* adc.{b,h,w,d} (bit17=0) / sbc.{b,h,w,d} (bit17=1) rd, rj, rk */
        int is_sub = (int)((w >> 17) & 1u);
        unsigned width = 8u << ((w >> 15) & 3u);
        uint64_t cf = (sw_lbt.eflags & EF_CF) ? 1u : 0u;
        uint64_t r = is_sub ? (regs[rj] - regs[rk] - cf)
                            : (regs[rj] + regs[rk] + cf);
        regs[rd] = (uint64_t)sext(r, width);
    } else if ((w & 0xffff8018u) == 0x003e8000u) {
        /* x86mul.{b,h,w,d,bu,hu,wu,du} rj, rk */
        lbt_mul(8u << (w & 3u), regs[rj], regs[rk], ((w & 4u) == 0));
    } else if ((w & 0xffffe000u) == 0x004c2000u) {
        /* rotri.b rd, rj, ui3 */
        unsigned sh = (w >> 10) & 7u;
        uint8_t v = (uint8_t)regs[rj];
        uint8_t r = sh ? (uint8_t)((v >> sh) | (v << (8u - sh))) : v;
        regs[rd] = (uint64_t)(int64_t)(int8_t)r;
    } else if ((w & 0xffffc000u) == 0x004c4000u) {
        /* rotri.h rd, rj, ui4 */
        unsigned sh = (w >> 10) & 0xfu;
        uint16_t v = (uint16_t)regs[rj];
        uint16_t r = sh ? (uint16_t)((v >> sh) | (v << (16u - sh))) : v;
        regs[rd] = (uint64_t)(int64_t)(int16_t)r;
    } else {
        handled = 0;
    }

    regs[0] = 0;
    if (!handled) {
        latx_bare_uart_puts("lbt unsupported era=");
        puthex(era);
        latx_bare_uart_puts(" w=");
        puthex(w);
        latx_bare_uart_puts("\n");
    }
    __asm__ volatile("csrwr %0, 0x6" : "+r"(next_era) :: "memory");
}

void lbt_handle(uint64_t *regs)
{
    uint64_t era, estat;

    __asm__ volatile("csrrd %0, 0x6" : "=r"(era));
    __asm__ volatile("csrrd %0, 0x5" : "=r"(estat));
    if (((estat >> 16) & 0x3f) != 0x0d) {
        uint64_t badv;

        __asm__ volatile("csrrd %0, 0x7" : "=r"(badv));
        latx_bare_uart_puts("trap ecode=");
        puthex((estat >> 16) & 0x3f);
        latx_bare_uart_puts(" estat=");
        puthex(estat);
        latx_bare_uart_puts(" era=");
        puthex(era);
        latx_bare_uart_puts(" badv=");
        puthex(badv);
        latx_bare_uart_puts(" insn=");
        puthex(*(const volatile uint32_t *)(uintptr_t)era);
        latx_bare_uart_puts("\n");
        for (;;) {
        }
    }
    lbt_handle_fast(regs, era);
}

__asm__(
".text\n.globl lbt_entry\nlbt_entry:\n"
"  addi.d $sp,$sp,-264\n"
"  st.d $zero,$sp,0\n"
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
    uint64_t entry = (uint64_t)(uintptr_t)lbt_entry;
    __asm__ volatile("csrwr %0, 0xc" : "+r"(entry) :: "memory");
}
