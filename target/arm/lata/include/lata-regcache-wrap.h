#ifndef LATA_REGCACHE_WRAP_H
#define LATA_REGCACHE_WRAP_H

#include "lata.h"

#define la_label(label) lata_regcache_label(label)
#define la_jirl(rd, rj, offset) lata_regcache_jirl(rd, rj, offset)
#define la_b(label) lata_regcache_b(label)
#define la_beqz(reg, label) lata_regcache_beqz(reg, label)
#define la_bnez(reg, label) lata_regcache_bnez(reg, label)
#define la_beq(lhs, rhs, label) lata_regcache_beq(lhs, rhs, label)
#define la_bne(lhs, rhs, label) lata_regcache_bne(lhs, rhs, label)
#define la_blt(lhs, rhs, label) lata_regcache_blt(lhs, rhs, label)
#define la_bge(lhs, rhs, label) lata_regcache_bge(lhs, rhs, label)
#define la_bltu(lhs, rhs, label) lata_regcache_bltu(lhs, rhs, label)
#define la_bgeu(lhs, rhs, label) lata_regcache_bgeu(lhs, rhs, label)
#define la_bcnez(cond, label) lata_regcache_bcnez(cond, label)

#endif
