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

## 2. Measured cost model (P0, done)

`LatrRegCacheStats` counts flushes, writebacks, reloads, stores and evictions at
translation time; the LATR ISA image and the guest-boot harnesses print them.
Measured:

| workload | flushes | writebacks | reloads | stores | evictions |
|---|---|---|---|---|---|
| ISA image (1607 checks) | 12801 | 282 | 319 | 282 | 0 |
| guest boot (`latr-sys`) | 10022 | 605 | 776 | 605 | 0 |

Three conclusions, two of which retire parts of this design:

1. **There is no double writeback to remove.** `writebacks == stores` in both
   workloads: every guest-register write is written back exactly once. The
   `dirty` bit already achieves what §3.1's version counters were introduced to
   guarantee, so a `version`/`env_version` pair buys nothing here.
2. **Almost every flush is a no-op, and the cost is the scan.** 94-98% of
   `latr_regcache_flush()` calls find nothing dirty (282/12801, 605/10022). Each
   call still walks 64 entries at *translation* time. The cheap win is therefore
   not versioning but a per-bank dirty counter: if it is zero, return without
   scanning. That is ~10 lines and does not touch semantics.
3. **The pool is not under pressure.** `evictions == 0` in both workloads, so
   residency bookkeeping beyond the existing `valid`/`dirty`/`age` has no work
   to do here.

What survives: the flush fast path (2) and, if it still shows up after that, the
label-flush elision of §3.2. What does not: the version counters of §3.1, and the
"redundant writeback" motivation in the original cost model - both refuted by
measurement. This is the outcome P0 existed to produce.

Measured effect of the fast path (P1):

| workload | flushes | empty | entries visited per flush | writebacks |
|---|---|---|---|---|
| ISA image | 12801 | 12519 | 128 -> 1.9 | 282 (unchanged) |
| guest boot | 10022 | 9417 | 128 -> 4.9 | 605 (unchanged) |

So ~97% / ~92% of the scanning is gone, and the writeback count is bit-identical:
the fast path removes work, not semantics. What is left is per-flush call
overhead (a counter compare) plus the *number* of flush calls, which is what
§3.2's label elision would attack next - it is not yet justified by a second
measurement, so it stays a separate step.

## 3. Design

### 3.1 Versioned entries - dropped

The original proposal was a shared `Ir2GuestValueState` with `version[]` /
`env_version[]` so a writeback could be skipped when env already held the value.
P0 refuted the premise (`writebacks == stores`, so no value is ever written
twice), and the header would therefore have no work to do. Removed rather than
kept: a shared abstraction with no consumer is a maintenance cost paid for an
argument nobody can measure.

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

### 3.2b Step 4 measured and retired (dead writes and next-use at labels)

Before building anything, the two claims step 4 rests on were measured with
permanent counters in `LatrRegCacheStats` (`dead_stores`, `elided_flushes`,
`release_only_flushes`) plus a temporary per-site split (instrumentation removed
after the run; numbers recorded here):

| workload | flushes | elided | release-only | dead stores | writebacks | stores | reloads |
|---|---|---|---|---|---|---|---|
| ISA image (1770 checks) | 14268 | 13822 | 164 | 0 | 282 | 282 | 321 |
| guest boot | 10022 | 9078 | 339 | 0 | 605 | 605 | 776 |

Per-site split on the ISA image (writebacks/flushes): helper-call boundaries
282/13936, internal label 0/4, exit stub 0/1, block end 0/327.

What that says:

1. **Dead writes do not exist in these workloads.** `dead_stores == 0` means no
   guest register is written twice inside one translated block, so there is no
   value to eliminate. The mechanism is already right where it matters: when a
   guest register's dirty value is replaced by a newer write, `regcache_store`
   drops the old value without a writeback (`latr.c:667-700`), so a dead write
   can never cost a store. A future workload that does produce them still costs
   nothing beyond the instruction itself.
2. **Writebacks only happen where canonical state is mandatory.** All 282/605
   writebacks occur at helper-call flush points (memory/atomic/CSR helpers that
   may enter the guest trap path), and the label/exit/block-end flushes write
   nothing back - they only free temporaries or find the cache empty.
3. **The remaining flush cost is translator time, not emitted code.** 97%/91% of
   flush calls find nothing resident, and those already emitted nothing; the
   fast path now returns before the counter split and the release loops (the
   desync assertion still runs).

So step 4's regcache half is retired by measurement, the same way step 2 was.
Next-use in the sense of *hoisting a load into a predecessor edge* stays open by
design: it needs the successor block to be known, i.e. chaining (5.2), and §3.3
already says it changes no observable state.

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
2. The flush fast path may skip the *scan* only when no entry in the bank is
   dirty, which leaves the set of writebacks identical; anything that changes
   *when* env becomes canonical relative to a span with
   `phase == LE_PHASE_PARTIAL_EFFECT` must drop the span (mark unusable) rather
   than leave it claiming a value env does not hold.
3. Fail closed exactly as `latr_machine_host_fault` does (`latr_machine.c`):
   a span with bindings and no usable snapshot returns 0 instead of reporting a
   partial state.

## 5. Phases

| phase | change | gate |
|---|---|---|
| P0 | done: `LatrRegCacheStats` counters, printed by both LATR harnesses | `latr-isa 1607`, `latr-sys`, numbers in §2 |
| P1 | done: per-bank dirty and resident counts; `latr_regcache_flush` skips a bank's writeback scan when it is clean and its release loop when it is empty, with an invariant assert that both banks end empty and clean | `latr-isa 1607`, `latr-sys`; identical `writebacks` (282 / 605) prove semantics unchanged, and entries visited per flush fell from 128 to ~1.9 / ~4.9 |
| P2 | label flush elision (3.2), only if the counters still show flushes dominating after P1 | same |
| P3 | load hoisting (3.3), only if the data justifies it | same |
| P4 | LATM parity for the fast path (LATM has the same flush shape) | `latm-isa 60`, board smoke |

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

**Objection 1 (resolved by P0): is versioning worth building, given that the
load half already exists?**
No. P0 measured `writebacks == stores`, so the double-writeback case the version
counters were meant to prevent does not occur; and `evictions == 0`, so residency
tracking has nothing to track. The plan is now the flush fast path (a dirty
counter) plus, conditionally, label elision. The shared `ir2-value-state.h`
header in §3.1 is dropped rather than kept "for LATM parity" - a header with no
consumer is exactly the abstraction this measurement was supposed to rule out.

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
