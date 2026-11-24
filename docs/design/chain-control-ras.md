# Chain control and RAS: design

Status: no chaining exists; the RAS primitive exists but is unbound to the
LATR machine. The design is therefore mostly about *what must stay true* when
dispatch stops returning to C.

## 1. Current dispatch, exactly

`latr_machine_run` (`target/riscv/latr/system/latr_machine.c`) ticks the virtual
clock once per round and calls `run_hart` per online hart. `run_hart` executes
**one** translation block per call and returns, then the loop continues. Each
call performs these duties before entering generated code:

| # | duty | code |
|---|---|---|
| 1 | bind per-hart translator context and env | `latr_context_bind`, `latr_set_cpu_env` |
| 2 | tick this hart's CLINT, then recompute `mip` | `latr_system_update_hart` -> `latr_update_interrupts` (`latr_system.c:246-254`) |
| 3 | drain `sfence_pending` -> broadcast ASID shootdown to all caches | `latr_tb_shootdown_asid_all_cores` |
| 4 | drain `fence_i_pending` -> broadcast fence.i to all caches | `latr_tb_fence_i_all_cores` |
| 5 | deferred arena reclaim / insn-map forget | `latr_context_safepoint` |
| 6 | deliver a pending interrupt | `latr_deliver_interrupt` |
| 7 | key -> lookup -> (translate, insert, `latr_tb_insn_map_rebase`, `latr_icache_sync`) | `latr_tb_cache_lookup/insert` |
| 8 | enter, then handle `wfi_pending`, trap latches, diagnostics, counters | `m->enter_tb`, tail of `run_hart` |

Steps 1-6 and 8 are *semantics*; step 7 is *lookup cost*. Any chaining design is
only worth its risk if it preserves 1-6 and 8 exactly, and it must be able to say
why for each row.

## 2. Design

### 2.1 Chain state, one word of truth

Add to `LatrMachineHart` (per-hart, never shared):

```c
typedef struct LatrChainControl {
    uint64_t chain_epoch;    /* bumped when any gate below becomes untrue */
    uint64_t tb_generation;  /* bumped on insert/invalidate of this hart's cache */
    uint32_t remaining;      /* blocks left in the current chain */
    uint32_t max_chain;      /* policy: chain length cap */
    uint64_t links;          /* chained dispatches taken */
    uint64_t breaks[8];      /* by reason: interrupt, sfence, fence_i, wfi,
                                exception, reclaim, miss, budget */
    SoftRas  ras;            /* see 2.4 */
} LatrChainControl;
```

`chain_epoch` is bumped by exactly six events, each mapping to a duty above:

| event | bumps | why the chain must break |
|---|---|---|
| interrupt becomes pending or is delivered | yes | duty 6, delivery is C-side |
| `sfence_pending` / `fence_i_pending` set by translated code | yes | duties 3, 4 are broadcasts |
| deferred reclaim armed (`reclaim_pending`) | yes | duty 5 with arena reuse |
| TB inserted / invalidated on this hart | `tb_generation` | duty 7's key is no longer what the chain assumed |
| `wfi_pending` | yes | duty 8 side effect |
| guest trap latched (`exception`/`mem_fault_cause`/`fetch_faulted`) | yes | duty 8 diagnostics |

Nothing else may break a chain. If a future feature needs to, it must add a row
here *and* a test that the chain breaks - that is the whole safety argument.

### 2.2 Phase A: C-side re-dispatch (no generated-code chaining)

After `m->enter_tb` returns, if

* `env.pc` equals the successor recorded by the last successful lookup (the
  `LatrTbEntry` already knows its `guest_pc` and the port knows the block's
  fallthrough/target because `target_disasm` produced them), and
* `chain_epoch` is unchanged and `tb_generation` is unchanged,

then call `enter_tb` again with the known code pointer, skipping only step 7's
*lookup* (key build + 4-way compare + hash). Duties 1-6 and 8 still run.

Why this first: it is a pure C change, it is obviously correct (steps 1-6 still
execute in the same order), and it removes the per-block key build, which is the
dominant cost of dispatch for a 14-45 word block (`lata`-side measurements show
14-83 words per block; LATR blocks are comparable).

A new counter `links` and `breaks[]` land in the same change, so the benefit is
measured before anything riskier is attempted.

### 2.3 Phase B: generated-code chaining (only if Phase A is insufficient)

Emit one exit stub per TB instead of a return to the dispatcher:

```
    ld.d   $t0, $s8, offsetof(env, chain_next_code)
    beqz   $t0, 1f
    ld.d   $t1, $s8, offsetof(env, chain_epoch)
    ld.d   $t2, $s8, offsetof(env, chain_epoch_entered)
    bne    $t1, $t2, 1f
    jirl   $zero, $t0, 0
1:  <return to dispatcher>
```

`chain_next_code` is written by C before entering a block, and only when the
successor is already translated and all gates are clear: the C side has the
authority, the generated code only checks "has anything changed since I was
entered". This keeps the epoch comparison off the C path and keeps the decision
in C, which is where the duties live.

Constraints that must be stated because they are where such designs fail:

* The stub may not be used for a block that ends in a **trap or helper fault
  path**; those return through the existing path so duties 5 and 8 run.
* `env.chain_next_code` must be cleared (a) on every chain break, (b) on any
  context switch, and (c) when the target TB is invalidated - the last one is
  what `tb_generation` is for. A stale pointer here is a jump into recycled
  arena memory, i.e. exactly the failure `latr_icache_sync` was added to stop.
* Reclaim: a chain that outlives `reclaim_pending` would reuse bytes it is
  executing; the `reclaim` break row exists for that.

### 2.4 RAS

`SoftRas` already exists (`include/loongemu/soft_ras.h`): 32 entries,
`guest_pc/native_pc/cr3/cpl/mode_flags` validation, push/lookup/flush plus
pushes/lookups/hits/misses/flushes/backfills counters, and `test_soft_ras`
covers it. It is x86-shaped (`cr3`, `cpl`).

Design: **adapt rather than duplicate**. Two options were considered:

1. Generalize `SoftRasEntry` to a translator-agnostic key blob.
2. Add a LATR-keyed view: `satp_mode/privilege/asid/vspec/root_ppn`, the same
   tuple as `LatrTbKey` (`latr_tb_cache.h:19`).

Option 2 is chosen: `SoftRasEntry`'s four identifying fields become the
`LatrTbKey` fields behind a union, guarded by a `ras_mode` tag (x86 vs RISC-V),
so `test_soft_ras` keeps working and LATR gets a validated predictor without a
second stack implementation. `soft_ras_lookup(ras, guest_pc, cr3, cpl, mode_flags)`
(`include/loongemu/soft_ras.h:45`) already validates the whole key, which is the
property that makes it safe to jump to `native_pc`; nothing may bypass that
check when the entry is retargeted to a `LatrTbKey`.

Use: on `jal/jalr` with `rd == ra` (CALL), push `(guest_pc after, native_pc
after)`; on `jalr x0, ra, 0` (RET), look up and, when it hits *and* all gates are
clear, offer the target as `chain_next_code`. RAS is therefore a *predictor for
chaining*, never an authority: a miss falls back to the normal lookup.

### 2.5 RAS (reliability) observations

`breaks[]` is the RAS (reliability, availability, serviceability) side: eight
counters answering "why did dispatch go back to C". Plus:

* chain length histogram (per-hart, 8 buckets);
* `tb_generation` and `chain_epoch` values in the panic/trap dump, so a stale
  chain is diagnosable from a core dump instead of by reproduction;
* a PERF counter pair (blocks entered, links taken) so the win is attributable.

## 3. Phases

| phase | change | gate |
|---|---|---|
| P0 | `LatrChainControl` with counters only; `links` stays 0 | boot smoke, `latr-isa` unchanged |
| P1 | Phase A re-dispatch + `breaks[]` | boot smoke, `latr-isa 1607`, counters recorded |
| P2 | exit stub (Phase B) + epoch compare, chain cap | same, plus forced-break tests |
| P3 | RAS-keyed prediction feeding `chain_next_code` | same, plus hit-rate counters |
| P4 | SMP: epochs are per-hart; broadcast only where a duty is cross-core (shootdown) | boot-smp CI job |

## 4. Tests

* `tests/test_latr_chain.c` (host, new): drive the state machine directly - each
  of the six break events must clear `chain_next_code` and increment its
  `breaks[]` bucket; a chain may not survive `tb_generation` change.
* `baremetal/bare/tb_loop.c` exists and already documents a label-dispose
  hazard on branch-heavy blocks; extend it with a chain-eligible loop so the
  generated-code path is exercised deterministically.
* Boot smoke (`boot-smp` CI job) with `wfi` in the guest idle loop, which is the
  most common chain breaker and the easiest to get wrong.
* ISA gates unchanged: `latr-isa 1607`, `latm-isa 60`.

## 5. Self-review

**Objection 1: Phase A is "chaining" that still returns to C, so the headline win
of chaining (no dispatch round trip) is not achieved.**
True, and deliberate. Phase A removes the lookup/key build while keeping every
duty in the same place, which is the only version whose correctness argument is
a one-line diff. It also produces the counters that decide whether Phase B is
worth its risk. If Phase A shows dispatch cost is already < 5% of runtime, Phase
B should not be built at all, and the doc says so rather than budgeting for it.

**Objection 2: the epoch design implies generated code reads `env` twice per
block exit, which is not free on the hot path.**
Two loads and two compares against a block that is 14-45 words of arithmetic is
acceptable, but it is a real cost and the design keeps the *decision* in C. If
the measurement shows the stub costs more than it saves, the fallback is Phase A
only - no semantic change required.

**Objection 3: `env.chain_next_code` is a host pointer in guest-visible
structure; a guest with a corruption primitive could steer it.**
The field lives in the translator's `CPUArchState`, which is PLV0-only and not
guest-mapped (same argument as `SoftRas`'s header comment: "PLV0-only per-vCPU
state, never mapped into PLV3"). Still, the generated-code check is an *epoch*
check, not a pointer check, so a corrupted pointer plus matching epoch would
jump anywhere. Mitigation, stated as a requirement: `chain_next_code` is written
only from a validated `LatrTbEntry->code` immediately before `enter_tb`, so its
window of validity is one block; the stub clears nothing, C always rewrites it.

**Objection 4: generalizing `SoftRasEntry` risks breaking the x86 path that
already has tests.**
Yes, hence the union + `ras_mode` tag rather than field renaming, and hence
`test_soft_ras` staying in the gate. The alternative (a second stack) doubles the
code and the bug surface for a 32-entry array; the union costs one branch on a
path that is already a prediction.

**Open question:** whether chaining should be disabled under `LOONGEMU_PERF` so
counter attribution stays exact. Lean: no - measure chain overhead as its own
bucket instead, because a profiled build that changes the thing being profiled
is worse.
