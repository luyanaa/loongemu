# LATA software NZCV: design

Status: blocked on one measurement (see §3.1), everything else is decided.
Owner note: this is the only one of the five designs whose remaining risk is
*knowledge*, not engineering.

## 1. Problem

LATA keeps AArch64 NZCV in the LoongArch binary-translation (LBT) flag
register. Every flag producer and consumer is an LBT instruction:

| role | instruction | sites in `translate-a64.c` (counted) |
|---|---|---|
| produce (add/sub) | `x86add_d/w` x1+x1, `armmtflag`+`x86adc_d/w` x3+x3 | `gen_add_CC:848`, `gen_sub_CC:860` (2), `disas_adc_sbc` (4) |
| produce (logical) | `x86and_d` x5, `x86and_w` x5 | `trans_ANDS_i`, `disas_logical_*` (10) |
| produce (fp compare) | `armmtflag` | `FCMPS/FCMPE` 9078/9108, `FCCMP` nomatch 9236 |
| consume (cond) | `armmove` x3, `setarmj` x4 | CSET 8252, CSEL 8303/8307; branch 1201, CCMP 8168, FCSEL 9230, FCCMP 9286 |
| consume (sysreg) | `armmfflag` x1 | MRS NZCV 1841 |
| transfer | `armmtflag` x7 | `gen_sub_CC` 870/883 (carry=1), MSR 1848, CCMP FALSE 8207, fp 9078/9108/9236 |

A host without LBT cannot run any translated AArch64 code, which is why the CI
matrix has no LATA gate (`ci.yml`, "the LATA gate is not here on purpose").
The port already probes the host (`lata_have_lbt`, `lata.c:358`,
`latx_cpu_has_lbt()`), so the switch exists; the software side does not.

## 2. Reference semantics (already in the tree)

`tests/test_lata_nzcv.c` (267 checks, in `make test`) pins the arithmetic:
AddWithCarry over 32/64 bits, the carry-in rule that `res < a` alone gets wrong,
and the full 16x16 condition matrix. Nothing in the design below may disagree
with it; the file is the arbiter, not the ISA suite.

## 3. What measurement already established

An implementation was written, forced on (`lata_force_no_lbt()`), measured on a
host that has LBT, and reverted. Three results, all reproduced:

### 3.1 Consumers are the problem, not the producers

Substituting a constant nibble for the pstate read still produced 0 at the flag
consumers, so the arithmetic was never implicated: `armmove` and friends are.

### 3.2 `armmove`'s contract is undocumented and, as read from its callers,
contradictory

* `disas_cond_select`, CSET path: `rd = 1; armmove(rd, 0, cond)`. The ISA suite
  asserts `cset x2, eq` after `subs` to zero yields 1, i.e. cond true must
  **leave rd unchanged**.
* Same function, CSEL path: `rd = false_value; armmove(rd, true_value, cond)`.
  The suite asserts `csel x3, x2, x4, lt` with LT true yields the true value,
  i.e. cond true must **write rd**.

Both pass on LBT. No single conditional-move polarity satisfies both, so the
LBT encoding carries more than a 4-bit ARM condition test. The failing side is
always CSET; the split follows bit 3 of the condition field (`EQ..VC` vs
`HI..LE`), which is exactly where ARM's simple/compound boundary sits.

Hypothesis worth testing before any code is written: the LBT flag register
layout differs per producer (`x86add` vs `armmtflag`+`x86adc`), and the suite's
expectations were recorded against that difference rather than against AArch64.
If true, the port has a latent conformance bug independent of this design.

### 3.3 MRS NZCV is not decodable in this configuration

`baremetal/bare/lata_isa_test.c:702`: "Direct MRS NZCV is not implemented by
this compatibility decoder." So the obvious measurement - read the raw flag
register after each producer - is currently blocked by another missing feature.
That makes the decode of `MSR/MRS NZCV` (op0=3, op1=3, CRn=4, CRm=2) the first
task, not an optional one.

### 3.4 Temporary-register budget

`ra_alloc_itemp` asserts at six (`reg-alloc.c:365-366`, seen as
`alloc itemp failed, cto_num 6`). `gen_cond_value` needs 3,
`gen_cond_move` adds 2 more and is called with operands live from
`disas_cond_select`, which already holds `reg_n`, `reg_m`, `temp_m`, `temp_n`.
Any implementation must free temporaries *before* emitting the pstate store.

## 4. Design

### 4.1 One storage location

NZCV lives in `env->pstate[31:28]`, layout `N<<3 | Z<<2 | C<<1 | V`. This is
already what the port means by `env_offset_PSTATE()` (`lata.c:1255` builds
SPSR_EL1 from it, `sve-lasx-lower.c:1990-1992` feeds it to `armmtflag`), so the
software path does not invent a location, it uses the existing one.

### 4.2 Helpers (one implementation, no per-site arithmetic)

| helper | contract | temporaries |
|---|---|---|
| `nzcv_nibble_load(dst)` | dst = pstate[31:28] | 1 (store/load path may need 1 more internally) |
| `nzcv_nibble_store(nib)` | pstate[31:28] = nib[3:0] | 1 |
| `gen_nzcv_addsub(a, b, is_sub, sf)` | flags of AddWithCarry | 4 (6 with 32-bit zero-extension) |
| `gen_logic_CC(res, sf)` | N, Z, C=V=0 | 2 |
| `gen_adc_sw(dst, a, b, is_sub, setflags, sf)` | result + flags, carry-in aware | 6 |
| `gen_cond_value(dst, cond)` | 0/1 per the §2 matrix | 3 |
| `gen_select(rd, rj, cond)` | `rd = cond ? rj : rd` | 2 + cond_value's 3, freed first |

`gen_select` takes the *pre-loaded* destination, which is exactly the shape
both call sites already have (`rd = false_value` / `rd = 1`), so the caller's
intent stays explicit and no site depends on a corner case.

### 4.3 Removing the ambiguity rather than reproducing it

Two options were considered for the LBT path:

1. Keep `if (lata_have_lbt)` on every site (what the reverted attempt did).
2. Retire LBT for NZCV entirely: one code path everywhere.

Option 2 is chosen. Rationale: the LBT path is only a speed optimisation on a
handful of hosts, while the software path is the only one that can be tested in
CI, reviewed against `tests/test_lata_nzcv.c`, and reasoned about. Two
semantically divergent flag paths is precisely the condition that produced
finding 3.2. Cost is measurable: ~4-8 extra host instructions per flag-setting
guest instruction, to be recorded with the existing PERF counters
(`LOONGEMU_PERF_TB_EXEC_TICKS`) before/after on the same workload.

The LBT instructions stay for *non-flag* uses if any remain after the audit
(the audit table in §1 is the checklist: 28 instruction sites across 6 instruction
kinds, all flag-related).

### 4.4 Caller semantics that the suite asserts, restated

* CSET: emitted as `gen_select(rd=1, rj=0, cond)` -> 1 when cond false, 0 when
  true, matching the *existing* LBT behaviour the suite encodes; the AArch64
  reading of `cset` is the opposite, and that discrepancy is §5.1.
* CSEL/CSINC/CSINV/CSNEG: `gen_select(rd=false_value, rj=true_value, cond)`.

## 5. Phases

**P0 - make flags observable (blocking, ~1 day).** Decode `MRS/MSR NZCV`; the
decoder already has the handler (`case ARM_CP_NZCV`, `translate-a64.c:1835`) and
maps MRS to `la_armmfflag` (1841) / MSR to `la_armmtflag` (1848), so the gap is
in what reaches it, not in the handler; add an ISA-suite block that reads the raw flags after
`adds`, `subs`, `adcs`, `ands` and prints them. Deliverable: one log line per
producer showing the actual nibble, which settles §3.2/§3.1 or refutes them.

**P1 - software helpers behind a switch.** Implement §4.2, wire every producer
and consumer, keep `lata_have_lbt` as the selector. Gate: `make lata-isa-run`
green with the switch forced on (`-DLATA_FORCE_NO_LBT`), on a host with LBT.

**P2 - retire the LBT flag path.** Delete the `if (lata_have_lbt)` blocks for
NZCV, drop the probe to a diagnostic, measure the delta.

## 6. Invariants and failure modes

1. Guest-visible NZCV is readable at any TB boundary (fault, safepoint, helper)
   from `env->pstate` alone. No translation may hold flags in a host register
   across an observable point.
2. Every producer writes the whole nibble, never a subset: a stale C bit from an
   earlier instruction is a wrong result, not a performance bug.
3. `gen_select` may not be called with fewer than 5 free itemps; the callers'
   live operands count against the pool, so it frees before storing.
4. Recovery spans (`LeRecoverySpan`) that bind a guest register holding flags
   must be invalidated if the software path changes when those flags are
   materialised. `retired_before`/`phase` are the fields that express this.

## 7. Verification

| layer | check |
|---|---|
| semantics | `tests/test_lata_nzcv.c` (host, ms) |
| emitted code | new ISA-suite blocks: producer x consumer matrix, all 16 conds |
| integration | `make lata-isa-run` (6525 checks) on both paths during P1 |
| conformance | §5.1 review: the CSET expectation is either fixed in LATA or in the suite, with the AArch64 pseudocode quoted in the commit |
| cost | PERF tick delta on the same guest workload |

## 8. Self-review

**Objection 1: choosing option 2 (retire LBT) is a performance regression on
LBT hosts, and LBT hosts are the ones this project targets.**
Resolution: measured, not asserted - P2 gates on the PERF delta. The alternative
(keep two paths) costs a permanently untestable production path in CI, which is
how §3.2 arose. If the delta exceeds ~2% on the benchmark, revisit with a
compile-time build variant instead of a runtime branch: same code shape, chosen
at build time, so only one path exists per artefact.

**Objection 2: the design defers the actual unknown (armmove) to P0, so the
"design" is not yet a design.**
Partly true, and deliberate. Any design that specifies a polarity now is
guessing; a design that specifies "one helper, caller-explicit, verified by
measurement" is not. P0 is bounded (decode two encodings, add a fixture) and
decisive. If P0 shows the LBT layout is producer-dependent, this design needs a
new section and the port needs a conformance bug fixed first - that is a better
outcome than silently matching it.

**Objection 3: §4.4 says CSET in LATA is inverted relative to AArch64 but the
design reproduces it. Should the port be fixed to AArch64 instead?**
It should, and the design makes that decision explicit rather than accidental:
fix it in P2 as a separate, visible commit that also fixes the suite's `cset`
expectations, because a silent semantic change inside a "make it work without
LBT" change is exactly the kind of coupling that hides a regression. The
`csel` fixtures already encode AArch64, so the suite will notice.

**Objection 4: does the software path interact with the recovery work?**
Yes: `LeRecoverySpan` records which guest registers a partially executed
instruction has already committed. Flags are guest state; if the software path
materialises them at a different point than the LBT path did, span bindings that
name flags become wrong. Invariant 6.4 covers it; the concrete check is that
`latr-machine`'s LATA equivalent fails closed (returns 0) when a span with
bindings has no host snapshot - the LATA port must apply the same rule as
`latr_machine_host_fault`.

**Open question:** whether `setarmj` (4 sites) can be folded into
`gen_cond_value` + `bnez` losslessly, or needs a fused predicate-to-branch
helper. The reverted attempt did the former; it is one extra host instruction
per conditional branch on non-LBT hosts, acceptable, but unmeasured.
