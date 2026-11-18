#ifndef LOONGEMU_LOONGBT_H
#define LOONGEMU_LOONGBT_H
/*
 * Loongson Binary Translation (LBT) extension: portable implementation.
 *
 * Two back ends share one contract:
 *   - hardware: the real LBT instructions, compiled with the assembler
 *     extension enabled (LoongArch 3A6000-class cores);
 *   - fallback: the same semantics computed in integer C, for cores without
 *     LBT (e.g. 2K1000LA / 2K2000).
 * The active back end is chosen from cpu_flag bits carried in
 * LoongemuCpuFeatures; see loongbt_set_cpu_features().
 *
 * Reference: LoongArch Reference Manual Vol.1 LBT chapter, cross-checked with
 * the per-instruction semantics published in the LoongArch intrinsics guide.
 * Hardware execution is not verified here; that needs a 3A6000-class machine.
 */
#include <stdint.h>
#include <stdbool.h>

#define LOONGEMU_CPU_LBT_X86   (UINT32_C(1) << 0)  /* x86 LBT subset          */
#define LOONGEMU_CPU_LBT_ARM   (UINT32_C(1) << 1)  /* ARM LBT subset          */
#define LOONGEMU_CPU_LSX       (UINT32_C(1) << 8)
#define LOONGEMU_CPU_LASX      (UINT32_C(1) << 9)

/* EFLAGS bits as held in LBT4 low word. */
#define LOONGBT_EF_CF (UINT32_C(1) << 0)
#define LOONGBT_EF_PF (UINT32_C(1) << 2)
#define LOONGBT_EF_AF (UINT32_C(1) << 4)
#define LOONGBT_EF_ZF (UINT32_C(1) << 6)
#define LOONGBT_EF_SF (UINT32_C(1) << 7)
#define LOONGBT_EF_OF (UINT32_C(1) << 11)
#define LOONGBT_EF_ALL (LOONGBT_EF_CF | LOONGBT_EF_PF | LOONGBT_EF_AF | \
                        LOONGBT_EF_ZF | LOONGBT_EF_SF | LOONGBT_EF_OF)

typedef enum LoongbtWidth {
    LOONGBT_B = 8,
    LOONGBT_H = 16,
    LOONGBT_W = 32,
    LOONGBT_D = 64,
} LoongbtWidth;

typedef enum LoongbtOp {
    LOONGBT_ADD,     /* x86add, sets CF AF OF PF ZF SF */
    LOONGBT_ADDU,    /* x86add.wu/.du: like ADD with OF forced 0 */
    LOONGBT_ADC,     /* x86adc, adds carry-in                   */
    LOONGBT_SUB,
    LOONGBT_SUBU,    /* x86sub.wu/.du: like SUB with OF forced 0 */
    LOONGBT_SBC,
    LOONGBT_AND,
    LOONGBT_OR,
    LOONGBT_XOR,
    LOONGBT_INC,     /* preserves CF                            */
    LOONGBT_DEC,
    LOONGBT_MUL,     /* signed overflow -> CF=OF                */
    LOONGBT_MULU,    /* x86mul.bu/hu/wu/du                      */
    LOONGBT_ROTL,
    LOONGBT_ROTLI,
    LOONGBT_ROTR,
    LOONGBT_ROTRI,
    LOONGBT_RCL,
    LOONGBT_RCLI,
    LOONGBT_RCR,
    LOONGBT_RCRI,
    LOONGBT_SLL,
    LOONGBT_SLLI,
    LOONGBT_SRL,
    LOONGBT_SRLI,
    LOONGBT_SRA,
    LOONGBT_SRAI,
    LOONGBT_MTFLAG,  /* x86mtflag rd, mask: EFLAGS <- rd bits   */
    LOONGBT_MFFLAG,  /* x86mfflag rd, mask: rd <- EFLAGS bits   */
    LOONGBT_MTTOP,
    LOONGBT_MFTOP,
    LOONGBT_INCTOP,
    LOONGBT_DECTOP,
    LOONGBT_SETTM,
    LOONGBT_CLRTM,
    LOONGBT_SETX86J,
    LOONGBT_LOOPE,
    LOONGBT_LOOPNE,
    LOONGBT__COUNT
} LoongbtOp;

/* Architectural LBT state subset: four scratch registers plus LBT4
 * (low word = x86 EFLAGS, high word = x87 FTOP).  TM is the FPR remap bit. */
typedef struct LoongbtState {
    uint64_t scr[4];
    uint32_t eflags;
    uint32_t top;      /* x87 TOP, 3 bits */
    bool tm;           /* x86settm / x86clrtm */
} LoongbtState;

typedef struct LoongemuCpuFeatures {
    uint32_t flags;
} LoongemuCpuFeatures;

/* Back-end selection. */
void loongbt_set_cpu_features(uint32_t flags);
uint32_t loongbt_cpu_features(void);
bool loongbt_hw_available(void);      /* true on a core that has LBT */
void loongbt_set_hw_available(bool available);

/* Execute one LBT operation.  Arithmetic ops return the architectural result
 * (the caller writes it back); flag-only ops return 0 and mutate the flags in
 * `st`.  `b` is the source operand or shift count / flag mask. */
uint64_t loongbt_exec(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                      uint64_t a, uint64_t b);

/* Accumulate the architectural flags of `st` into a GPR image, using the
 * x86mfflag bit packing (mask bit 0..5 selects CF,PF,AF,ZF,SF,OF). */
uint64_t loongbt_pack_flags(uint32_t eflags, uint8_t mask);
void loongbt_unpack_flags(uint32_t *eflags, uint64_t value, uint8_t mask);

/* Hardware back end.  Compiled for LoongArch with `-mattr=+lbt`; the wrappers
 * use the real instructions and are what a 3A6000-class core runs.  They are
 * declared for every target so the dispatcher can compile everywhere. */
uint64_t loongbt_hw_exec(LoongbtState *st, LoongbtOp op, LoongbtWidth width,
                         uint64_t a, uint64_t b);
bool loongbt_hw_probe(void);

/* IR2 integration: map a generated LISA_* x86 opcode onto the back ends.
 * Returns false for opcodes the portable core does not model (x86settag and
 * the x86_inst marker). */
bool loongbt_op_from_ir2(uint32_t lisa_opcode, LoongbtOp *op, LoongbtWidth *width);
uint64_t loongbt_exec_ir2(LoongbtState *st, uint32_t lisa_opcode,
                          uint64_t a, uint64_t b, bool *supported);

#endif
