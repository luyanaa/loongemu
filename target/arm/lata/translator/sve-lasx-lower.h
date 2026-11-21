#ifndef LATA_SVE_LASX_LOWER_H
#define LATA_SVE_LASX_LOWER_H

#include <stdbool.h>

struct DisasContext;

bool trans_sve_ptrue(struct DisasContext *s);
bool trans_sve_add(struct DisasContext *s);
bool trans_sve_sub(struct DisasContext *s);
bool trans_sve_mul(struct DisasContext *s);
bool trans_sve_whilelo(struct DisasContext *s);

bool trans_sve_ld1b(struct DisasContext *s);
bool trans_sve_st1b(struct DisasContext *s);
bool trans_sve_setffr(struct DisasContext *s);
bool trans_sve_rdffr(struct DisasContext *s);

#endif
