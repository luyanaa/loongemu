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
 * Mapping from one translated x86 instruction inside a TB to the native code
 * offset where its next x86 instruction begins.  Used by the hardware page
 * fault handler to recover the exact faulting x86 RIP from CSR.ERA when
 * option_guest_paging = 0.
 */
#define TB_HARNESS_MAX_INSNS 64

typedef struct TbInsnSpan {
    uint64_t x86_pc;
    uint16_t native_end_off;
} TbInsnSpan;

typedef struct TbInsnMap {
    uint16_t count;
    TbInsnSpan spans[TB_HARNESS_MAX_INSNS];
    /* Direct-branch link slots (`b 0` patched to `nop` at translate time). */
    int32_t jmp_slot_off[2];
    uint64_t jmp_target_pc[2];
} TbInsnMap;

/*
 * Translate the block starting at `pc` into `code`, at most `limit` bytes.
 * Returns the number of bytes emitted, or -1 if the block could not be
 * decoded or did not fit.  Fetch goes through latx_bare_ldub_code(), which
 * every harness defines.
 */
int tb_harness_translate_at(uint64_t pc, uint8_t *code, uint64_t limit);

/*
 * Same as tb_harness_translate_at(), and also records the per-instruction
 * x86 PC -> native offset table when `out_map` is non-NULL.
 */
int tb_harness_translate_at_with_map(uint64_t pc, uint8_t *code,
                                     uint64_t limit, TbInsnMap *out_map);

/*
 * Resolve a host ERA inside a translated block (`tb_code`) back to the
 * guest x86 PC that emitted that native instruction.  Falls back to
 * `fallback_pc` if `era` is outside the recorded spans.
 */
uint64_t tb_harness_resolve_x86_pc(const TbInsnMap *map, uint64_t tb_code,
                                   uint64_t era, uint64_t fallback_pc);

#endif
