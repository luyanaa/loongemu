/*
 * x86-64 guest page-table walker.
 *
 * The translator's memory path is direct mapped: it hands the guest virtual
 * address to the host as an address.  That is fine for a bring-up harness,
 * but it means the tables the guest builds are never consulted -- a guest
 * page marked read-only is still writable, and an unmapped page is still
 * reachable.  This walker is the read side of fixing that: it turns a guest
 * virtual address into a guest physical address through the guest's own
 * four-level tables, and reports the leaf permissions and the fault the
 * hardware would raise.
 *
 * It is deliberately free of policy: nothing here installs a mapping, touches
 * a TLB or injects an exception.  A caller that wants shadow paging uses the
 * translation to build its own entry, and a caller that wants to deliver a
 * #PF uses latx_x86_fault_error_code() for the stack slot.
 *
 * Table memory is read through latx_platform_phys_read(), so a board can put
 * guest RAM anywhere; the identity harness maps it one-to-one.
 */
#ifndef LATX_X86_PAGING_H
#define LATX_X86_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Access being attempted; the bits mirror the #PF error code for W/R and I/D. */
#define LATX_X86_ACCESS_READ   (1u << 0)
#define LATX_X86_ACCESS_WRITE  (1u << 1)
#define LATX_X86_ACCESS_EXEC   (1u << 2)
#define LATX_X86_ACCESS_USER   (1u << 3)

typedef enum LatxX86WalkResult {
    LATX_X86_WALK_OK = 0,
    LATX_X86_WALK_NOT_PRESENT,   /* entry not present */
    LATX_X86_WALK_PROTECTION,    /* present, but this access is not allowed */
    LATX_X86_WALK_RESERVED,      /* malformed entry (PS at the wrong level, NX
                                  * without EFER.NXE, LA57) */
    LATX_X86_WALK_UNSUPPORTED,   /* no long-mode paging configured */
    LATX_X86_WALK_PAGING_OFF,    /* CR0.PG clear: gpa == va */
} LatxX86WalkResult;

typedef struct LatxX86Translation {
    uint64_t gpa;        /* guest physical address of the access */
    uint64_t pte;        /* the leaf entry that produced it */
    uint32_t page_bits;  /* 12, 21 or 30 */
    uint32_t perm;       /* the same bits as the access mask */
    uint32_t fault_error_code; /* authoritative #PF error code from x86_64_walk_page_tables */
    uint8_t  global;     /* leaf G=1 && CR4.PGE */
    uint8_t  pkey;       /* leaf protection key [62:59] */
    uint8_t  dirty;      /* leaf D=1 */
} LatxX86Translation;

struct CPUX86State;

/*
 * Walk one address.  `access` is a mask of LATX_X86_ACCESS_*; USER selects
 * the ring-3 rules (US bit required), otherwise the supervisor rules apply
 * and CR0.WP decides whether a write to a read-only page faults.
 */
LatxX86WalkResult latx_x86_walk(const struct CPUX86State *env, uint64_t va,
                                uint32_t access, LatxX86Translation *out);

/* The value that belongs in the #PF error-code slot for this walk result. */
uint32_t latx_x86_fault_error_code(const struct CPUX86State *env,
                                   LatxX86WalkResult result, uint32_t access);

/*
 * Cached walks.
 *
 * A page-granular cache keyed by (cr3, pcid, leaf index) delegates every miss
 * and dirty-bit promotion to the single authoritative x86_64_walk_page_tables().
 */
#define LATX_X86_TLB_ENTRIES 64

void latx_x86_tlb_flush_all(void);
void latx_x86_tlb_flush_non_global(void);
void latx_x86_tlb_flush_pcid(uint16_t pcid);
void latx_x86_tlb_flush_page(uint64_t va);
void latx_x86_tlb_flush_all_cpus(void);
uint32_t latx_platform_cpu_id(void);
LatxX86WalkResult latx_x86_walk_cached(const struct CPUX86State *env,
                                       uint64_t va, uint32_t access,
                                       LatxX86Translation *out);

/*
 * Checked physical memory hooks used by the authoritative page-table walker.
 * Return 0 on success, non-zero if the GPA is unmapped, MMIO, ROM, or
 * VMM-protected reserved memory.
 */
int latx_platform_phys_read_u64_checked(uint64_t pa, uint64_t *value);
int latx_platform_phys_atomic_or_u64(uint64_t pa, uint64_t bits,
                                     uint64_t *new_value);
int latx_platform_phys_cmpxchg_u64(uint64_t pa, uint64_t *expected,
                                   uint64_t desired);

/*
 * Validate one guest access.  `gva` is the first byte, `size_bits` the access
 * width; every page the access touches is checked, so a store crossing a
 * page boundary is not approved on the strength of its first page.
 *
 * Returns 0 when the access may proceed.  Otherwise it records the fault the
 * guest should take -- CR2, exception_index and the error code -- and returns
 * non-zero, leaving the caller to leave the block so the instruction restarts.
 */
/*
 * Validate one guest access and return the host address to use for it.
 *
 * The translated address is the return value, not a side channel: that keeps
 * it scoped to the instruction that asked, and a refusal is unambiguous
 * because a valid host address never has the high bit set.  On refusal the
 * fault is recorded in the environment (CR2, exception index, error code) and
 * the caller leaves the block.
 */
#define LATX_X86_ACCESS_FAULTED UINT64_C(0x8000000000000000)

uint64_t latx_x86_check_guest_access(struct CPUX86State *env, uint64_t gva,
                                     uint32_t access, uint32_t size_bits,
                                     uint64_t insn_pc);

/*
 * The host address of the last approved access, returned through a slot the
 * caller owns.
 *
 * A checked access is emitted as: validate, then dereference the translated
 * address instead of the guest virtual one.  The address is written to the
 * caller's slot rather than to a global, because a global survives the
 * instruction: the handler's own stores would then dereference whatever the
 * previous access published.  The slot lives in this translator's scratch
 * space, one per emitted check, and is dead once the instruction is done.
 */
extern uint64_t latx_x86_access_hpa;

/*
 * Set by the translator to the operand it just validated, cleared right
 * after.  Translation-time identity, run-time address: the dereference of
 * exactly this operand reads latx_x86_access_hpa.
 */
struct dt_cs_x86_op;
extern struct dt_cs_x86_op *latx_x86_checked_operand;

/* The host register holding the translated address of that operand. */
struct IR2_OPND;
extern struct IR2_OPND latx_x86_checked_addr;

/*
 * The guest virtual address of the access that was just approved, or -1.
 *
 * The shared load/store paths substitute the published host address when the
 * operand they are asked to convert resolves to exactly this address.  An
 * address comparison rather than an operand pointer is what keeps this honest:
 * the translator builds several IR1 operands for one instruction (a descriptor
 * fetched at operand+2, the stack slot a push synthesizes), and only the one
 * whose address was validated may be redirected.
 */
extern uint64_t latx_x86_access_hpa;

/*
 * Set by the translator to the operand it just validated, cleared right
 * after.  Translation-time identity, run-time address: the dereference of
 * exactly this operand reads latx_x86_access_hpa.
 */
struct dt_cs_x86_op;
extern struct dt_cs_x86_op *latx_x86_checked_operand;

/* The host register holding the translated address of that operand. */
struct IR2_OPND;
extern struct IR2_OPND latx_x86_checked_addr;

/*
 * Deliver the fault the last check recorded to the guest's own #PF handler.
 *
 * The frame is built the way the architecture describes it -- the faulting
 * RIP, the interrupted CS, RFLAGS and the error code on the guest stack, with
 * CS:RIP taken from IDT[14] -- and CS base stays zero because long mode
 * ignores segment bases.  Returns 0 when a handler was entered, non-zero when
 * the IDT has no usable gate for vector 14 (a triple fault for the caller to
 * report).
 */
int latx_x86_deliver_fault(struct CPUX86State *env, uint32_t vector);
int latx_x86_get_tss_stack(const struct CPUX86State *env, uint32_t ist,
                           uint64_t *out_rsp);
void helper_syscall_sys(struct CPUX86State *env, uint64_t next_rip);
void helper_sysret_sys(struct CPUX86State *env);

/*
 * Instruction fetch.
 *
 * Decode and execution must agree on what a guest virtual address means, so
 * this is the single place a fetched byte comes from: with paging enabled it
 * walks the guest's tables with LATX_X86_ACCESS_EXEC and reads the translated
 * physical byte; otherwise it reads the flat guest image.
 *
 * A refusal records the fault exactly as a data refusal does (CR2, exception
 * index, error code) and returns 0xCC, so a caller that decodes the returned
 * bytes sees an int3 rather than silently executing whatever happened to be
 * behind an unmapped page.
 */
uint8_t latx_x86_fetch_byte(const struct CPUX86State *env, uint64_t gva);

/*
 * Physical-memory read used for the tables themselves.  Weak default reads
 * the identity mapping; a board overrides it when guest RAM lives elsewhere.
 */
uint64_t latx_platform_phys_read(uint64_t pa, int size);

#ifdef __cplusplus
}
#endif

#endif /* LATX_X86_PAGING_H */
