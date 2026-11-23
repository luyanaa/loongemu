#ifndef LATA_MEM_H
#define LATA_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "target/arm/cpu.h"

/*
 * Unified guest memory contract shared by scalar, NEON, and SVE paths.
 *
 * `probe()` / `load()` / `store()` are the checked path: they return an
 * architectural status so precise synchronous Data Aborts or suppressed FFR
 * updates stay exact. Everything that is not positively admitted for inline
 * host access goes through them.
 *
 * Inline direct access is a separate, explicitly claimed capability. A span
 * [vaddr, vaddr+size) may be lowered to raw host loads/stores only when the
 * provider publishes a `LataGuestMemDirectPolicy` that can prove all three of:
 *
 *   mapping      every accepted span is backed by a host-accessible mapping,
 *                not a device/MMIO window, and the host bytes are the guest
 *                bytes for that span;
 *   permission   a write span is only accepted when the guest may write it,
 *                and the proof is against the *current* translation regime;
 *   invalidation `epoch()` changes whenever any mapping or permission change
 *                could invalidate a span that was previously accepted.
 *
 * LATA never trusts the state observed at translation time: emitted code
 * carries the epoch it was translated under and calls
 * lata_guest_mem_direct_admit() before every direct span. A changed epoch, a
 * rejected span, or a provider that publishes no policy drops that access to
 * the checked path. The default provider publishes no policy, so no inline
 * direct access is reachable at all.
 */
typedef struct LataGuestMemDirectPolicy {
    /* Diagnostic name; never NULL for a policy that is actually installed. */
    const char *name;
    /* Monotonic mapping/permission generation. */
    uint64_t (*epoch)(CPUARMState *env);
    /* Per-span proof of mapping + permission; must be cheap and side-effect
     * free (it runs on the hot path when a policy is installed). */
    bool (*span_admissible)(CPUARMState *env, uint64_t vaddr, uint64_t size,
                            bool is_write);
} LataGuestMemDirectPolicy;
typedef enum {
    LATA_MEM_OK = 0,
    LATA_MEM_FAULT_TRANSLATION = 1,
    LATA_MEM_FAULT_PERMISSION = 2,
    LATA_MEM_FAULT_ALIGNMENT = 3,
    LATA_MEM_FAULT_UNMAPPED = 4,
} LataGuestMemStatus;

#define LATA_MEM_PERM_NONE 0u
#define LATA_MEM_PERM_READ (1u << 0)
#define LATA_MEM_PERM_WRITE (1u << 1)
#define LATA_MEM_PERM_RW (LATA_MEM_PERM_READ | LATA_MEM_PERM_WRITE)

typedef struct LataGuestMemOps {
    LataGuestMemStatus (*probe)(CPUARMState *env, uint64_t vaddr,
                                size_t size, bool is_write, bool clean_tbi);
    LataGuestMemStatus (*load)(CPUARMState *env, uint64_t vaddr,
                               size_t size, bool is_signed,
                               uint64_t *out_value);
    LataGuestMemStatus (*store)(CPUARMState *env, uint64_t vaddr,
                                size_t size, uint64_t value);
    /*
     * NULL means "this provider cannot prove inline host access"; every
     * access is then lowered through probe/load/store.
     */
    const LataGuestMemDirectPolicy *direct;
    /*
     * Byte-buffer transfer for spans wider than 8 bytes (SIMD/vector). NULL
     * means the lowering must split the span into 8-byte provider calls.
     */
    LataGuestMemStatus (*load_bytes)(CPUARMState *env, uint64_t vaddr,
                                     void *dst, size_t size);
    LataGuestMemStatus (*store_bytes)(CPUARMState *env, uint64_t vaddr,
                                      const void *src, size_t size);
    /*
     * Declared environment property, not a probe result: the loader maps every
     * guest VA at the same host address and applies no permission model of its
     * own.  Only the bare-metal harness may claim this; a custom provider
     * leaves it false and its accesses go through load/store instead.
     */
    bool va_is_hpa_identity;
} LataGuestMemOps;

extern const LataGuestMemOps lata_default_guest_mem_ops;

void lata_set_guest_mem_ops(const LataGuestMemOps *ops);
const LataGuestMemOps *lata_get_guest_mem_ops(void);

LataGuestMemStatus lata_guest_mem_probe(CPUARMState *env, uint64_t vaddr,
                                        size_t size, bool is_write,
                                        bool clean_tbi);
LataGuestMemStatus lata_guest_mem_load(CPUARMState *env, uint64_t vaddr,
                                       size_t size, bool is_signed,
                                       uint64_t *out_value);
LataGuestMemStatus lata_guest_mem_store(CPUARMState *env, uint64_t vaddr,
                                        size_t size, uint64_t value);

/* Direct-access contract. `available()` gates code generation: when it is
 * false the lowering must not emit an inline host access at all. */
bool lata_guest_mem_direct_available(void);
/* True when inline host access is justified (policy published, or the provider
 * declares the identity environment).  Scalar/NEON lowering must consult this
 * instead of comparing against a specific provider pointer. */
bool lata_guest_mem_inline_host_access_ok(void);
/* Epoch to embed in code generated now; 0 when no policy is installed. */
uint64_t lata_guest_mem_direct_epoch(void);
/* Runtime admission for one span. `translation_epoch` is the value observed
 * when the block was translated: any change re-routes the access to the
 * checked path instead of using a possibly stale mapping proof. */
bool lata_guest_mem_direct_admit(CPUARMState *env, uint64_t vaddr,
                                 uint64_t size, bool is_write,
                                 uint64_t translation_epoch);

/* Configure test/MMU fault windows on env->sve_ctrl. */
void lata_guest_mem_clear_fault_regions(CPUARMState *env);
bool lata_guest_mem_add_fault_region(CPUARMState *env, uint64_t base,
                                     uint64_t size, uint32_t perm);

/* Helper entry points invoked by generated IR2 (returning 0 on OK, else LataGuestMemStatus). */
uint64_t lata_guest_mem_load_checked(CPUARMState *env, uint64_t vaddr,
                                     uint32_t size_and_flags, uint64_t *out_val);
uint64_t lata_guest_mem_store_checked(CPUARMState *env, uint64_t vaddr,
                                      uint32_t size_and_flags, uint64_t val);

/*
 * Translate a LataGuestMemStatus into an AArch64 Data Fault Status Code.
 *
 * The internal enum values are an implementation detail: they are not ESR
 * encodings (DFSC 0b000010 is "address size fault, level 2", not "permission"),
 * so they must never be OR-ed into a syndrome word. Each architectural class
 * gets its own code:
 *
 *   LATA_MEM_FAULT_TRANSLATION / UNMAPPED -> translation fault, level 3 (0x07)
 *   LATA_MEM_FAULT_PERMISSION            -> permission fault, level 3 (0x0f)
 *   LATA_MEM_FAULT_ALIGNMENT             -> alignment fault          (0x21)
 *   anything else (including OK)         -> synchronous external abort (0x10)
 *
 * Level 3 is used for translation/permission faults because the generic guest
 * memory interface does not report the level at which the walk failed; the
 * final level is the only claim that cannot understate the fault.
 */
uint32_t lata_guest_mem_status_to_dfsc(uint32_t status);

/* Takes the architectural Data Abort: ESR.EC = 0x25 with the DFSC derived from
 * `status`, FAR/HPFAR-like state latched in env->sve_ctrl, and the guest PC set
 * to the faulting instruction. */
void lata_guest_mem_raise_fault(CPUARMState *env, uint64_t fault_pc,
                                  uint64_t vaddr, uint32_t is_write,
                                  uint32_t status);

#endif
