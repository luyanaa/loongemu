# Design index and cross-review

Five designs, written one per file:

| # | design | file | dominant risk |
|---|---|---|---|
| 1 | LATA software NZCV | [lata-software-nzcv.md](lata-software-nzcv.md) | unknown (LBT flag semantics) |
| 2 | IR2 ValueVersion / next-use | [ir2-value-version.md](ir2-value-version.md) | recovery-span staleness |
| 3 | MMIO fast path, MSI/MSI-X, DMA isolation | [mmio-msi-dma.md](mmio-msi-dma.md) | interrupt-target self-modification |
| 4 | Chain control and RAS | [chain-control-ras.md](chain-control-ras.md) | skipping a dispatch duty |
| 5 | Persistent code cache | [persistent-code-cache.md](persistent-code-cache.md) | executing bytes that no longer match |

Each file carries its own self-review (objections and resolutions). This page is
the cross-review: the parts that only show up when the five are read together.

## Dependency order (what can start when)

```
#5 P0 (relocation test) ──┬─> #5 P1..P3
                          └─> hardens #4's exit stub (same codegen surface)
#4 P0 (counters) ──> #4 P1 (C re-dispatch) ──> #4 P3 (RAS prediction)
#3 P0 (msix core + delivery decode) ──> #3 P1 (capability) ──> #3 P2 (sink)
#3 P3 (DMA domains) independent of MSI
#2 P0 (counters) ──> #2 P1 (versioned state) ──> #2 P2 (flush elision)
#1 P0 (MRS/MSR NZCV observable) ── then halted on external documentation
```

Nothing above depends on #1, which is why #1 can be parked without blocking the
rest. #2 and #4 interact only through `env` growth (both add per-hart fields);
neither may add a field without naming it in the other's docs.

## Conflicts found while reviewing the five together

1. **Both #2 and #4 add per-hart state to `CPUArchState`.** #4 adds
   `chain_next_code`, `chain_epoch`, `chain_epoch_entered`; #2 adds nothing to
   `env` (its state is translator-side, per TB). Resolution: #4's fields are the
   only new `env` fields in this batch, and they must be zero-initialised on
   every context switch - stated in #4 §2.3 but easy to miss from #2's side.
2. **#4's exit stub and #5's relocation both change what a TB ends with.** If #4
   lands first, the stub is an extra code shape that #5 must relocate; if #5
   lands first, #4 inherits a codegen-fingerprint constraint (any stub change
   invalidates every cache record). Resolution: #5's `format_version` must be
   bumped by #4's exit-stub change, and #5's fingerprint (translate a
   micro-program and hash it) makes that automatic rather than a reminder.
3. **#3's MSI-X delivery and #4's interrupt epoch.** A vector injection is one of
   the events that must break a chain (#4 §2.1 table). #3's sink must call
   whatever bumps `chain_epoch`; the designs say so individually, but the
   coupling is one-directional and #3 must not know about chaining. Resolution:
   the bump lives on the *interrupt-pending* transition, in the machine, not in
   the device or the sink.
4. **#2's flush elision and #1's flag materialisation point.** If #1 ever lands,
   flags become guest state materialised at a chosen point; #2's elision changes
   when guest state becomes canonical. #1 §6.4 and #2 §4.2 state the same rule
   from both sides (no elision that changes what a recovery span would claim), so
   they agree - but only by that shared sentence, which is worth keeping in one
   place if either is edited later.

## Cross-cutting invariants (apply to all five)

1. **Guest-observable state is canonical at every observable point**; anything
   that defers a write (`#2`), skips a duty (`#4`), or reuses bytes (`#5`) must
   name the point at which it becomes canonical.
2. **Fail closed on uncertainty.** `latr_machine_host_fault` returns 0 rather than
   reporting a partial state; #1's flag spans, #2's bindings, #5's recovery rows
   all inherit that rule.
3. **One owner per resource.** Arena bytes: the arena (#5 copies, never maps as
   executable). MSI-X table: the host copy (#3). Guest values: the port's regcache
   (#2). Chain decision: C, not generated code (#4).
4. **A regression must be attributable.** Every design lands counters before it
   lands behaviour (#1: none, blocked; #2: flush/writeback; #3: MMIO latency and
   interrupt counts; #4: links and breaks; #5: load vs translate time). A
   performance claim without a counter is not made in any of the five.

## What is deliberately not designed here

* An IOMMU. #3 says so explicitly: no PCIe passthrough without one.
* AOT region compilation (P3 of the roadmap). It depends on #5's relocation work
  and on hot-page counters that already exist (`latr-tb-cache.h`, `LATR_TB_HOT_SLOTS`),
  but it is a different design.
* Migrating recovery rows across codegen changes (#5 §self-review 2). Requires
  symbolic bindings; out of scope.

## What the review pass changed (documented, not silent)

The self-review was done against the tree, not against memory, and it corrected
four claims:

1. **#2**: an early draft said control-flow edges flush through the
   `latr_regcache_b/label/jirl` wrappers. They have **no caller** - the port
   emits raw `la_b`/`la_label` and calls `latr_regcache_flush()` by hand at 10
   sites. The design now states the real discipline and lists "adopt or delete
   the wrappers" as a prerequisite of P2.
2. **#3**: an early draft named IMSIC (RISC-V) and LAPIC (x86) as delivery
   targets. Both exist in the tree only as ACPI *descriptions*
   (`riscv_acpi.h`, `acpi_tables.h:16` + `acpi_tables.c:133`); the PLIC is the
   only implemented interrupt controller. The design now names the PLIC as the
   first sink and the x86 side as a pending-vector decoder that is explicitly
   not a LAPIC.
3. **#5**: label resolution was described vaguely as "PC-relative offsets"; the
   code shows `label_dispose` converts labels to **instruction-index
   differences** before encoding (`latr.c:1075-1125`), so `offs16/offs26` fall
   out of the encoder. That is why the relocation surface is only the
   `li_d(target, &fn)` helper literals - now stated as such, with the far-branch
   caveat.
4. **#1**: the site table was an estimate; it is now counted (28 instruction
   sites across 6 LBT instruction kinds, with line numbers), and the MRS/MSR
   handler location (`translate-a64.c:1835/1841/1848`) is named so P0 knows the
   handler exists and the gap is upstream of it.

Two claims were deliberately left as claims rather than evidence: LATR block
sizes (no measurement; LATA's 14-83 words per fixture is cited as a proxy and
labelled as such) and the benefit of #5 (objection 5 says to stop after P0 if
translate time is a small fraction of boot).

## Review checklist applied to each file

* Every claim about current behaviour cites a file and a line that was read, not
  remembered. Where a number came from a log (block sizes), the doc says so.
* Every phase has a gate that can fail, and the gate is the narrowest one that
  exercises the change.
* Every design states at least one reason it might not be worth doing, and what
  to keep if the measurement says so (#2 P0, #3 P2/P3 split, #4 Phase A, #5
  objection 5).
* Every "self-review" objection is either resolved in the design or left as an
  explicit open question; none is dismissed.
