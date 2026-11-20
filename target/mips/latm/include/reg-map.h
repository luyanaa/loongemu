#ifndef LATM_REG_MAP_H
#define LATM_REG_MAP_H

#define NOT_DEF         -1
#define GPR_NUM         32

/* LoongArch GPR numbers (same names as the LATA/IR2 vocabulary). */
#define la_zero  0
#define la_ra    1
#define la_tp    2
#define la_sp    3
#define la_a0    4
#define la_a1    5
#define la_a2    6
#define la_a3    7
#define la_a4    8
#define la_a5    9
#define la_a6    10
#define la_a7    11
#define la_t0    12
#define la_t1    13
#define la_t2    14
#define la_t3    15
#define la_t4    16
#define la_t5    17
#define la_t6    18
#define la_t7    19
#define la_t8    20
#define la_r21   21
#define la_fp    22
#define la_s0    23
#define la_s1    24
#define la_s2    25
#define la_s3    26
#define la_s4    27
#define la_s5    28
#define la_s6    29
#define la_s7    30
#define la_s8    31

/* LBT / FPU helper register numbers. */
#define la_fsmask 15

/* Static (translator-private) registers. */
#define STATIC_NUM 2
#define S_ENV      0
#define S_EFLAGS   1

/* Temporary registers. */
#define ITEMP0 0
#define ITEMP1 1
#define ITEMP2 2
#define FTEMP0 0
#define FTEMP1 1
#define FTEMP2 2
#define FTEMP3 3
#define FTEMP4 4
#define FTEMP5 5
#define TRACE  0

extern const int reg_gpr_map[];
extern const int reg_fpr_map[];
extern const int reg_xmm_map[];
extern const int reg_mmx_map[];
extern const int reg_statics_map[];
extern const int reg_itemp_map[];
extern const int reg_ftemp_map[];
extern const int reg_itemp_reverse_map[];
extern const int reg_ftemp_reverse_map[];
extern const int reg_scr_map[];
extern const int reg_trace_map[];

#endif
