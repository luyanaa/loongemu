# IR2 ValueVersion / Next-use: design

Status: the load-elision half already exists; the design is about the parts that
do not, and about keeping recovery exact.

## 1. What already exists (do not rebuild)

LATR and LATM both keep guest GPR/FPR values resident in itemp/ftemp slots:

* `LatrRegCacheEntry { int8_t temp; uint8_t valid, dirty; uint64_t age; }`
  (`target/riscv/latr/include/latr.h:68`), array of 32 per bank plus
  `itemp_borrows[6]` / `ftemp_borrows[6]` (`latr.h:75`).
* `alloc_gpr_src` returns the resident host register when `entry->valid` and only
  falls back to `la_ld_d(env->gpr[reg])` on a miss (`latr.c:689`). So a redundant
  *load* of a still-resident guest register is already avoided.
* `store_gpr_dst` -> `regcache_store` marks the entry dirty instead of storing to
  `env->gpr[]` (`latr.c:469`); writeback happens lazily in
  `latr_regcache_flush()` (`latr.c:400`).
* Control-flow edges flush explicitly, *not* through wrappers. `latr.h:280-300`
  defines `latr_regcache_label/jirl/b/...` which flush and then emit, but a
  `grep` shows **no caller**: the translator emits raw `la_b`/`(la_label)` (14 in
  `translate-riscv.c`, 3 in `latr.c`, 10 in `translate-riscv-csr.c`) and calls
  `latr_regcache_flush()` by hand at 10 places (4 + 5 + 1). Sites that emit a
  branch with no dirty guest state - e.g. the CSR.SATP legalisation in
  `translate-riscv-csr.c:600-620`, which only writes `env->csr[]` - correctly skip
  the flush. So the real discipline is "flush where guest state is live", and the
  wrappers are dead code.
* LATM mirrors this (`latm_regcache_*`, `latm.h:230`) and additionally exports
  `latm_regcache_snapshot_gpr_binds()` for recovery spans (`latm.h:168`).

Consequences for the design: "version" cannot mean "avoid the load" (done); it
must mean "know whether `env` already holds this version", and "next-use" cannot
mean "avoid the writeback of a dead register" in the general case (every guest
register is live out of a TB). The real opportunities are narrower and must be
stated as such.

## 2. Measured cost model (to be confirmed in P0)

For a straight-line block with `B` branches and `L` labels, the current shape is
`2 x (B + L)` flushes, each writing back up to 32 dirty entries. Three costs
follow:

1. **Redundant writebacks**: a register written back at a branch, then reloaded
   and written back again at a later label, is written twice for one value.
2. **Redundant flushes**: a label reached only by fallthrough needs no
   canonicalisation at all.
3. **Reload-after-flush**: the entry stays valid across flush, so no reload; the
   cost is only the store. (This is why 3.1 in the LATA note - "the flag
   consumers read 0" - is not a regcache symptom.)

## 3. Design

### 3.1 Versioned entries (shared header, per-port state)

New header `shared/backend/loongarch/include/ir2-value-state.h`:

```c
typedef struct Ir2GuestValueState {
    uint32_t version[32];      /* bumped on every write to the value */
    uint32_t env_version[32];  /* version currently mirrored in env */
    uint8_t  resident[32];     /* temp slot index, or 0xff */
    uint8_t  dirty[32];
} Ir2GuestValueState;

/* Called by the port's regcache on store/flush/writeback. */
void ir2_value_written(Ir2GuestValueState *s, int reg);
bool ir2_value_needs_writeback(const Ir2GuestValueState *s, int reg);
void ir2_value_flushed(Ir2GuestValueState *s, int reg);
```

`dirty` becomes derived (`version != env_version`), which makes double writeback
of one value impossible by construction rather than by discipline. This is the
part worth sharing: LATR and LATM get identical semantics from one file, and a
host unit test can drive the state machine without a translator.

### 3.2 Flush elision at labels (structural, not speculative)

At `latr_regcache_label(L)`: skip the flush when the label has exactly one
predecessor and that predecessor is the immediately preceding fallthrough with
no store, helper call, memory access or guest-state observation between them.
This is decidable at translate time by looking *backwards* in the IR2 list from
the label to the previous boundary, because the port emits linearly.

- Invariant: the flush may be skipped only if `ir2_value_needs_writeback()` is
  false for every register, which the version table answers directly.
- The predecessor count for a label is known at emission: the port already keeps
  `tb_insn_label[]` (`latr.h:108`), consumed when it resolves labels at
  `latr.c:1139-1146`. No new analysis pass is needed.

### 3.3 Next-use for writeback ordering, not elimination

Where a writeback set must be emitted anyway (TB exit, helper call, fault
point), order and fusing matter but elimination does not: the successor is not
known. The design therefore uses next-use only for **load placement**: when a
value is needed in a successor block that provably starts at the fallthrough
address, the load can be hoisted into the predecessor before the edge. This is
P3 and only after P1/P2 show a measurable win; it changes no observable state.

## 4. Recovery and the one hard rule

`LeRecoverySpan` (`shared/backend/loongarch/include/le-recovery.h`) binds guest
registers to host temporaries so a fault mid-block can be classified. The
regcache is therefore *not* an implementation detail: it is evidence.

**Hard rule: no value-state change may make a span's binding stale.** Concretely:

1. A span that binds a temp must be emitted only while that temp owns the
   register (`regcache_borrow`), and the binding must be captured by the port's
   snapshot function at the same point (LATM: `latm_regcache_snapshot_gpr_binds`;
   LATR: `tb_insn_binds[]` staging in `TRANSLATION_DATA`, `latr.h:114`).
2. Version-based writeback elision must never change *when* env becomes
   canonical relative to a span with `phase == LE_PHASE_PARTIAL_EFFECT`; if it
   does, the span must be dropped (mark unusable) rather than left claiming a
   value that env does not hold.
3. Fail closed exactly as `latr_machine_host_fault` does (`latr_machine.c`):
   a span with bindings and no usable snapshot returns 0 instead of reporting a
   partial state.

## 5. Phases

| phase | change | gate |
|---|---|---|
| P0 | counters only: flushes, writebacks, per-reg writeback count, reload misses | `latr-isa` + boot unchanged; numbers recorded in the commit |
| P1 | `ir2-value-state.h` + LATR wiring, `dirty` derived from versions | `latr-isa 1607`, boot smoke, host unit test |
| P2 | label flush elision (3.2) | same, plus counters show flush count down and no regression in boot |
| P3 | load hoisting (3.3), only if P0-P2 data justifies it | same |
| P4 | LATM wiring of the same shared header | `latm-isa 60`, board smoke |

## 6. Failure modes to design against

1. **Stale resident value**: a helper writes `env->gpr[x]` directly (e.g. the SBI
   call path writes `env->gpr[10]`/`[11]` at `latr.c:668-669`), leaving the regcache
   thinking it owns a newer value. Every such site must flush first; the version
   table makes the bug *detectable* (compare `version` against a
   `direct_write_version` bump) instead of silent.
2. **Double writeback**: eliminated by construction in 3.1.
3. **Bindings after elision**: rule 4.2.
4. **Pool exhaustion**: elision reduces stores, never increases live temps; the
   six-itemp limit (`reg-alloc.c:365-366`) stays the bound.
5. **Undisciplined edges**: because the flush is called by hand, any new
   translator path that emits a branch while a guest register is dirty is a
   latent stale-state bug, and nothing in the build catches it. Two options:
   adopt the existing wrappers (delete nothing, make the rule structural) or
   delete them and add a review checklist item. Either way this must be decided
   before P2 adds more branch-shape decisions; leaving 10 hand-placed flushes and
   4 unused wrappers is the worst of both.

## 7. Verification

* `tests/test_ir2_value_state.c` (host, new): version/writeback state machine
  including "written twice, flushed once", "written, flushed, written",
  "direct env write bump".
* `tests/test_latr_regcache*` (existing, extend): flush-at-label adjacency cases.
* ISA gates: `latr-isa` 1607, `latm-isa` 60, boot smoke; PERF counters before
  and after on the same workload.

## 8. Self-review

**Objection 1: this is a 5-phase plan for an optimisation with no measured
baseline; the roadmap called it "value version / next-use" as if it were the
core win, but the load half already exists.**
Correct, and the design says so in §1 rather than pretending otherwise. P0 is
instrumentation only and may well show the win is < 1%, in which case P2/P3
should be dropped and only the shared state header (which buys the LATM parity
and the double-writeback proof) is worth keeping. That is a legitimate outcome
and is written into the table as a decision point.

**Objection 2: label flush elision is unsafe if a branch inside the block
targets that label.**
Yes. §3.2 restricts it to labels with a single fallthrough predecessor, which is
decidable from `tb_insn_label[]`. Anything else flushes. The restriction is
deliberately syntactic: it does not require a dataflow fixpoint, which is what
would make this expensive and bug-prone in a single-pass translator.

**Objection 3: versions as `uint32_t` can wrap.**
Not a correctness issue: comparison is equality of two counters that advance
together per write to the same register; wrap needs 2^32 writes to one register
within one TB. Cheap to bound instead: reset per TB (`tr_fini` already resets
translation data), so the counters are per-block.

**Objection 4: does the shared header leak into the backends' ABI?**
No: it is a header-only struct plus inline-free functions with a plain C ABI,
no IR2 or QEMU types, so host tests link it directly (same pattern as
`le-recovery.h` and `tb_cache.h`).

**Open question:** whether LATM's `snapshot_gpr_binds` should be lifted into the
shared header (both ports need the same "which guest regs are bound to which
temps" query). Lean yes, but only after LATR gains the same call shape - doing it
now would invent an abstraction with a single implementation.
