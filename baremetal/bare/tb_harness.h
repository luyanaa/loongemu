/*
 * Shared bring-up plumbing for the bare translators that run a real guest
 * program one translated block at a time.
 *
 * The harnesses own their guest image and their assertions; this module owns
 * the two things that must stay identical between them -- decoding a block
 * into IR1 and patching the exit slots of the block it produced.
 */
#ifndef LATX_BARE_TB_HARNESS_H
#define LATX_BARE_TB_HARNESS_H

#include <stdint.h>

/*
 * Translate the block starting at `pc` into `code`, at most `limit` bytes.
 * Returns the number of bytes emitted, or -1 if the block could not be
 * decoded or did not fit.  Fetch goes through latx_bare_ldub_code(), which
 * every harness defines.
 */
int tb_harness_translate_at(uint64_t pc, uint8_t *code, uint64_t limit);

#endif
