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

### 3.2 `armmove`: a contradiction that dissolved on measurement

An earlier reading concluded the two callers demanded opposite polarities:

* CSET: `rd = 1; armmove(rd, 0, cond)`, and the suite wants 1 when `cset`'s
  condition holds;
* CSEL: `rd = false_value; armmove(rd, true_value, cond)`, and the suite wants
  the true value when its condition holds.

That was a misreading of the *encoding*, not of the hardware. `cset rd, cond` is
not an instruction: it assembles to `csinc rd, xzr, xzr, invert(cond)`, so the
condition the translator decodes for CSET is already inverted. Verified with the
assembler: `cset x2, eq` and `csinc x2, xzr, xzr, ne` are the same word
(`0x9a9f17e2`, cond field `0x1` = NE).

With that, a single rule satisfies both callers and AArch64:

```
armmove(rd, rj, cond)  ==  if (cond) rd = rj
```

CSET: cond (inverted) false exactly when the guest's condition holds, so rd keeps
its pre-set 1 - which is what the suite asserts. CSEL: rd takes the true value
when the condition holds.

The corollary matters more than the rule: the reverted software implementation
wrote `rd = rj & mask`, which drops `rd` unconditionally; that is what made every
CSET read 0 while CSEL looked fine. The fix is
`rd = (rd & ~mask) | (rj & mask)`.

### 3.3 MRS/MSR NZCV were not decodable; now they are

`baremetal/bare/lata_isa_test.c:702` said "Direct MRS NZCV is not implemented by
this compatibility decoder". The handler existed (`case ARM_CP_NZCV`,
`translate-a64.c:1835`), but nothing ever returned an `ARMCPRegInfo` for it: the
bare runtime's `get_arm_cp_reginfo` is a weak stub returning NULL
(`lata/runtime.c:39`), so every MRS/MSR became "Unknown register".

Fixed by adding `lata_nzcv_reginfo { .type = ARM_CP_NZCV }` to the port-local
sysreg table (`lata_local_sysreg`, `translate-a64.c:1766`), which is where the
bare build serves the registers it models itself (FPCR/FPSR/SPSR/ELR/ESR).
`mrs x5, nzcv` and `msr nzcv, x5` now decode and round-trip.

### 3.4 Why the raw flags first read as zero: an unreachable case, then a sign extension

The first run of the raw probe reported `mrs x5, nzcv` = 0 after every producer,
which looked like two separate flag registers. It was not: `handle_sys()`
dispatched special registers with

```c
switch (ri->type & ARM_CP_SPECIAL_MASK)
```

and `lata/compat/cpregs.h` defines `ARM_CP_SPECIAL_MASK` as `0xf` while
`ARM_CP_NZCV` is `1 << 7`. The `case ARM_CP_NZCV` was therefore unreachable for
every register, and MRS/MSR NZCV fell through to the generic field-offset path
with `fieldoffset == 0` - reading and writing the first eight bytes of
`CPUARMState`. The MSR/MRS round trip that "worked" was that same wrong offset
round-tripping through itself.

Widening the mask is not a one-line fix: it also activates `ARM_CP_DC_ZVA`, whose
case references a helper this build does not link (`ld.lld: undefined symbol:
helper_dc_zva`). NZCV is therefore handled explicitly ahead of the switch, and the
mask is left alone with a comment saying why.

With that fixed, the second run measured the producer flags directly, and they
were already architecturally correct on LBT:

| block | `mrs x5, nzcv` (before the sign-extension fix) | AArch64 |
|---|---|---|
| `adds` -1+1 | `0x0000000060000000` | `0x60000000` (Z\|C) |
| `subs` 0-1 | `0xffffffff80000000` | `0x80000000` (N) |
| `adds` MIN+MIN | `0x0000000070000000` | `0x70000000` (Z\|C\|V) |
| `ands` | `0x0000000040000000` | `0x40000000` (Z) |
| `adcs` chain | `0x0` | `0x0` |

One defect remained: `armmfflag` sign-extends the 32-bit flag word into 64 bits,
while AArch64 `MRS Xt, NZCV` zero-extends. Masking the upper half after
`armmfflag` fixes it, and both paths now agree bit for bit:

* software path (`LATA_FORCE_NO_LBT=1`): `6541` checks, `0` fails;
* LBT path: `6541` checks, `0` fails, identical per-producer values.

Consequences for the design: the "two flag views" concern is retracted - the port
has one flag model, and it was merely unobservable. The software path is still
the right long-term owner of `pstate[31:28]` (it is the only version that runs in
CI-less environments and that can be reasoned about without LBT), but P2's
motivation is now a cost/benefit call rather than a correctness one.

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

* CSET: emitted as `gen_select(rd=1, rj=0, cond)` where `cond` is the *decoded*
  field, i.e. the inverse of the assembly-level condition (§3.2). The result is
  AArch64-correct - 1 when the guest's condition holds - so no inversion is
  introduced or reproduced.
* CSEL/CSINC/CSINV/CSNEG: `gen_select(rd=false_value, rj=true_value, cond)`.

## 5. Phases

**P0 - done.** Decode `MRS/MSR NZCV` (`lata_nzcv_reginfo`, §3.3) and add the raw
probe (`run_raw_flag_probes`, §3.4). The probe's first reading was an artifact of
the unreachable `ARM_CP_NZCV` case; once fixed it showed the producers were
already correct, and the only defect was `armmfflag`'s sign extension (§3.4).
Both are fixed, and the probes are now assertions on the AArch64 nibbles.

**P1 - done.** Helpers in §4.2 are implemented and wired into all 28 sites, with
`lata_have_lbt` as the selector; `LATA_FORCE_NO_LBT=1` builds the image with the
software path forced. Both paths pass the whole ISA image (6541 checks) and the
raw-flag probes agree bit for bit. Two defects found on the way are fixed in the
same change: the unreachable `ARM_CP_NZCV` case (§3.4) and `armmfflag`'s sign
extension of the 32-bit NZCV word.

**P2 - retire the LBT flag path (now a cost decision).** Both paths are correct
and measured, so deleting the LBT branch is no longer a correctness argument.

Code size, measured from the ISA image (`words=` per block, both builds):

| fixture kind | LBT path | software path | delta |
|---|---|---|---|
| single producer + MRS | 17-21 | 27-37 | +10..+16 |
| ADDS/SUBS + 4x CSET pack | 30-40 | 75-85 | +45 |
| CSEL pair + 4x CSET pack | 36-38 | 88-90 | +50..+54 |
| ADC/SBC chain | 35-37 | 83-85 | +48 |
| 14-condition pack + MRS (8 CSETs) | 44-52 | 116-118 | +66..+72 |
| whole image, 21 flag-heavy blocks | 644 | 1416 | **+772 (+120%)** |
| whole image, every block | 3149 | 3931 | +782 (+25%) |

That is the cost side only: it is static code size, not runtime, and the fixture
set is deliberately flag-dense (a real guest spends most instructions on
non-flag work). A runtime number needs the image running on hardware rather than
under QEMU, which this environment does not provide - so P2's gate stays "measure
on a native LBT host, or keep both branches". Keeping both is the default.

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
| emitted code | ISA-suite blocks: producer x consumer matrix; every encodable condition (EQ..VC, HI..LE - 14; AL/NV are aliases the assembler rejects for CSET and are covered by the always-taken branch fixtures) packed into two bytes, over two flag states (N only, Z\|C), on both paths |
| integration | `make lata-isa-run` (6525 checks) on both paths during P1 |
| conformance | review of finding §3.2: the CSET expectation is either fixed in LATA or in the suite, with the AArch64 pseudocode quoted in the commit |
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

**Objection 3 (answered by measurement): is LATA's CSET inverted?**
No - the objection came from an earlier misreading of the encoding, corrected in
§3.2: `cset` decodes as `csinc … , invert(cond)`, so the single rule
`armmove = if (cond) rd = rj` produces AArch64-correct results for both CSET and
CSEL. Nothing in the port needs inverting, and the P2 "fix the suite" step this
objection asked for is dropped.

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
