#include "sve-decode.h"

typedef void (*SveExtract)(uint32_t insn, union a64_u *arg);

typedef struct {
    uint32_t mask;
    uint32_t value;
    AARCH64_INSN_TYPE_A64 insn_type;
    SveExtract extract;
} SveDecodePattern;

static unsigned field(uint32_t insn, unsigned lsb, unsigned width)
{
    return (insn >> lsb) & ((1u << width) - 1u);
}

static int signed_imm4(uint32_t insn)
{
    unsigned imm = field(insn, 16, 4);
    return (int)imm - ((imm & 8u) ? 16 : 0);
}

static void init_effects(arg_sve_effect_intent *effects,
                         PvirInactivePolicy inactive_policy)
{
    *effects = (arg_sve_effect_intent){0};
    effects->inactive_policy = inactive_policy;
    effects->memory.access = PVIR_MEMORY_NONE;
    effects->memory.ordering = PVIR_MEMORY_ORDER_UNSPECIFIED;
    effects->memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NOT_APPLICABLE;
    effects->fault_behavior = PVIR_FAULT_NONE;
    effects->fault_order = PVIR_FAULT_ORDER_UNSPECIFIED;
    effects->fault_result = PVIR_FAULT_RESULT_UNSPECIFIED;
    effects->state_domain = PVIR_ARCH_STATE_DOMAIN_NONE;
    effects->state_action = PVIR_ARCH_STATE_ACTION_NONE;
    effects->state_update = PVIR_ARCH_STATE_UPDATE_NONE;
}

static void init_predicated_memory(arg_sve_effect_intent *effects,
                                   PvirMemoryAccess access,
                                   PvirInactivePolicy inactive_policy)
{
    init_effects(effects, inactive_policy);
    effects->memory.access = access;
    effects->memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
    effects->memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
    effects->fault_behavior = PVIR_FAULT_PRECISE;
    /* C8.2.298/704 visit active lanes in ascending order. */
    effects->fault_order = PVIR_FAULT_ORDER_LANE_ASCENDING;
}

static void decode_ptrue(uint32_t insn, union a64_u *arg)
{
    arg_sve_ptrue *out = &arg->f_sve_ptrue;
    out->pd = field(insn, 0, 4);
    out->pattern = field(insn, 5, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_whilelo(uint32_t insn, union a64_u *arg)
{
    arg_sve_whilelo *out = &arg->f_sve_whilelo;
    out->pd = field(insn, 0, 4);
    out->rn = field(insn, 5, 5);
    out->rm = field(insn, 16, 5);
    out->element_bits = 8u << field(insn, 22, 2);
    out->scalar_bits = field(insn, 12, 1) ? 64 : 32;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.writes_nzcv = true;
}

static void decode_integer_binary(uint32_t insn,
                                  arg_sve_integer_binary *out)
{
    out->zd = field(insn, 0, 5);
    out->zm = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->element_bits = 8u << field(insn, 22, 2);
    init_effects(&out->effects, PVIR_INACTIVE_MERGE);
}

static void decode_add(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_add);
}

static void decode_sub(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_sub);
}
static void decode_mul(uint32_t insn, union a64_u *arg)
{
    decode_integer_binary(insn, &arg->f_sve_mul);
}

static void decode_ld1b(uint32_t insn, union a64_u *arg)
{
    arg_sve_contiguous *out = &arg->f_sve_contiguous;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->immediate_vl = signed_imm4(insn);
    out->element_bits = 8u << field(insn, 21, 2);
    out->memory_bits = 8;
    out->base_is_sp = out->rn == 31;
    init_predicated_memory(&out->effects, PVIR_MEMORY_READ,
                           PVIR_INACTIVE_ZERO);
}

static void decode_st1b(uint32_t insn, union a64_u *arg)
{
    arg_sve_contiguous *out = &arg->f_sve_contiguous;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->immediate_vl = signed_imm4(insn);
    out->element_bits = 8u << field(insn, 21, 2);
    out->memory_bits = 8;
    out->base_is_sp = out->rn == 31;
    init_predicated_memory(&out->effects, PVIR_MEMORY_WRITE,
                           PVIR_INACTIVE_NOT_APPLICABLE);
}

static void decode_setffr(uint32_t insn, union a64_u *arg)
{
    arg_sve_ffr *out = &arg->f_sve_ffr;
    (void)insn;
    out->has_pd = false;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    out->effects.state_action = PVIR_ARCH_STATE_WRITE;
    out->effects.ffr_initializes_all_true = true;
}

static void decode_rdffr(uint32_t insn, union a64_u *arg)
{
    arg_sve_ffr *out = &arg->f_sve_ffr;
    out->pd = field(insn, 0, 4);
    out->has_pd = true;
    init_effects(&out->effects, PVIR_INACTIVE_NOT_APPLICABLE);
    out->effects.state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    out->effects.state_action = PVIR_ARCH_STATE_READ;
}

static void init_faulting_load(arg_sve_effect_intent *effects,
                               PvirFaultBehavior behavior)
{
    init_predicated_memory(effects, PVIR_MEMORY_READ, PVIR_INACTIVE_ZERO);
    effects->fault_behavior = behavior;
    effects->fault_order = behavior == PVIR_FAULT_FIRST_FAULT
                               ? PVIR_FAULT_ORDER_LANE_ASCENDING
                               : PVIR_FAULT_ORDER_UNSPECIFIED;
    effects->fault_result = PVIR_FAULT_RESULT_UNKNOWN;
    effects->state_domain = PVIR_ARCH_STATE_DOMAIN_FIRST_FAULT;
    effects->state_action = PVIR_ARCH_STATE_UPDATE;
    effects->state_update = PVIR_ARCH_STATE_UPDATE_CUMULATIVE_SUCCESS;
    effects->result_unknown_from_fault_boundary = true;
    effects->result_unknown_where_ffr_input_clear = true;
    effects->ffr_clears_from_fault_boundary = true;
}

static void decode_ldff1b(uint32_t insn, union a64_u *arg)
{
    arg_sve_fault_load *out = &arg->f_sve_fault_load;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->rm = field(insn, 16, 5);
    out->element_bits = 8u << field(insn, 21, 2);
    out->memory_bits = 8;
    out->base_is_sp = out->rn == 31;
    out->has_index = out->rm != 31;
    init_faulting_load(&out->effects, PVIR_FAULT_FIRST_FAULT);
}

static void decode_ldnf1b(uint32_t insn, union a64_u *arg)
{
    arg_sve_nonfault_load *out = &arg->f_sve_nonfault_load;
    out->zt = field(insn, 0, 5);
    out->rn = field(insn, 5, 5);
    out->pg = field(insn, 10, 3);
    out->immediate_vl = signed_imm4(insn);
    out->element_bits = 8u << field(insn, 21, 2);
    out->memory_bits = 8;
    out->base_is_sp = out->rn == 31;
    init_faulting_load(&out->effects, PVIR_FAULT_NONFAULTING);
}

/*
 * Encoding masks and field layouts follow Arm ARM DDI 0487, Part C, Chapter
 * C8 SVE instruction descriptions. Variable fields are deliberately confined
 * to this table and the extractors above; the legacy A64 decoder never probes
 * SVE bits itself.
 */
static const SveDecodePattern sve_patterns[] = {
    /* C8.2 PTRUE (predicate): Pd, pattern, size. */
    { 0xff3ffc10u, 0x2518e000u, AARCH64_A64_SVE_PTRUE, decode_ptrue },
    /* C8.2 WHILELO: X operands; size bits select width (Pd, Rn, Rm). */
    { 0xff20fc10u, 0x25201c00u, AARCH64_A64_SVE_WHILELO, decode_whilelo },
    /* C8.2 ADD (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04000000u, AARCH64_A64_SVE_ADD, decode_add },
    /* C8.2.778 SUB (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04010000u, AARCH64_A64_SVE_SUB, decode_sub },
    /* C8.2 MUL (vectors, predicated): Zdn, Zm, Pg, size; /M only. */
    { 0xff3fe000u, 0x04100000u, AARCH64_A64_SVE_MUL, decode_mul },
    /* C8.2 LD1B/ST1B, .B scalar plus immediate, single register. */
    { 0xfff0e000u, 0xa400a000u, AARCH64_A64_SVE_LD1B, decode_ld1b },
    { 0xfff0e000u, 0xe400e000u, AARCH64_A64_SVE_ST1B, decode_st1b },
    /* C8.2 SETFFR: fixed encoding. */
    { 0xffffffffu, 0x252c9000u, AARCH64_A64_SVE_SETFFR, decode_setffr },
    /* C8.2 RDFFR (unpredicated): Pd. */
    { 0xfffffff0u, 0x2519f000u, AARCH64_A64_SVE_RDFFR, decode_rdffr },
    /* C8.2 LDFF1B, .B scalar plus scalar. */
    { 0xffe0e000u, 0xa4006000u, AARCH64_A64_SVE_LDFF1B, decode_ldff1b },
    /* C8.2 LDNF1B, .B scalar plus immediate. */
    { 0xfff0e000u, 0xa410a000u, AARCH64_A64_SVE_LDNF1B, decode_ldnf1b },
};

static bool has_sve_primary_prefix(uint32_t insn)
{
    const uint32_t prefix_mask = 0xff000000u;
    unsigned i;

    for (i = 0; i < sizeof(sve_patterns) / sizeof(sve_patterns[0]); i++) {
        const SveDecodePattern *pattern = &sve_patterns[i];

        if ((pattern->mask & prefix_mask) == prefix_mask &&
            (insn & prefix_mask) == (pattern->value & prefix_mask)) {
            return true;
        }
    }
    return false;
}

A64SVEDecodeStatus a64_sve_decode(uint32_t insn,
                                  A64SVEIR1Instruction *decoded)
{
    if (decoded == NULL) {
        return A64_SVE_DECODE_INVALID_ARGUMENT;
    }

    *decoded = (A64SVEIR1Instruction){0};
    decoded->insn_type = AARCH64_A64_NULL;

    for (unsigned i = 0;
         i < sizeof(sve_patterns) / sizeof(sve_patterns[0]); i++) {
        const SveDecodePattern *pattern = &sve_patterns[i];

        if ((insn & pattern->mask) == pattern->value) {
            decoded->insn_type = pattern->insn_type;
            pattern->extract(insn, &decoded->arg);
            return A64_SVE_DECODE_MATCH;
        }
    }

    if (has_sve_primary_prefix(insn)) {
        decoded->insn_type = AARCH64_A64_SVE_UNSUPPORTED_ENCODING;
        decoded->arg.f_sve_unsupported_encoding.raw_insn = insn;
        return A64_SVE_DECODE_UNSUPPORTED_ENCODING;
    }

    return A64_SVE_DECODE_NO_MATCH;
}

A64SVEAccessDisposition
a64_sve_access_disposition(bool sve_implemented, A64SVEControlState state)
{
    if (!sve_implemented) {
        return A64_SVE_ACCESS_FEATURE_ABSENT;
    }

    switch (state) {
    case A64_SVE_CONTROL_UNMODELED:
        return A64_SVE_ACCESS_CONTROL_UNSUPPORTED;
    case A64_SVE_CONTROL_ALLOWED:
        return A64_SVE_ACCESS_ALLOWED;
    case A64_SVE_CONTROL_DENIED:
        return A64_SVE_ACCESS_TRAP_REQUIRED;
    }

    return A64_SVE_ACCESS_CONTROL_UNSUPPORTED;
}
