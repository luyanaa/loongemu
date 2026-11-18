/* Freestanding EFI self-test: proves the LoongBT *fallback* back end runs on a
 * LoongArch core without the LBT extension (the QEMU la464 model has none), and
 * that the produced image contains no x86 or setx86 LBT instructions. */
#include "loongemu/loongbt.h"
#include "loongemu/loongbt_ir2_opcodes.h"

#define UART0_THR ((volatile unsigned char *)0x1fe001e0)

static void putc_(char c)
{
    *UART0_THR = (unsigned char)c;
}

static void puts_(const char *s)
{
    while (*s) {
        putc_(*s++);
    }
}

static void puthex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    puts_("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        putc_(digits[(v >> shift) & 0xf]);
    }
}

static int checks;
static int fails;
static LoongbtState st;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        puts_("FAIL ");
        puts_(what);
        puts_("\n");
    }
}

static void reset(void)
{
    st.eflags = 0;
    st.top = 0;
    st.tm = false;
}

static void run_semantics(void)
{
    uint64_t r;

    reset();
    r = loongbt_exec(&st, LOONGBT_ADD, LOONGBT_W, 0x7fffffffu, 1);
    check(r == 0x80000000u && (st.eflags & LOONGBT_EF_OF), "add.w OF");

    reset();
    r = loongbt_exec(&st, LOONGBT_ADD, LOONGBT_W, 0xffffffffu, 1);
    check(r == 0 && (st.eflags & LOONGBT_EF_CF) && (st.eflags & LOONGBT_EF_ZF),
          "add.w CF/ZF");

    reset();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec(&st, LOONGBT_ADC, LOONGBT_W, 0xffffffffu, 0);
    check(r == 0 && (st.eflags & LOONGBT_EF_CF), "adc.w carry");

    reset();
    r = loongbt_exec(&st, LOONGBT_SUB, LOONGBT_W, 0, 1);
    check(r == 0xffffffffu && (st.eflags & LOONGBT_EF_CF) &&
          (st.eflags & LOONGBT_EF_SF), "sub.w borrow");

    reset();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec(&st, LOONGBT_SBC, LOONGBT_W, 5, 2);
    check(r == 2 && !(st.eflags & LOONGBT_EF_CF), "sbc.w");

    reset();
    r = loongbt_exec(&st, LOONGBT_AND, LOONGBT_W, 0xf0f0, 0x0f0f);
    check(r == 0 && (st.eflags & LOONGBT_EF_ZF) && (st.eflags & LOONGBT_EF_PF),
          "and.w flags");

    reset();
    st.eflags = LOONGBT_EF_CF;
    r = loongbt_exec(&st, LOONGBT_INC, LOONGBT_W, 0x7fffffffu, 0);
    check(r == 0x80000000u && (st.eflags & LOONGBT_EF_OF) &&
          (st.eflags & LOONGBT_EF_CF), "inc.w keeps CF");

    reset();
    r = loongbt_exec(&st, LOONGBT_MULU, LOONGBT_W, 0x10000, 0x10000);
    check(r == 0 && (st.eflags & LOONGBT_EF_CF) && (st.eflags & LOONGBT_EF_OF),
          "mulu.w overflow");

    reset();
    r = loongbt_exec(&st, LOONGBT_ROTL, LOONGBT_W, 0x80000001u, 1);
    check(r == 3 && (st.eflags & LOONGBT_EF_CF) && (st.eflags & LOONGBT_EF_OF),
          "rotl.w");

    reset();
    r = loongbt_exec(&st, LOONGBT_SRA, LOONGBT_W, 0x80000000u, 1);
    check(r == 0xc0000000u && !(st.eflags & LOONGBT_EF_CF), "sra.w");

    reset();
    st.eflags = LOONGBT_EF_CF | LOONGBT_EF_ZF | LOONGBT_EF_OF;
    r = loongbt_exec(&st, LOONGBT_MFFLAG, LOONGBT_W, 0, 63);
    check(r == (1u | (1u << 6) | (1u << 11)), "mfflag pack");
    reset();
    loongbt_exec(&st, LOONGBT_MTFLAG, LOONGBT_W, r, 63);
    check(st.eflags == (LOONGBT_EF_CF | LOONGBT_EF_ZF | LOONGBT_EF_OF), "mtflag");

    reset();
    st.eflags = LOONGBT_EF_ZF;
    check(loongbt_exec(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 4) == 1, "setx86j E");
    check(loongbt_exec(&st, LOONGBT_SETX86J, LOONGBT_W, 0, 5) == 0, "setx86j NE");

    reset();
    loongbt_exec(&st, LOONGBT_MTTOP, LOONGBT_W, 0, 5);
    loongbt_exec(&st, LOONGBT_INCTOP, LOONGBT_W, 0, 0);
    loongbt_exec(&st, LOONGBT_INCTOP, LOONGBT_W, 0, 0);
    loongbt_exec(&st, LOONGBT_INCTOP, LOONGBT_W, 0, 0);
    check(st.top == 0, "top wraps");

    reset();
    r = loongbt_exec_ir2(&st, LISA_X86ADD_W, 0x7fffffffu, 1, 0);
    check(r == 0x80000000u && (st.eflags & LOONGBT_EF_OF), "ir2 add.w");
    r = loongbt_exec_ir2(&st, LISA_X86MTFLAG, 0, 0, 0);
    check(r == 0, "ir2 mtflag mask 0");
}

int efi_main(void *image, void *systab)
{
    uint64_t lbt;

    (void)image;
    (void)systab;
    puts_("latx-efi: start\n");

    loongbt_set_cpu_features(0);      /* no LBT_X86 in cpu_flag */
    loongbt_set_hw_available(false); /* pure integer fallback */
    run_semantics();

    lbt = loongbt_hw_probe() ? 1 : 0;
    puts_("latx-efi: lbt=");
    puthex(lbt);
    puts_(" checks=");
    puthex((uint64_t)checks);
    puts_(" fails=");
    puthex((uint64_t)fails);
    puts_(fails == 0 ? " ok\n" : " bad\n");
    puts_("latx-efi: done\n");
    return fails == 0 ? 0 : 1;
}
