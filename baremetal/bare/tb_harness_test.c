/*
 * Deterministic tests for decode-time #GP/#SS metadata and its propagation to
 * the native-instruction spans consumed by the bare harness.
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "lsenv.h"
#include "ir1.h"
#include "tb_harness.h"

extern void latx_bare_uart_puts(const char *s);

#define IR1_TEST_GUEST_PC UINT64_C(0x1fff)


typedef struct Ir1EaVector {
    const char *name;
    uint8_t mode;
    uint8_t size;
    uint8_t expected_class;
    uint8_t bytes[8];
} Ir1EaVector;

static const Ir1EaVector ir1_ea_vectors[] = {
    { "64 [rsp]", LATX_DT_X64, 4, IR1_EA_SEG_SS,
      { 0x48, 0x8b, 0x04, 0x24 } },
    { "64 [rbp]", LATX_DT_X64, 4, IR1_EA_SEG_SS,
      { 0x48, 0x8b, 0x45, 0x00 } },
    { "64 [r12] SIB+REX.B", LATX_DT_X64, 4, IR1_EA_SEG_NOT_SS,
      { 0x49, 0x8b, 0x04, 0x24 } },
    { "64 [r13] REX.B", LATX_DT_X64, 4, IR1_EA_SEG_NOT_SS,
      { 0x49, 0x8b, 0x45, 0x00 } },
    { "64 addr32 [r12d]", LATX_DT_X64, 5, IR1_EA_SEG_NOT_SS,
      { 0x67, 0x49, 0x8b, 0x04, 0x24 } },
    { "64 addr32 [r13d]", LATX_DT_X64, 5, IR1_EA_SEG_NOT_SS,
      { 0x67, 0x49, 0x8b, 0x45, 0x00 } },
    { "64 ignored DS override [rsp]", LATX_DT_X64, 5, IR1_EA_SEG_SS,
      { 0x3e, 0x48, 0x8b, 0x04, 0x24 } },
    { "64 ignored SS override [rax]", LATX_DT_X64, 4, IR1_EA_SEG_NOT_SS,
      { 0x36, 0x48, 0x8b, 0x00 } },
    { "64 FS override [rsp]", LATX_DT_X64, 5, IR1_EA_SEG_NOT_SS,
      { 0x64, 0x48, 0x8b, 0x04, 0x24 } },
    { "64 FS then ignored DS override [rsp]", LATX_DT_X64, 7,
      IR1_EA_SEG_NOT_SS,
      { 0x64, 0x3e, 0x48, 0x8b, 0x44, 0x24, 0x00 } },
    { "64 [rax]", LATX_DT_X64, 3, IR1_EA_SEG_NOT_SS,
      { 0x48, 0x8b, 0x00 } },
    { "64 push reg", LATX_DT_X64, 1, IR1_EA_SEG_SS,
      { 0x54 } },
    { "64 pop reg", LATX_DT_X64, 1, IR1_EA_SEG_SS,
      { 0x5c } },
    { "64 push [rax] mixed", LATX_DT_X64, 2, IR1_EA_SEG_UNKNOWN,
      { 0xff, 0x30 } },
    { "64 push [rsp]", LATX_DT_X64, 3, IR1_EA_SEG_SS,
      { 0xff, 0x34, 0x24 } },
    { "64 push [r12] mixed", LATX_DT_X64, 4, IR1_EA_SEG_UNKNOWN,
      { 0x41, 0xff, 0x34, 0x24 } },
    { "64 pop [rax] mixed", LATX_DT_X64, 2, IR1_EA_SEG_UNKNOWN,
      { 0x8f, 0x00 } },
    { "64 call [rax] mixed", LATX_DT_X64, 2, IR1_EA_SEG_UNKNOWN,
      { 0xff, 0x10 } },
    { "64 call [rsp]", LATX_DT_X64, 3, IR1_EA_SEG_SS,
      { 0xff, 0x14, 0x24 } },
    { "64 ret", LATX_DT_X64, 1, IR1_EA_SEG_SS,
      { 0xc3 } },
    { "64 MOVSB", LATX_DT_X64, 1, IR1_EA_SEG_NOT_SS,
      { 0xa4 } },
    { "64 ignored SS MOVSB", LATX_DT_X64, 2, IR1_EA_SEG_NOT_SS,
      { 0x36, 0xa4 } },
    { "64 ignored SS LODSB", LATX_DT_X64, 2, IR1_EA_SEG_NOT_SS,
      { 0x36, 0xac } },
    { "64 SS STOSB ignored", LATX_DT_X64, 2, IR1_EA_SEG_NOT_SS,
      { 0x36, 0xaa } },
    { "64 string MOVSD", LATX_DT_X64, 1, IR1_EA_SEG_NOT_SS,
      { 0xa5 } },
    { "64 ignored SS MOVSD", LATX_DT_X64, 2, IR1_EA_SEG_NOT_SS,
      { 0x36, 0xa5 } },
    { "64 string CMPSD", LATX_DT_X64, 1, IR1_EA_SEG_NOT_SS,
      { 0xa7 } },
    { "64 ignored SS CMPSD", LATX_DT_X64, 2, IR1_EA_SEG_NOT_SS,
      { 0x36, 0xa7 } },
    { "64 scalar CMPSD [rax]", LATX_DT_X64, 5, IR1_EA_SEG_NOT_SS,
      { 0xf2, 0x0f, 0xc2, 0x00, 0x00 } },
    { "64 scalar MOVSD [rax]", LATX_DT_X64, 4, IR1_EA_SEG_NOT_SS,
      { 0xf2, 0x0f, 0x10, 0x00 } },
    { "64 LEA is not a data EA", LATX_DT_X64, 3, IR1_EA_SEG_UNKNOWN,
      { 0x48, 0x8d, 0x00 } },
    { "32 [esp]", LATX_DT_X86, 3, IR1_EA_SEG_SS,
      { 0x8b, 0x04, 0x24 } },
    { "32 [ebp]", LATX_DT_X86, 3, IR1_EA_SEG_SS,
      { 0x8b, 0x45, 0x00 } },
    { "32 DS override [ebp]", LATX_DT_X86, 4, IR1_EA_SEG_NOT_SS,
      { 0x3e, 0x8b, 0x45, 0x00 } },
    { "32 SS override [eax]", LATX_DT_X86, 3, IR1_EA_SEG_SS,
      { 0x36, 0x8b, 0x00 } },
    { "32 SS XLAT implicit EA", LATX_DT_X86, 2, IR1_EA_SEG_SS,
      { 0x36, 0xd7 } },
    { "32 SS MASKMOVDQU implicit EA", LATX_DT_X86, 5, IR1_EA_SEG_SS,
      { 0x36, 0x66, 0x0f, 0xf7, 0xc1 } },
    { "32 SS MASKMOVQ implicit EA", LATX_DT_X86, 4, IR1_EA_SEG_SS,
      { 0x36, 0x0f, 0xf7, 0xc1 } },
    { "64 VMASKMOVDQU implicit DS EA", LATX_DT_X64, 4,
      IR1_EA_SEG_NOT_SS,
      { 0xc5, 0xf9, 0xf7, 0xc1 } },
    { "16 [bp]", LATX_DT_X16, 3, IR1_EA_SEG_SS,
      { 0x8b, 0x46, 0x00 } },
    { "16 [bx]", LATX_DT_X16, 2, IR1_EA_SEG_NOT_SS,
      { 0x8b, 0x07 } },
    { "16 DS override [bp]", LATX_DT_X16, 4, IR1_EA_SEG_NOT_SS,
      { 0x3e, 0x8b, 0x46, 0x00 } },
    { "16 SS override [bx]", LATX_DT_X16, 3, IR1_EA_SEG_SS,
      { 0x36, 0x8b, 0x07 } },
};

static int ir1_ea_test_fail(const char *name)
{
    latx_bare_uart_puts("FAIL ir1-ea: ");
    latx_bare_uart_puts(name);
    latx_bare_uart_puts("\n");
    return 1;
}

static int test_ir1_ea_vectors(CPUX86State *env)
{
    uint8_t code[16] __attribute__((aligned(8)));
    uint8_t decoded_area[2048] __attribute__((aligned(16)));
    uint8_t saved_mode = env->sys.codemode;
    int failures = 0;

    for (unsigned i = 0; i < sizeof(ir1_ea_vectors) / sizeof(ir1_ea_vectors[0]); i++) {
        const Ir1EaVector *vector = &ir1_ea_vectors[i];
        IR1_INST ir1;
        ADDRX next;

        env->sys.codemode = vector->mode;
        memset(code, 0x90, sizeof(code));
        memcpy(code, vector->bytes, vector->size);
        memset(decoded_area, 0, sizeof(decoded_area));
        memset(&ir1, 0, sizeof(ir1));
        next = ir1_disasm(&ir1, code, (ADDRX)0x1000, 0, decoded_area);
        if (next != (ADDRX)(0x1000 + vector->size) ||
            ir1_ea_seg_class(&ir1) != vector->expected_class) {
            failures += ir1_ea_test_fail(vector->name);
        }
    }
    env->sys.codemode = saved_mode;
    return failures;
}

static int test_effective_segment_override(CPUX86State *env)
{
    static const uint8_t bytes[] = {
        0x64, 0x3e, 0x48, 0x8b, 0x44, 0x24, 0x00
    };
    uint8_t code[16] __attribute__((aligned(8)));
    uint8_t decoded_area[2048] __attribute__((aligned(16)));
    uint8_t saved_mode = env->sys.codemode;
    IR1_INST ir1;
    ADDRX next;
    int failures = 0;

    env->sys.codemode = LATX_DT_X64;
    memset(code, 0x90, sizeof(code));
    memcpy(code, bytes, sizeof(bytes));
    memset(decoded_area, 0, sizeof(decoded_area));
    memset(&ir1, 0, sizeof(ir1));
    next = ir1_disasm(&ir1, code, (ADDRX)0x1000, 0, decoded_area);
    if (next != (ADDRX)(0x1000 + sizeof(bytes)) ||
        ir1.ea_segment_prefix != dt_X86_PREFIX_FS ||
        ir1.info->x86.prefix[1] != dt_X86_PREFIX_FS ||
        ir1_opnd_seg_reg(ir1_get_opnd(&ir1, 1)) != dt_X86_REG_FS) {
        failures += ir1_ea_test_fail(
            "64 FS followed by ignored DS normalizes prefix and mem.segment");
    }
    env->sys.codemode = saved_mode;
    return failures;
}


static int test_unknown_span_class(void)
{
    TbInsnMap map = { 0 };
    uint64_t gpr[16] = { 0 };
    uint64_t x86_pc = 0;
    uint8_t ea_seg_class = IR1_EA_SEG_SS;
    uint64_t code = UINT64_C(0x100000);
    int failures = 0;

    map.count = 1;
    map.spans[0].x86_pc = UINT64_C(0x4000);
    map.spans[0].native_end_off = 8;
    map.spans[0].ea_seg_class = IR1_EA_SEG_UNKNOWN;
    map.spans[0].fault_ea_index = TB_HARNESS_FAULT_EA_NONE;
    if (!tb_harness_resolve_x86_fault(&map, code, code + 4, 0,
                                      &x86_pc, &ea_seg_class) ||
        x86_pc != UINT64_C(0x4000) || ea_seg_class != IR1_EA_SEG_UNKNOWN) {
        failures += ir1_ea_test_fail("resolver preserves UNKNOWN span class");
    }
    if (tb_harness_resolve_x86_fault(&map, code, code + 8, 0,
                                     &x86_pc, &ea_seg_class) ||
        ea_seg_class != IR1_EA_SEG_UNKNOWN) {
        failures += ir1_ea_test_fail("resolver rejects ERA after last span");
    }
    if (tb_harness_refine_x86_fault_ea(
            &map, code, code + 4, 0, gpr, 0, 0, &ea_seg_class) ||
        ea_seg_class != IR1_EA_SEG_UNKNOWN) {
        failures += ir1_ea_test_fail(
            "UNKNOWN span without decoded EA remains UNKNOWN");
    }
    return failures;
}

static int test_cross_page_decode(uint8_t *native_code, uint64_t native_limit)
{
    const uint64_t pc = IR1_TEST_GUEST_PC;
    const uint64_t native_pc = (uint64_t)(uintptr_t)native_code;
    uint8_t *guest_code = (uint8_t *)(uintptr_t)pc;
    TbInsnMap map;
    uint64_t x86_pc = 0;
    uint8_t ea_seg_class = IR1_EA_SEG_UNKNOWN;
    int rc;
    int failures = 0;

    if (native_code == NULL || native_limit < 4096) {
        return ir1_ea_test_fail("cross-page test lacks a native code buffer");
    }

    /* The REX prefix is the final byte of one page; opcode/modrm follow. */
    guest_code[0] = 0x48;
    guest_code[1] = 0x8b;
    guest_code[2] = 0x00; /* mov rax, [rax] -> #GP class */
    guest_code[3] = 0x54; /* push rsp -> #SS class */
    guest_code[4] = 0x64;
    guest_code[5] = 0xff;
    guest_code[6] = 0x30; /* push fs:[rax] -> mixed stack/data accesses */
    guest_code[7] = 0x8f;
    guest_code[8] = 0x00; /* pop [rax] -> mixed stack/data accesses */
    guest_code[9] = 0xff;
    guest_code[10] = 0x10; /* call [rax] -> mixed stack/data accesses */
    memset(native_code, 0, 4096);
    memset(&map, 0, sizeof(map));

    rc = tb_harness_translate_at_with_map(pc, native_code, native_limit, &map);
    if (rc <= 0 || map.count != 5 ||
        map.spans[0].x86_pc != pc ||
        map.spans[0].ea_seg_class != IR1_EA_SEG_NOT_SS ||
        map.spans[1].x86_pc != pc + 3 ||
        map.spans[1].ea_seg_class != IR1_EA_SEG_SS ||
        map.spans[2].x86_pc != pc + 4 ||
        map.spans[2].ea_seg_class != IR1_EA_SEG_UNKNOWN ||
        map.spans[2].fault_ea_index != 0 ||
        map.spans[2].stack_access_kind != TB_HARNESS_STACK_ACCESS_PUSH ||
        map.spans[3].x86_pc != pc + 7 ||
        map.spans[3].ea_seg_class != IR1_EA_SEG_UNKNOWN ||
        map.spans[3].fault_ea_index != 1 ||
        map.spans[3].stack_access_kind != TB_HARNESS_STACK_ACCESS_POP ||
        map.spans[4].x86_pc != pc + 9 ||
        map.spans[4].ea_seg_class != IR1_EA_SEG_UNKNOWN ||
        map.spans[4].fault_ea_index != 2 ||
        map.spans[4].stack_access_kind != TB_HARNESS_STACK_ACCESS_PUSH ||
        map.fault_ea_count != 3 || map.fault_eas[0].base != 0 ||
        map.fault_eas[0].segment_prefix != dt_X86_PREFIX_FS ||
        map.fault_eas[0].ea_seg_class != IR1_EA_SEG_NOT_SS ||
        map.fault_eas[1].base != 0 || map.fault_eas[2].base != 0) {
        return ir1_ea_test_fail("cross-page decode preserves per-instruction classes");
    }

    if (!tb_harness_resolve_x86_fault(
            &map, native_pc, native_pc + 1, 0,
            &x86_pc, &ea_seg_class) ||
        x86_pc != pc || ea_seg_class != IR1_EA_SEG_NOT_SS) {
        failures += ir1_ea_test_fail("first native span class after cross-page decode");
    }
    if (!tb_harness_resolve_x86_fault(
            &map, native_pc, native_pc + map.spans[0].native_end_off,
            0, &x86_pc, &ea_seg_class) ||
        x86_pc != pc + 3 || ea_seg_class != IR1_EA_SEG_SS) {
        failures += ir1_ea_test_fail("second native span class after cross-page decode");
    }
    {
        uint64_t gpr[16] = { 0 };

        gpr[0] = UINT64_C(0x1000);
        gpr[4] = UINT64_C(0x3000);
        if (!tb_harness_refine_x86_fault_ea(
                &map, native_pc,
                native_pc + map.spans[1].native_end_off,
                UINT64_C(0x0000800000001000), gpr,
                UINT64_C(0x0000800000000000), 0, &ea_seg_class) ||
            ea_seg_class != IR1_EA_SEG_NOT_SS) {
            failures += ir1_ea_test_fail(
                "mixed FS:PUSH [RAX] BADV includes FS base");
        }

        for (uint16_t span_index = 2; span_index < map.count; span_index++) {
            uint64_t mixed_era =
                native_pc + map.spans[span_index - 1].native_end_off;

            gpr[0] = UINT64_C(0x0000800000000000);
            gpr[4] = UINT64_C(0x1000);
            if (!tb_harness_refine_x86_fault_ea(
                    &map, native_pc, mixed_era,
                    UINT64_C(0x0000800000000000), gpr, 0, 0,
                    &ea_seg_class) ||
                ea_seg_class != IR1_EA_SEG_NOT_SS) {
                failures += ir1_ea_test_fail(
                    "mixed stack/data instruction data BADV resolves to #GP");
            }

            gpr[0] = UINT64_C(0x1000);
            gpr[4] = UINT64_C(0x0000800000000008);
            if (!tb_harness_refine_x86_fault_ea(
                    &map, native_pc, mixed_era,
                    UINT64_C(0x0000800000000000), gpr, 0, 0,
                    &ea_seg_class) ||
                ea_seg_class != IR1_EA_SEG_SS) {
                failures += ir1_ea_test_fail(
                    "mixed stack/data instruction stack BADV resolves to #SS");
            }

            gpr[0] = UINT64_C(0x0000800000000000);
            gpr[4] = UINT64_C(0x0000800000000008);
            if (tb_harness_refine_x86_fault_ea(
                    &map, native_pc, mixed_era,
                    UINT64_C(0x0000800000000000), gpr, 0, 0,
                    &ea_seg_class) ||
                ea_seg_class != IR1_EA_SEG_UNKNOWN) {
                failures += ir1_ea_test_fail(
                    "overlapping stack/data BADV remains UNKNOWN");
            }
        }
    }
    return failures;
}

int tb_harness_test_ir1_ea_classification(uint8_t *native_code,
                                          uint64_t native_limit)
{
    CPUX86State *env;
    int failures = 0;

    if (lsenv == NULL || lsenv->cpu_state == NULL) {
        return ir1_ea_test_fail("missing x86 environment");
    }
    env = (CPUX86State *)lsenv->cpu_state;
    failures += test_ir1_ea_vectors(env);
    failures += test_unknown_span_class();
    failures += test_cross_page_decode(native_code, native_limit);
    failures += test_effective_segment_override(env);
    return failures;
}

