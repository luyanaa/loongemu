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
#include "ir1.h"

/*
 * Mapping from one translated x86 instruction inside a TB to the native code
 * offset where its next x86 instruction begins.  Used by the hardware page
 * fault handler to recover the exact faulting x86 RIP from CSR.ERA when
 * option_guest_paging = 0.
 */
#define TB_HARNESS_MAX_INSNS 64

#define TB_HARNESS_FAULT_EA_NONE UINT8_MAX
#define TB_HARNESS_FAULT_REG_NONE UINT8_MAX
#define TB_HARNESS_FAULT_REG_RIP 0xfeu
#define TB_HARNESS_STACK_ACCESS_NONE 0u
#define TB_HARNESS_STACK_ACCESS_PUSH 1u
#define TB_HARNESS_STACK_ACCESS_POP 2u

typedef struct TbFaultEa {
    int32_t displacement;
    uint8_t base;          /* GPR index, TB_HARNESS_FAULT_REG_*, or RIP */
    uint8_t index;         /* GPR index or TB_HARNESS_FAULT_REG_NONE */
    uint8_t scale;         /* 1, 2, 4, or 8 */
    uint8_t address_size;  /* bytes: 4 or 8 */
    uint8_t size;          /* bytes accessed */
    uint8_t segment_prefix; /* effective FS/GS override byte, or zero */
    uint8_t ea_seg_class;  /* class of this explicit memory EA */
    uint8_t reserved;
} TbFaultEa;

typedef struct TbInsnSpan {
    uint64_t x86_pc;
    uint16_t native_end_off;
    uint8_t  ea_seg_class; /* IR1_EA_SEG_*; UNKNOWN without one unambiguous fault class */
    uint8_t  x86_size;
    uint8_t  fault_ea_index; /* index into TbInsnMap.fault_eas, or NONE */
    uint8_t  stack_access_kind;
    uint8_t  stack_access_size;
    uint8_t  reserved;
} TbInsnSpan;

typedef struct TbInsnMap {
    uint16_t count;
    uint16_t fault_ea_count;
    TbInsnSpan spans[TB_HARNESS_MAX_INSNS];
    TbFaultEa fault_eas[TB_HARNESS_MAX_INSNS];
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
 * guest x86 PC and its decode-time IR1_EA_SEG_* class.  The class is UNKNOWN
 * when the ERA is outside a recorded instruction span or no single faulting
 * EA class can be assigned to the instruction.
 */
uint64_t tb_harness_resolve_x86_pc(const TbInsnMap *map, uint64_t tb_code,
                                   uint64_t era, uint64_t fallback_pc);
uint64_t tb_harness_resolve_x86_span(const TbInsnMap *map, uint64_t tb_code,
                                     uint64_t era, uint64_t fallback_pc,
                                     uint8_t *out_ea_seg_class);
/* Resolve only when ERA lies inside a recorded instruction span. */
int tb_harness_resolve_x86_fault(const TbInsnMap *map, uint64_t tb_code,
                                 uint64_t era, uint64_t fallback_pc,
                                 uint64_t *out_x86_pc,
                                 uint8_t *out_ea_seg_class);
/*
 * Refine supported x86-64 stack+data instructions by matching BADV against
 * the decoded memory EA and implicit stack EA. `gpr` uses the x86 order
 * RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8-R15. This is valid only when
 * generated accesses use guest-linear addresses directly (guest paging off);
 * unsupported or ambiguous cases remain UNKNOWN.
 */
int tb_harness_refine_x86_fault_ea(const TbInsnMap *map, uint64_t tb_code,
                                   uint64_t era, uint64_t badv,
                                   const uint64_t gpr[16],
                                   uint64_t fs_base, uint64_t gs_base,
                                   uint8_t *out_ea_seg_class);

#endif
