/*
 * The LBT-opcode classifier, and the one property that keeps it honest.
 *
 * tests 1-4 are ordinary unit checks: the classifier must answer 1 for a
 * representative member of each LBT family and 0 for base LoongArch opcodes,
 * so a broken switch cannot pass by answering "no LBT" to everything.
 *
 * test 5 is the drift check. ir2-lbt-opcodes.h is generated from ir2-opcode.h,
 * and the failure it prevents is silent: a newly added LBT instruction that the
 * classifier does not know about is exactly the instruction a software-path
 * image would then leak into a qemu that cannot decode it. So the test reads
 * both files and compares the name sets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir2-lbt-opcodes.h"

#ifndef IR2_OPCODE_HEADER
#define IR2_OPCODE_HEADER "shared/backend/loongarch/include/ir2-opcode.h"
#endif
#ifndef IR2_LBT_HEADER
#define IR2_LBT_HEADER "shared/backend/loongarch/include/ir2-lbt-opcodes.h"
#endif

static int checks;
static int fails;

static void check(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s: got=%d want=%d\n", what, got, want);
    }
}

/* Names from `text` that look like LBT instructions. */
static int collect_lbt_names(const char *text, char names[][64], int max)
{
    int count = 0;
    const char *p = text;

    while (*p != '\0' && count < max) {
        if (strncmp(p, "LISA_", 5) == 0) {
            size_t len = 5;

            while (p[len] != '\0' &&
                   ((p[len] >= 'A' && p[len] <= 'Z') ||
                    (p[len] >= '0' && p[len] <= '9') || p[len] == '_')) {
                len++;
            }
            int is_lbt = (len >= 9 &&
                          (strncmp(p, "LISA_ARM", 8) == 0 ||
                           strncmp(p, "LISA_X86", 8) == 0)) ||
                         (len == 9 && strncmp(p, "LISA_GR2SCR", 11) == 0) ||
                         (len == 9 && strncmp(p, "LISA_SCR2GR", 11) == 0) ||
                         (len == 11 && strncmp(p, "LISA_JISCR0", 11) == 0) ||
                         (len == 11 && strncmp(p, "LISA_JISCR1", 11) == 0) ||
                         (len == 12 && strncmp(p, "LISA_SETARMJ", 12) == 0);
            if (is_lbt && len < 63) {
                memcpy(names[count], p, len);
                names[count][len] = '\0';
                count++;
            }
            p += len;
            continue;
        }
        p++;
    }
    return count;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = calloc(1, (size_t)size + 1);
    if (buf != NULL && size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

int main(void)
{
    /* Representative members of each LBT family. */
    check(ir2_opcode_is_lbt(LISA_ARMMTFLAG), 1, "ARMMTFLAG is LBT");
    check(ir2_opcode_is_lbt(LISA_ARMMOVE), 1, "ARMMOVE is LBT");
    check(ir2_opcode_is_lbt(LISA_X86ADD_D), 1, "X86ADD_D is LBT");
    check(ir2_opcode_is_lbt(LISA_X86AND_D), 1, "X86AND_D is LBT");
    check(ir2_opcode_is_lbt(LISA_GR2SCR), 1, "GR2SCR is LBT");
    check(ir2_opcode_is_lbt(LISA_JISCR0), 1, "JISCR0 is LBT");
    check(ir2_opcode_is_lbt(LISA_SETARMJ), 1, "SETARMJ is LBT");

    /* Base LoongArch must stay out: a classifier that answers 1 everywhere
     * would make the software-path assertion fail on every image. */
    check(ir2_opcode_is_lbt(LISA_ADD_D), 0, "ADD_D is not LBT");
    check(ir2_opcode_is_lbt(LISA_LD_D), 0, "LD_D is not LBT");
    check(ir2_opcode_is_lbt(LISA_BSTRINS_D), 0, "BSTRINS_D is not LBT");
    check(ir2_opcode_is_lbt(LISA_LABEL), 0, "LABEL is not LBT");

    check((int)IR2_LBT_OPCODE_COUNT > 100, 1, "classifier covers a family-scale set");

    /* Drift check: generated table vs the enum it is generated from. */
    {
        static char enum_names[256][64];
        static char gen_names[256][64];
        char *enum_text = slurp(IR2_OPCODE_HEADER);
        char *gen_text = slurp(IR2_LBT_HEADER);
        int ok = 1;

        if (enum_text == NULL || gen_text == NULL) {
            fails++;
            checks++;
            printf("FAIL drift check: cannot read %s / %s\n", IR2_OPCODE_HEADER,
                   IR2_LBT_HEADER);
        } else {
            int enum_count = collect_lbt_names(enum_text, enum_names, 256);
            int gen_count = collect_lbt_names(gen_text, gen_names, 256);
            int missing = 0;

            for (int i = 0; i < enum_count; i++) {
                int found = 0;

                for (int j = 0; j < gen_count; j++) {
                    if (strcmp(enum_names[i], gen_names[j]) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    missing++;
                    printf("FAIL drift check: %s missing from the generated table\n",
                           enum_names[i]);
                }
            }
            checks++;
            if (missing != 0 || gen_count != enum_count) {
                fails++;
                printf("FAIL drift check: enum=%d generated=%d\n", enum_count,
                       gen_count);
            }
            ok = missing == 0 && gen_count == enum_count;
            (void)ok;
        }
        free(enum_text);
        free(gen_text);
    }

    printf("ir2-lbt-opcodes: %d checks, %d fails\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
