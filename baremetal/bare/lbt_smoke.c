#include <stdint.h>
#define UART ((volatile unsigned char *)0x1fe001e0)
static void pc(char c){*UART=c;}
static void ps(const char*s){while(*s)pc(*s++);}
static void ph(uint64_t v){ps("0x");for(int i=15;i>=0;i--)pc("0123456789abcdef"[(v>>(i*4))&15]);}
uint64_t out[16];
uint64_t membuf[2];
void run_lbt(void);
void kmain(void);
__asm__(
".text\n.globl _start\n_start:\n la $sp, kstack\n lu12i.w $t0, 16\n add.d $sp,$sp,$t0\n bl kmain\n1: b 1b\n"
".text\n.globl run_lbt\nrun_lbt:\n"
" la $s0, out\n"
" la $t0, membuf\n addi.d $t0, $t0, 1\n"
" lu12i.w $t1, 0x11223\n ori $t1, $t1, 0x344\n"
" .word 0x2f00018d\n"          /* stl.w t1,t0,0 */
" .word 0x2f400d8d\n"          /* str.w t1,t0,3 */
" ori $t2, $zero, 0\n"
" .word 0x2e00018e\n"          /* ldl.w t2,t0,0 */
" .word 0x2e400d8e\n"          /* ldr.w t2,t0,3 */
" st.d $t2, $s0, 88\n"

" ori $t0,$zero,5\n ori $t1,$zero,7\n"
" .word 0x0037359e\n"          /* armadd.w t0,t1,AL */
" .word 0x005ce44e\n"          /* armmfflag t2,0x39 */
" st.d $t2,$s0,48\n"
" ori $t2,$zero,1\n slli.d $t2,$t2,30\n" /* Z bit -> value 0x40000000 */
" .word 0x005c206e\n"          /* armmtflag t2,0x8 */
" .word 0x005c204e\n"          /* armmfflag t2,0x8 */
" st.d $t2,$s0,80\n"
" .word 0x0036c00f\n"          /* setarmj t3,EQ */
" st.d $t3,$s0,56\n"
" ori $t0,$zero,5\n"
" .word 0x0029858e\n"          /* addu12i.d t2,t0,1 */
" st.d $t2,$s0,64\n"
" addi.d $t0,$zero,-1\n srli.d $t0,$t0,1\n addi.d $t0,$t0,1\n ori $t1,$zero,2\n"
" .word 0x003eb583\n"          /* x86mul.d t0,t1 */
" .word 0x005c840e\n"          /* x86mfflag t2,0x21 */
" st.d $t2,$s0,72\n"

" ori $t0,$zero,5\n ori $t1,$zero,7\n"
" .word 0x003f3587\n"          /* x86add.d t0,t1 */
" .word 0x005cfc0e\n"          /* x86mfflag t2,0x3f */
" st.d $t2,$s0,0\n"
" ori $t2,$zero,1\n"
" .word 0x005c042e\n"          /* x86mtflag t2,1 (CF=1) */
" addi.d $t0,$zero,-1\n ori $t1,$zero,0\n"
" .word 0x003f358f\n"          /* x86adc.d t0,t1 */
" .word 0x005cfc0e\n st.d $t2,$s0,8\n"
" ori $t2,$zero,0x40\n"
" .word 0x005c242e\n"          /* x86mtflag t2,8 (ZF=1) */
" .word 0x0036900f\n"          /* setx86j t3,4 (E) */
" st.d $t3,$s0,16\n"
" lu12i.w $t0,0x1234\n"
" .word 0x00000980\n"          /* movgr2scr scr0,t0 */
" .word 0x00000c0e\n"          /* movscr2gr t2,scr0 */
" st.d $t2,$s0,24\n"
" li.d $t2,0x8d5\n"
" .word 0x005cfc2e\n"          /* x86mtflag t2,0x3f */
" .word 0x005cfc0e\n"          /* x86mfflag t2,0x3f */
" st.d $t2,$s0,32\n"
" addi.d $t0,$zero,-1\n srli.d $t0,$t0,1\n addi.d $t0,$t0,1\n"
" .word 0x00550583\n"          /* x86slli.d t0,1 */
" .word 0x005cfc0e\n st.d $t2,$s0,40\n"
" jirl $zero,$ra,0\n"
);
static char kstack[65536] __attribute__((aligned(16),used));
void kmain(void){
  for (int i=0;i<16;i++) out[i]=0;
  ps("lbt-smoke: run\n");
  run_lbt();
  ps("lbt-smoke: add="); ph(out[0]); ps("\n");
  ps("lbt-smoke: adc="); ph(out[1]); ps("\n");
  ps("lbt-smoke: setcc="); ph(out[2]); ps("\n");
  ps("lbt-smoke: scr="); ph(out[3]); ps("\n");
  ps("lbt-smoke: mt/mf="); ph(out[4]); ps("\n");
  ps("lbt-smoke: shl="); ph(out[5]); ps("\n");
  ps("lbt-smoke: armadd="); ph(out[6]); ps("\n");
  ps("lbt-smoke: armflags="); ph(out[10]); ps("\n");
  ps("lbt-smoke: setarmj="); ph(out[7]); ps("\n");
  ps("lbt-smoke: addu12i="); ph(out[8]); ps("\n");
  ps("lbt-smoke: mul="); ph(out[9]); ps("\n");
  ps("lbt-smoke: mips="); ph(out[11]); ps("\n");
  ps((out[0]==0x4 && out[1]==0x55 && out[2]==1 && out[3]==0x1234000 && out[4]==0x8d5 && out[5]==0x845 && out[6]==0 && out[7]==1 && out[8]==0x1005 && out[9]==0x801 && out[11]==0x11223344) ? "lbt-smoke: PASS\n" : "lbt-smoke: FAIL\n");
  ps("lbt-smoke: done\n");
}
