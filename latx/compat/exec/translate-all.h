#ifndef LATX_BARE_EXEC_TRANSLATE_ALL_H
#define LATX_BARE_EXEC_TRANSLATE_ALL_H
/* Translation-block cache helpers referenced by the translator. */
#include "exec/exec-all.h"
void tb_invalidate_phys_range(uint64_t start, uint64_t end);
int get_insn_len_readable(uint64_t pc);
extern IntervalTreeRoot pageflags_root;
#endif
