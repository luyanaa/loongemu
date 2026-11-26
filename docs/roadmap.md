# Roadmap: remaining work, in priority order

This file is the single record of what is left and in which order. It supersedes
the per-design phase tables where they disagree, and it is written from a review
of the four translators plus the shared layers. Each claim below states the file
and symbols it is about; designs live in `docs/design/`.

Already landed and **not** re-opened here (see the design docs for evidence):

| area | state |
|---|---|
| LATA software NZCV | P0/P1 done; both paths 6549 checks, CI job `isa-lata` runs the software path under a distro qemu |
| LATA SVE flag writes | routed through `lata_emit_nzcv_write()`; software image emits 0 LBT opcodes (asserted) |
| LBT-opcode leakage | `tools/gen_ir2_lbt_opcodes.py` + `tests/test_ir2_lbt_opcodes.c` + harness assertion |
| MMIO route cache | implemented and tested (`src/mmio_bus.c`, `tests/test_riscv_uart.c`) |
| ValueVersion P0 | `LatrRegCacheStats` counters; writebacks == stores, evictions == 0, 94-98% of flushes are no-ops |
| Persistent-cache P0 | `latr_emit_code_symbol()`; 485 relocation sites over 286 blocks, overflow is a hard failure |

## 2.1 LATA: stop treating NZCV as the front line; the runtime is - ABI DONE

NZCV is done. The next LATA work is multi-TB lifecycle, and two items are
potential out-of-bounds writes, not performance:

`src/lata_dispatch.c:55-105` and `include/loongemu/lata_dispatch.h`:

| current behaviour | risk |
|---|---|
| arena is a bump allocator, and exhaustion simply stops execution | a guest that outgrows it stalls with no diagnosis |
| `translate()` receives no remaining capacity | the translator cannot refuse |
| only the starting offset is checked before translating | a TB that is longer than the remaining space writes past the arena |
| `lata_dispatch_flush()` clears the cache but does not reclaim the arena | flush does not buy space back |
| I-cache synchronisation is the embedder's job | a dispatcher user that forgets it executes stale bytes |
| no owner safepoint, no cross-core invalidation, no reclaim | unlike LATR (`latr_tb_cache_reclaim_all`, `latr_context_safepoint`, `latr_tb_shootdown_*_all_cores`) |

Target ABI (from the review) - implemented in `include/loongemu/lata_dispatch.h`
and `src/lata_dispatch.c`:

```c
typedef struct TbEmitResult {
    uint32_t code_size;
    uint32_t guest_size;
    uint32_t recovery_count;
    uint32_t flags;
} TbEmitResult;

int translate_tb(void *opaque, uint64_t guest_pc, void *code_addr,
                 size_t capacity, TbEmitResult *result);
```

The dispatcher must reserve the whole TB before the translator runs, refuse when
it does not fit, reclaim on flush, own the I-cache sync, and reuse LATR's arena
lifecycle instead of a second implementation.

Status: capacity is now passed down (`size_t capacity`), the translator reports
`TbEmitResult`, a result larger than the reservation is refused and counted
(`refused`), arena exhaustion reclaims the cache and arena and retries once
(`reclaims`) instead of stopping, `lata_dispatch_flush()` reclaims the arena with
the cache, and `sync_icache` is a dispatcher-called op instead of an embedder
duty. Tests cover capacity, a translator that lies about its size, reclaim across
six TBs, and a TB too large for the whole arena.

Safepoint and SMC are now explicit too: `in_tb` marks the window in which the
guest executes arena bytes, a flush from inside `enter` is refused and counted
(`safepoint_violations`), every flush/reclaim bumps a generation (`epoch`), and
the new optional `smc_epoch` op lets the embedder announce that guest code
changed - the loop then drops the cache and arena before the next dispatch
(`smc_flushes`), which is the dispatcher's half of LATR's fence.i + shootdown
contract. Tests drive both paths.

Still open on this line: reusing LATR's arena implementation rather than the same
policy written twice, owner-side safepoint integration with the machine's halt
path, and cross-core invalidation once vCPUs run in parallel (that last one is
listed under 2.2 SMP).

Rest of the LATA route:

| priority | work |
|---|---|
| P0 | arena bounds, reclaim, SMC, I-cache sync ownership |
| P0 | host fault -> precise guest PC / register recovery - **done**: `lata_host_fault()` resolves, decides with the shared rule, replays bindings and sets `env->pc` |
| P1 | AArch64 Linux boot contract: EL1, GIC, generic timer - **EL/vector delivery done** (`lata_take_exception`: ESR/ELR/SPSR/PSTATE latched, VBAR+0x400 from EL0 and +0x200 from EL1, DAIF masked); GIC and generic timer still open |
| P1 | exclusive monitor, LSE atomics, SMP validation |
| P1 | SVE, FPCR/FPSR, exception and rounding semantics |
| P2 | lazy NZCV, CSEL/B.cond fusion |
| P2 | TB chaining, cross-TB register residency |
| P3 | SVE2, PAC, MTE selectable by CPU profile |

## 2.2 LATR: closest to running a full OS

**RVV 1.0 encodings: complete.** The per-group table that used to sit here (24
FP FMA, 10 carry/borrow, 8 integer MAC, 8 widening, 6 high-multiply, 6 other FP,
2 fixed-point) went stale one batch at a time and was still saying "missing"
after every one of those groups had landed; a session was spent surveying work
that was already done. The check that replaces it takes one command and is the
one to re-run before starting any RVV batch:

```sh
python3 - <<'EOF'
import re, pathlib
names=[m.group(1) for l in pathlib.Path('target/riscv/latr/ir1/rv64gc-opcodes.def').read_text().splitlines()
       if (m:=re.search(r'LATR_RV_INSN\(([A-Za-z0-9_]+),', l))]
tr="".join(pathlib.Path(p).read_text() for p in
    ['target/riscv/latr/translator/translate-riscv-rvv.c','target/riscv/latr/translator/rvv.c',
     'target/riscv/latr/translator/rvv-legality.c'])
suf=re.compile(r'_(VV|VX|VI|WV|WX|WI|VF|VVM|VXM|VIM|V)$')
print([n for n in names if n.startswith('V') and suf.sub('', n) not in tr])
EOF
```

Current result: `[]` - no base-V mnemonic is absent from the three files that
translate RVV, and the 24 whole-register encodings (`vl<n>re<eew>`, `vs<n>r`,
`vmv<n>r`) that a first-pass grep flags are dispatched through
`latr_rvv_whole_kind()` into `latr_rvv_whole_mem()` / `latr_rvv_whole_move()`, not
by name.

That family is now covered by the ISA image end to end: all 24 encodings are
executed (20 loads across the four EEW spellings, `vs1r/vs2r/vs4r/vs8r` and
`vmv1r/vmv2r/vmv4r/vmv8r`), 1830 checks in the image. The expected bytes come
from a QEMU probe, not the spec: `qemu-system-riscv64 -M virt -cpu max` (and
`-cpu rv64,v=true`) run against a page holding byte i+1 at offset i returns
`01..10` for `vl1re{8,16,64}`, `11..20` for the second register of a
two-register group, `31..40` for the fourth of four, `71..80` for the eighth of
eight, and the stores write exactly `1..16`, `1..64`, `1..128` bytes and nothing
past that. Two probe facts worth keeping, both of which cost a run: `mstatus.VS`
must be set (`csrs mstatus, 0x600`) or `vsetvli` traps as illegal with no output
at all, and a bring-up routine must not use `t2`/`x7` (nor `s0`/`x8`) if those
hold the probe's base addresses.
| `vrsub` | 2 |

After those, SMP is still not real: `latr_machine.c:864-902` runs harts
round-robin, single-threaded. Per-hart context, cache, TLS, shootdown and the
LR/SC reservation base exist, but none of the following is proven:

* cache/arena safety with two vCPUs executing generated code at once;
* completion semantics of cross-core `FENCE.I` / `SFENCE.VMA`;
* ordering of LR/SC, AMO and device DMA;
* `WFI`, IPI, SBI HSM and remote fence under real parallelism;
* RVWMO mapped onto the LoongArch memory model.

Priority: finish single-hart Linux boot, then make SMP real.

## 2.3 LATM: the missing piece is a genuine 3A4000-compatible machine

**Inventory before starting (measured, so the next attempt does not redo it).**
`make test-latm` is green: decode, tlb, vclock, interrupts, insn-map, tb-cache,
cpu, loongson, system, uncore, llsc, unaligned (296), pci, board-system (55)
suites, plus `latm-isa-run` (60 checks) and `latm-boot-smoke-run` (ELF load, one
TB, a UART byte through the board). Already present and tested, so **not** part
of the 2.3 work:

| piece | where | covered by |
|---|---|---|
| 4-CPU mailboxes + IPI enable/status/set/clear + send-buf, Linux `loongson3_ipi` offsets (0x1004/08/0c, 0x1020+, 0x1040/48) | `uncore/latm_uncore.c:299` | `latm-uncore` 25 checks |
| EXTIOI: per-CPU `coreisr[4][8]`, `cpu_ip[4]` Cause-IP masks, route/coremap/nodetype, write-1-to-clear | same | `latm-uncore`, `latm-interrupts` |
| PCH PIC (64 lines), MSI window, 7A PCI config/BAR | `system/latm_pci.c`, `uncore` | `latm-pci` 7, `latm-board-system` |
| DTB: 4 `cpu@N` nodes with `enable-method = loongson,3a4000` and a matching `cpu-map` | `system/latm_dtb.c:216` | `latm-system` 21 |
| boot info + 3A4000 memory layout (low/high RAM, VMM, code) | `system/latm_memory.c`, `latm-system.h` | `latm-system` |
| 3A4000 CPU profile, MMI/LLSC/unaligned | `cpu/`, `helpers/` | `latm-cpu`, `latm-loongson`, `latm-llsc`, `latm-unaligned` |

**What is actually missing is the execution model, not the register model:** the
board image drives exactly one `CPUMIPSState` through one dispatch loop, so no
second vCPU ever runs and nothing cross-CPU can be observed. The next step is the
LATR-shaped piece for LATM, and its acceptance test can be stated now:

**Items 1-2 are in** (`baremetal/bare/latm_board.c`, gate `latm-board-run`):
`run_one_tb_of(env, cache, buf, next, size)` takes the CPU explicitly, and
`board_run_host_of()` rebinds `lsenv`/`tr_data` to that CPU before every dispatch
(LATM already carries `latm_env`/`latm_tr_data` per `CPUMIPSState`, so the bind is
`lata_tr_data_init(cpu)` and the context needs no new type). A second CPU context,
TLB, TB cache, host code buffer and prologue/epilogue pair are created next to
CPU 0's, and both run the same guest with `a0 = cpuid`, each incrementing its own
word: measured `c0=16 c1=16 pc0=pc1=0xffffffff80004014`, `latm-smp: PASS`.
Mutation-tested: making CPU 1 dispatch through CPU 0's context gives
`c0=18 c1=14` and FAIL, so the per-CPU bind is what the test is measuring.

Two trap worth keeping from this step: the guest is linked at 0 but runs in
kseg0, so `j` (which takes its region bits from PC) jumped nowhere - a
PC-relative `b` is the right loop. And the board's instruction words are stored
as host words holding big-endian encodings, e.g. `lui $t0,0x8000` is `0x3c0c8000`.

**Remaining:**

3. **in**: the board now wires `platform.iocsr_read/write` to a single
   `LatmUncore` instance with the dispatching CPU id, CPU 0's guest executes
   `iocsrwr` (major opcode 0x32) to 0x1040 with target=1, the driver maps the
   mailbox bit to IP4 and delivers it to CPU 1, and CPU 1 enters a handler at
   `exception_base + 0x180` that counts the interrupt and clears the mailbox
   through IOCSR 0x100c. Measured: `mailbox=1 vector-pc=0xffffffff80000180
   status=0`, `latm-ipi: PASS`, `EPC` back at the interrupted instruction.
   Mutation-tested: removing the handler's clear makes it FAIL (status stays 1
   and the assertion sees it).

   **The "handler ran six times for one interrupt" anomaly is solved, and it was
   two bugs of my own making plus one model gap.** (a) The handler used `$t0`,
   which the interrupted loop was using as its counter base, so after `eret` the
   loop incremented the *handler's* word - one interrupt, one handler run, but a
   counter that kept climbing. A real handler saves what it uses; this one now
   uses `$s0`/`$s1`. (b) `$k0`/`$k1` cannot be used at all: the LATM register
   allocator keeps them and asserts `guest register writeback requires an itemp`
   on a guest write, which is a concrete constraint for any handler written for
   this port. (c) The IPI is level-triggered and the model never deasserted
   `Cause.IP4` after the mailbox cleared, so the board now clears the bit when
   `uncore.mailbox[cpu].status` is clear - without that the hart re-enters the
   handler on every dispatch. With all three fixed the measurement is
   `handled=1 loop=21` (the loop resumed past its pre-IPI 16), and the assertion
   is exact rather than "at least one";
   `iocsrwr` also needs major opcode 0x32 - the first encoding omitted it, the
   instruction decoded as something harmless and the platform hook was never
   called;
4. **in**: a shared reservation table (`target/mips/latm/llsc.c`,
   `latm-llsc.h`, one entry per hart) now sits behind the board's memory hooks -
   LL records, any store invalidates every *overlapping* reservation including
   the issuer's own, SC checks the table before the compare-and-swap and clears
   afterwards. Eleven host checks cover the semantics (cross-hart invalidation,
   overlap not equality, adjacent ranges untouched, own-store self-invalidation,
   address and width matching) and the board asserts the wiring end to end:
   hart 1's LL, an **idempotent** store from hart 0 (same byte value), then hart
   1's SC - `sc-ok=0`, memory unchanged. The idempotent store is the whole point:
   with a different value the compare-and-swap alone would fail the SC and the
   test would not prove the table matters. Mutation-verified: removing the
   invalidation makes `sc-ok=1` and the memory updated, and the board gate fails.

   A build trap repeated from the SMC blob work: `build/latm_llsc.o` was added to
   the link's prerequisite list but not to the command line, so the link failed
   with undefined symbols only after the fact - both places have to be edited.

| module | remaining work | priority |
|---|---|---|
| MIPS64 Linux | real vmlinux/initrd boot into `start_kernel()` | P0 |
| MIPS64 Linux, first attempt | **A real Debian kernel boots into its own code.** `linux-image-6.1.0-50-loongson-3` (6.1.176, mips64el, ELF64, entry `0xffffffff81490000`, one 5.28 MB load segment) is loaded through `LatmSystem` and executed by a local-only harness (`make latm-linux-run LOONGSON_KERNEL=<vmlinuz>`, kernel never vendored, not in CI, `LINUX_TIMEOUT` for the budget). It reaches its entry and runs its own BSS clear (`sw $zero,0($4); addiu $4,4; bne` at entry+0x20), which needs ~4.9 M three-instruction blocks. **Measured rate is ~1 000 blocks/s (about 1 ms per block), and 1 M blocks took ~1 000 s twice** - so the BSS clear alone is 80-90 minutes and throughput, not the budget, is the gate. `rdtime.d` is not usable as a clock on this QEMU either: the same 1 M blocks reported 26.0 M and 31.7 M ticks on two runs, so the harness now prints only raw ticks and ticks-per-block and leaves seconds to `time`. The cost has now been split with two measurements. **Translation plus dispatch only** (`make latm-linux-run LINUX_NO_EXEC=1`, the host call compiled out): 5 000 000 iterations in 125 s, i.e. 40 k blocks/s - so the lookup, the fast path and the harness loop are ~25 us per iteration and are *not* the problem. **With execution**: ~1 k blocks/s. And the emitted code is small: a 4-instruction guest block becomes **112 host words**, and `lata_gen_prologue_env`/`lata_gen_epilogue_env` emit **nothing at all** (both measured as 0 bytes), so the earlier vclock-in-prologue suspect is dead. What remains is ~975 us for ~112 emitted LoongArch instructions, i.e. **~9 us per emitted instruction under QEMU-LVZ** - three orders above what a LoongArch TCG JIT should do, which points at how the emitted code is laid out for QEMU (branch-dense 3-instruction loop = many host TB boundaries per guest block) rather than at the translator's code size. Counted with `-d exec`: **61 226 host TB executions for 2 000 guest blocks, i.e. ~30.6 QEMU TBs per 112-word emitted block** - the layout theory holds. A three-instruction guest loop therefore costs about thirty dispatch hops, which is why a 1 ms block is 30+ us per host TB. Two caveats recorded with the number: `-d exec` inflates the run ~47x (95 s for 2 000 blocks vs ~2 s untraced), so the trace gives the *count* and never the time; and `ticks-per-tb` printed 9 245 for that run, which is only comparable to other traced runs. `LINUX_TB_BUDGET` and `LINUX_DUMP_BLOCK` are build knobs now, so a 2 000-block experiment takes seconds and the first emitted block can be dumped as hex (`latm-linux: ` then 8-hex words, disassemble with `llvm-mc --disassemble` after byte-swapping).

**What the dump shows, and the fix it points at.** The 124-word block for 12 guest instructions contains **51 branch-class words** (top six bits in 0x10-0x1b): 24 `bceqz` and 19 `bnez`, against only 11 `addi`-class and 7 `ld`-class. So the block is not branch-dense because of guest control flow - it is branch-dense because **every guest-register access emits a run-time conditional**: "if this entry is dirty, store it back", "if it is invalid, load it", "if the value is already in this register, skip". All of those conditions are decided during translation, so most of them can be specialised away: emit the load/store unconditionally when the state is known, and only branch in the genuinely dynamic case. That is the concrete target (24 `bceqz` first), with the same `-d exec` count as acceptance: host TBs per emitted block must drop from ~30 without changing any ISA check.

**Correction: the 51 "branch words" were a measurement artefact.** The hex dump
printed nibbles low-to-high, so reading the words back by hand reversed every
word and the top-six-bit histogram was meaningless. Measured properly, at the IR2
level and on the kernel's own block shape (`sw $zero,0($4); addiu $4,$4,4;
bne $6,$4,-3` - $4/$6 are pinned host registers, so the register cache is not
involved): **0 IR2 branches either way**, but **15 host words with the system
memory path off and 71 with it on** (4.7x). So the per-instruction cost is not
branch density and not the register cache - it is the expansion the system memory
path adds to every guest access, which is exactly what the boot harness must run
(enabling it on the env-resident shape instead trips the `regcache_store` itemp
assertion, a separate pre-existing limit). `latm-isa` now pins both directions:
the sys path must cost extra words, and the loop must need no IR2 branch.

**Measured baseline for the accepted metric, and a correction.** With the boot
harness exiting through the LVZ exit device (it now does, so a timing run ends
when the work does), 2 000 translated kernel blocks take **87-125 ms**, i.e.
~20 k blocks/s - not the ~1 k/s inferred earlier. That earlier figure was an
artefact of measuring runs that never exited and were killed by a wrapper, so the
"1 ms per three-instruction block" claim is withdrawn; the block cost is ~45 us.

**The RAM fast path was implemented, verified, measured, and reverted - because it
did not help.** Second attempt, debugged in `latm-isa` instead of the kernel
harness (a second per iteration instead of a 40 s hang): the itemp exhaustion was
two temporaries held across `hcall()`, fixed by freeing them before the slow path;
with that, the bss-loop translates in both forms and the windowed form shows the
range test as expected (`branches 4` against `2`, `words 0x50` against `0x47` -
the fast path is *bigger* in code size because the helper stays inline for the
miss). Then the kernel A/B on the accepted metric: **196 / 121 / 111 ms for 2 000
blocks with the window, against 87 / 81 / 125 ms without** - no improvement, and
median slightly worse. So the 34-register spill around the helper is evidently not
where the block's time goes, and the extra range test plus pointer materialisation
costs more than it saves. Reverted whole (API, emission and test) rather than kept
as a correctness-verified change with no measured benefit.

**The helper counters are in, and they bound the question.** `latm_helper_stats_get()`
counts load/store helper calls (two increments, no behaviour change), and the boot
harness prints them with the block count: for the kernel's BSS loop it is
**exactly 1 store helper per block and 0 loads** - the block is `sw`, `addiu`,
`bne`, so the helper call is one of four guest instructions, not an occasional
event. Skipping the helper's memory work (mutation, reverted) gave 139/66/75 ms
against 183/72 ms with it: **inconclusive**, because the measurement's own spread
at 2 000 blocks (66-183 ms, i.e. 33-91 us/block) is wider than any effect being
measured.

So the next step is measurement quality before more optimisation: time the same
A/B at a workload where the noise is proportionally small (100 k blocks, seconds
rather than milliseconds), and only then attribute the ~45 us to helper, call
sequence or dispatch.

**`test_latm_tlb` is fixed and back in the gate.** The rule was missing
`-I shared/backend/loongarch/include`, so `latm.h`'s `mem.h` include failed and the
suite had silently stopped building (on HEAD too, which is why the gate looked
green). `make test-latm` now exits 0 with **14** `fails=0` suites including
`latm-tlb: checks=38`, plus `latm-boot-smoke: PASS`.

**Measurement quality is still the blocker, and the numbers now disagree by 3000x.**
Same build, same BSS loop: 100,000 blocks in ~95 ms (0.95 us/block), but a
6,000,000-block budget reached only 1,000,000 blocks in 300 s with the PC still in
the loop. Two runs of the same code cannot both be right, so the leading
hypothesis is TB-pool churn: the harness's 1 MiB pool holds 128 8 KiB slots and
the wrapper flushes the cache when it fills, so a long run may be re-translating
the same block instead of hitting. Instrumented: **2 translations for 100 000 blocks**, so TB-pool churn is
refuted - the dispatcher hits the cache and the block is not being re-translated.

**But the cost per block grows within a run, which no churn explanation covers.**
Same build, clean host (no other QEMU), progress every 100 000 blocks with the
guest PC and the tick counter:

| blocks | PC | ticks | ticks/block |
|---|---|---|---|
| 100 000 | 0xffffffff81490020 | 2.8 M | 28 |
| 500 000 | same | 15.5 M | 31 |
| 1 000 000 | same | 69.5 M | 70 |

and the wall clock agrees: 100 000 blocks in ~95 ms (0.95 us/block) but 1 000 000
blocks in ~60 s (60 us/block). The PC never leaves the kernel's BSS-clear loop, so
the guest is doing identical work; the per-block cost tripling over the run is
either host/QEMU state that degrades with time or something in the harness that
grows. Next diagnostic: sample ticks every 10 000 blocks to see whether the cost
grows smoothly or steps at a particular block count (a step points at the harness,
a smooth ramp at the host), and check QEMU's translated-block count from
`-d exec` at two points in the same run. Until that is answered, no per-block
number here is quoted.

**Answered: the per-block cost is constant, and wall-clock throughput on this host
is not measurable.** Sampling every 10 000 blocks with the tick counter shows
`ticks/block` flat at 45-55 from 20 000 to 1 050 000 blocks with the PC never
leaving the BSS loop - no ramp, no step, so neither host degradation nor a
harness-side growth. The earlier "28 -> 31 -> 70" was coarser sampling noise and is
withdrawn.

What is *not* usable is wall time: the same build and the same workload ran at
0.95 us/block in one measurement (100 000 blocks in 95 ms) and at 571 us/block in
another (1 050 000 blocks in 600 s), a 600x spread, and `rdtime` is not a clock on
this QEMU either. So performance work on LATM needs a **deterministic** cost
metric - emitted host words, IR2 opcodes, helper calls per block, which the harness
already prints - and wall clock only as a sanity check across many runs or on a
QEMU built with `-icount`.

**Solved: run with `-icount shift=auto,sleep=off`.** `scripts/run_qemu_lata.sh`
gained `QEMU_EXTRA_ARGS` and `make latm-linux-run` passes that flag, which makes the
guest's own counter deterministic: two consecutive runs of the same workload report
**identical** `ticks=0xb8b579`, `ticks-per-tb=0x3c` (60), `translations=2`,
`helper-stores=200000`, with wall time 3.75 s and 3.84 s. That is a cost metric safe
to compare across changes, and it retires the "no per-block number" ban.

Two side effects worth keeping: the harness now prints `latm-linux: done` and writes
the LVZ exit device, so a run returns in ~4 s instead of burning the whole QEMU
timeout (the wrapper waits for that token); **Correction after a longer run: the rate is not constant after all.** With
`-icount`, a 200 000-block run costs 3.8 s (52 k blocks/s) and reports
`ticks-per-tb=60`, but a 6 000 000-block budget reached only 1 050 000 blocks in
428 s (2.4 k blocks/s) with `ticks-per-tb` still 51. So the *virtual* counter stays
proportional while *wall* time per block grows 22x with run length - an effect that
`-icount` was supposed to remove, and which is now the open question. The earlier
"BSS clear is about 90 s" estimate is withdrawn; the measured cost of 1.05 M blocks
is 428 s, so a full 4.9 M-block clear at that rate is ~34 minutes and the 6 M-block
run was stopped rather than burning an hour.

**And then the wall-clock spread turned out to be a stall, not a rate.** Sampling
every 1 000 blocks shows every long run stopping at exactly **tbs = 1 050 000**,
with or without `-icount`: the kernel's BSS clear is 4 194 304 bytes = 1 048 576
words, so the loop *finishes* at that block count and the guest moves on to
`0xffffffff81490030` and then a `jalr $25` to `0xffffffff814911c8` - after which no
further block is ever dispatched. A single dispatch never returns there, and the
callee's first instructions are ordinary stores, so this is not an obvious guest
spin: it is the first concrete candidate for a **translator bug** on this path,
and it is cheap to chase in `latm-isa` (translate and run that block, one second
per iteration) rather than in the harness.

So the kernel now gets through its BSS clear and dies at the first call afterwards;
wall-clock "rates" on this host were measuring how long a stuck run sat there, not
throughput.

**And the block itself is innocent.** Rebuilt in `latm-isa` from the ELF bytes at
`0xffffffff814911c8` (daddiu $sp; lui $3; move $2,$4; lui $5; ld 0x2d50($3);
sd $19,0xb8($sp); sd $3,0x50($sp); lui $9), it translates to **0 IR2 branches and
191 host words** - straight-line code that cannot loop - and it is now a
regression check in the image (67 checks). So "a single dispatch never returns"
is not explained by this block: the stall is in the harness or dispatch layer after
the BSS clear, not in the translated code. Next step needs a per-dispatch watchdog
in the harness (print PC and helper state if a dispatch exceeds a bound) rather
than more guessing from outside.

**Root cause found, and it is not a hang: the translator aborts.** Translating
**32** instructions from `0x814911c8` in `latm-isa` (the same eight plus the rest of
the function, including its two backward loops) stops the image with
`assertion failed in <ra_alloc_itemp> ... out of IR2 itemps`. The pool holds three
host temporaries (t0/t1/t2 - every other LoongArch GPR is mapped to a guest
register) and `ra_alloc_itemp()` can only evict an entry whose temporary is not
currently borrowed; an instruction that already holds all three while asking for a
fourth cannot be satisfied, so the translator aborts. In the freestanding harness
that abort is a trap/halt, which is why no further block is dispatched and no
summary is printed - the "stall" was the translator dying on the kernel's next
function. Eight instructions translate fine (0 branches, 191 words), which is why
the narrow repro looked innocent.

**The aborting block is now identified, and it is not the one the ISA repro used.**
The harness prints the PC of every translation for its first 64 blocks; the last
line before the stall is `translate pc=ffffffff81491208`, and that block is a long
run of stack stores with **large negative displacements** (`sd $3,-0x7550($2)` and
friends) - exactly the shape that needs an extra temporary for the displacement on
top of the ones the `hcall` itself holds, which is the itemp starvation. Lowering
the block limit to 16 does not help (tried), so it is not block length but that
displacement+hcall combination.

**Fixed, and it was one operand class, not the displacement.** The ten
system-memory call sites materialised their `op` selector with `imm_opnd()`, which
allocates an itemp and holds it across `hcall()`; `hcall()` already stages
immediate operands itself, so that temporary was pure overhead - one itemp of the
three, for the whole call. Every site now passes a real `IR2_OPND_IMM` and the
matching `ra_free_itemp()` calls are gone. Result on the real kernel: **6 000 000
blocks in 2.4 s with no abort**, the last translated PC `0xffffffff81492b10` -
a million-block-scale run through its own code, where before it died at 1 050 000.

Gates after the change: `latm-isa` 66/0, `test-latm` 14 suites exit 0, and the
board's three SMP assertions (`latm-smp`, `latm-ipi`, `latm-llsc-ipi`) still pass.

**Regression added and the new stop point measured.** `latm-isa` now translates the
block that used to abort (`0xffffffff81491208`, sixteen stack stores with large
negative displacements and env-resident sources): 0 branches, 0x327 words, image at
**67 checks**. With that guard in place, a 40 M-block budget shows the kernel
running **6,324,226 blocks** and then jumping to a wild PC - the harness now reports
it instead of hanging on the translation attempt:

    latm-linux: bad pc=5ffb5495820b36ec at tbs=0x607002

So the BSS clear and roughly six million blocks of real kernel code execute, and the
next defect is a corrupted jump target (a register holding a non-canonical value)
rather than anything block-related. Next diagnostic: run with the guard in place and
watch the registers at the last good PC (`0xffffffff81492b10`), i.e. print the guest
GPRs when the PC leaves the image's range, and compare the jump with the kernel's
own disassembly at that address.

**Done, and it points at `jalr`'s link register.** The harness prints all 32 GPRs on
the bad-PC path; in the failing run the wild value sits in **`$31` (`ra`)** while
`$25` (`t9`) holds `0xffffffff81490050` - exactly the link address of the
`jalr $25` at `0xffffffff81490048` in `kernel_entry`. A minimal ISA test reproduces
it: `lui $25; ori $25; jalr $25; nop`, with the target placed in a separate array so
target and link differ, ends with `$31 = link` (correct) **and `$25 = link` as
well**, where `$25` must still hold the target. Suspect: for env-resident `$25`,
`alloc_gpr_src(25)` takes a temporary from the same pool as `imm_opnd()`, and after
`free_gpr_temp()` the link reuses that temporary while $25's cache entry still points
at it. **Fixed, and it was a real cache-lifetime bug.** `free_gpr_temp()` returned an
env-resident register's host temporary to the shared pool without touching its cache
entry, so the next allocation (the link's `imm_opnd()`) wrote into the same register
and `$25` silently became the link address. The entry is now released through
`latm_regcache_release_temp()`, which **writes back when dirty** and then invalidates
- the first attempt only invalidated and lost the guest's value (`$25` read 0), which
is why the writeback matters. `latm-isa` gains the `jalr-link` regression: `$31` =
link, `$25` = target, image at **69 checks**.

**But it was not what killed the kernel.** With the fix in, the same run still ends
at `tbs=0x607002` with `bad pc=5ffb5495820b36ec` - identical block count, identical
value. So the wild jump has another source.

**The IR2 scan points at spilling, not at a helper.** `LINUX_WATCH_GPR31=1` walks the
IR2 list of every translation and counts `st.d` against the env register with
immediate `env_offset_gpr(31)`. The epilogue block at `0xffffffff8149051c` emits **six**
such stores even though the guest code has exactly one `ld $ra,0x18($sp)`, and
`0xffffffff814904fc` emits four. `$31` is therefore spilled repeatedly inside one block,
each spill reading whatever host temporary the cache entry currently points at, and
one of those six writes is the garbage seen at runtime. That is a lifetimes problem
again, but in the *spill sequence* rather than at a helper call - which is why the
blanket fixes failed: they targeted helper entries, not these.

Next step was to dump that block's IR2 sequence around the six `st.d` ops.

**Done, and the six spills all read host register 1 - `la_ra` - where guest `$31` lives
by design.** That is the conflict: `hcall()` emits `li_d(f, fn); jirl ra, f, 0`, and
`jirl` writes the *host* return address into the same `$ra` that is hosting guest
`$31`. The prologue spills guest `$31` before the call and the epilogue reloads it
after, which is why most calls survive - but any path that reads `la_ra` between the
`jirl` and the reload sees the host link address instead of the guest return address,
and that is exactly the high-entropy value the kernel ends up returning through.

A first attempt to fix it by mapping guest `$31` to `NOT_DEF` (so it lives in the
cache) fails one ISA check - `jal link = branch+8` reads 0 - because the `jal` exit
path relies on that fixed mapping for its link store. Reverted.

Tried that too: `hcall()` now saves `la_ra` to `env->gpr[31]` immediately before the
`jirl` and reloads it immediately after. **The kernel's result is unchanged** - same
block, same `0x5ffb5495820b36ec` - so the `la_ra` window is not where the value comes
from either. Reverted (the two extra instructions bought nothing).

Five hypotheses have now been eliminated with measurements (stack contents, `jalr`
link, helper env pointer, helper PC, exception/trap path, frame loads, `la_ra`
clobbering), and every blanket fix has either failed the ISA suite or changed nothing.
The next attempt should stop looking for a writer and instead **freeze the state and
replay it**: stop the harness at `0xffffffff8149051c`, dump all 32 GPRs plus the frame
page, and replay that block in `latm-isa` by translating first and *then* loading the
GPRs into `env` before entering through the normal `cs_bt_to_native` stub (which is
what the earlier attempt skipped, which is why it saw `$sp == 0`). A failing replay there is a two-second loop instead of a 90-second kernel run.

**The rig is built, its control passes, and it acquits the block.** `latm-isa` now has
`replay_block()`: translate the block, *then* load the guest GPRs into `env`, then enter
through the normal `cs_bt_to_native` stub (the step an earlier attempt skipped, which is
why it saw `$sp == 0`). Control block `ld $ra,0($sp); jr $ra` returns through the frame
value as expected (`ra = pc = replay_far`). The kernel's epilogue replayed with the
frame the write watch recorded returns **`ra = pc = 0xffffffff81490528`** - correct.

So the block's translation and the rig agree: the corruption is in the *frame content*,
not in the epilogue. And the poison dump already showed that: at crash time the slot at
`$sp+0x18` held `9026260403a1133a`, while the write watch logged the kernel storing
`0xffffffff81490528` there. The watch was capped at **16 entries**, so a later writer
(the 17th and beyond) was never recorded - that is now the specific gap to close:
watch only the exact slot with a larger cap and see which write lands the garbage.

**Closed: `movn` had its masks backwards, so the copy length was never clamped.**
The exact-slot watch (GPA `0x1e9add8`, the address `ld $ra,0x18($sp)` actually reads;
the old dump's `$sp+0x18` was *after* `daddiu $sp,$sp,0x20`) records eight `sb`s from
`0xffffffff81492b28`. That is the byte-copy loop a `jal` at `0xffffffff814904f4`
calls. Little-endian, the eight bytes are `0x5ffb5495820b36ec`, and those same bytes
sit in the kernel image at `src + index`. The epilogue then loads them. Neither the
copy nor the epilogue is wrong.

At the copy's entry the arguments are dest `0xffffffff81a98f88`, src
`0xffffffff81492d90`, len `0x505d18`. The object span is `0xc`
(`[0xb0] - [0xa8] = 12 - 0`) and `sltu` writes 1, but `s0` stays `0x505d18`. Replaying
those 11 instructions in `latm-isa` with the same memory gives the same split:
`a0 = 1`, `s0` unchanged. `maskeqz` keeps `rj` when `rk != 0`; `masknez` keeps `rj`
when `rk == 0`. The emitter had the two masks swapped, so `movn` copied only when
the condition was zero and `movz` did the opposite.

Swapping them makes the replay clamp to `0xc` (`latm-isa` checks=118, fails=0, plus
both `movz` directions). The same boot now enters the copy with `a2 = 0xc`, never
poisons `$31`, and runs the whole 40,000,000-block budget. It then sits in `b .` at
`0xffffffff81491b18` — a kernel halt loop, not a wild PC.

**The spin is table entry 7, and the table itself is intact.** Nothing in the image
branches to `0xffffffff81491b18` except the instruction. The `jr $2` at
`0xffffffff81491a8c` indexes `0xffffffff81492d08` by `$17`. First dispatch of
`0xffffffff81491a7c` (tb `0x10132f`): `$17 = 7`, `$2 = 0xffffffff81490000` (the
`lui` in the delay slot), base `= $2 + 0x2d08 = 0xffffffff81492d08`. The nine
qwords in RAM match the image; entries 7 and 8 are both that `b .`. So the base
is right and the index really is 7.

**That 7 is the delay slot at `0xffffffff81491abc`.** The block that last changed
`$17` before the table starts at `0xffffffff81491aa8`, guest size `0x18`, so it
covers `[81491aa8, 81491ac0)`. The only write to `$17` in those six instructions is
`addiu $17, $zero, 7` in the delay slot of `beqz $2, 0x81491a34`. The branch was
taken (`next = 0xffffffff81491a34`) and the value left behind is 7. The other
`addiu` at `0xffffffff81491a30` is not in the block. The kernel itself plants the
index; the table entry for 7 is the spin.

**The watchdog found the writer.** The harness now checks `$25` and `$31` on every
dispatch and reports the first non-canonical value together with the PC that had just
executed:

    poisoned gpr[31]=5ffb5495820b36ec after pc=ffffffff8149051c at tbs=0x607002

`0xffffffff8149051c` is a function epilogue: `ld $17,8($sp); ld $16,0($sp);
ld $ra,0x18($sp); jr $ra; daddiu $sp,$sp,0x20`. `$ra` is therefore *loaded from the
stack* and that slot already held the garbage - not a register-allocation bug but a
corrupted stack frame, and the value is high-entropy rather than a stale pointer,
which suggests an unwritten slot or a store to the wrong address. Next diagnostic:
trap writes to the frame slot.

**Done, and the stack is innocent - but the helper's guest PC is not.** With a
write watchpoint on the frame page the only writers are the kernel saving exactly
what it should (`0xffffffff8149051c`, `...0520`, `...0524`, `...0528` - the return
addresses of the epilogue above), so the frame contents are plausible and the wild
`$ra` does not come from the stack.

What the watch did expose is that **inside the store helper, `env->active_tc.PC`
holds the store address, not the guest instruction address**:

    watch write gpa=0x1e9adc0 pc=0x1e9ade0 val=0xffffffff81490020
    watch write gpa=0x1e9adc4 pc=0x1e9ade4 val=0xffffffff81490020

`gpa` and `pc` advance together, so the PC field tracks the data address. Any helper
that reads or saves the guest PC - a faulting access, `GETPC()`-based exception
posting, anything spilling registers - then records nonsense, which explains a
high-entropy `$ra` far better than a corrupted stack. Next step: check how
`latm_set_pc()` and the `hcall()` argument staging interact.

**Answered, and it is neither: the PC in a helper is the block-entry PC by design.**
The translator records the PC it programs and the store helper records what it sees:
`env` is exactly the harness's env pointer (`0xc64e120` both ways), and the PC seen is
a *valid guest address* (`0xffffffff81491214`), just not the current instruction
(`0xffffffff8149052c`), because `latm_set_pc()` runs at block entry rather than per
instruction. So the earlier "PC tracks the data address" reading was a coincidence of
the particular store addresses, and the argument staging is fine. The debug taps were
removed again afterwards: they cost a comparison on every store helper call in the hot
path and bought an answer, not a fix.

That leaves the wild `$ra` still unexplained, with these eliminated: the stack (write
watch), the `jalr` link register (fixed and regressed), the env pointer in helpers, and
the helper PC. The most promising remaining lead is what the *epilogue block itself*
loads: `ld $ra,0x18($sp)` reads a plausible saved value (`0xffffffff81490528`) as far
as the watch can see, so the next step was to log the value at the moment of the load
inside `latm_helper_load` for that one address. Done, and it recorded **zero loads**
in the watched page over the whole run. With `$sp` at load time being
`0xffffffff81e9adc0` (the post-epilogue dump minus its `daddiu`), those loads would
land at `0x1e9adc0/0x1e9adc8/0x1e9add8` - all inside the watched range - so the
epilogue's loads are apparently **not reached at all**. That moves the hypothesis from
"the load returns garbage" to "the block traps before its loads", after which `$31`
simply keeps whatever it had. The tap cost a comparison on every load helper call and
was removed again; next step was to instrument the exception path.

**Done: no exception fires at all, and the block's frame loads never go through the
helper.** The exception tap (PC, EPC, BadVAddr, cause code for the first eight
raises) recorded **zero raises** over the whole 6.3 M-block run, and the earlier
load tap recorded zero loads in the frame page. So the epilogue neither traps nor
performs helper loads: its `ld` instructions are inlined, and `$31` ends up with
0x5ffb5495820b36ec through some other path - the strongest remaining candidate is
another instance of the register-cache lifetime class fixed for `free_gpr_temp()`,
i.e. a value surviving in a host temporary after it was handed to a different owner
(`alloc_gpr_dst()` reuse, `store_gpr_zero`, or the `latm_spill_*` slots around the
delay slot).

**Two fix attempts were tried and both are wrong; the ISA suite is the proof.**
(i) A unified release inside `ra_free_itemp()` (any guest register still claiming the
returned temporary is written back and dropped) breaks **18 of 69** checks, including
`jalr`'s link - the macro is called in places where that claim is legitimate, so a
blanket release destroys live state. (ii) Deleting the spill loop after
`latm_regcache_flush()` in `lata_gen_call_helper_prologue()`, on the theory that it
overwrote env with a stale host `$ra`, also breaks 18 checks - those spills are
load-bearing. Both were reverted; the tree is back at the committed, green state.

So the `$31` corruption is not the register-cache lifetime class after all. The next
targeted step is a **write watch on `env->gpr[31]`** itself (not on guest RAM): the
value must be written either by a spill, by a writeback, or by an `env_st_d` in the
emitted code, and the watch will name the emitter.

The exception tap stays in the tree: it costs a branch only on the raise path, and
without it "no exception" would have remained a guess. `test_latm_interrupts` also
needed the same `-I shared/backend/loongarch/include` fix as `test_latm_tlb` had. `latm-isa` keeps the 8-instruction case at `0x814911c8` as the green
regression, and `KERNEL_BLOCK_INSNS=<n>` is a build knob for bisecting block
limits (16 translates, 24 aborts at the first PC). That is the same conclusion the RVV and LATA work reached by a different
route: count what you emit, do not time what you emulate.

**The earlier attempt, kept because its failure mode is instructive:** `latm_set_ram_window()` plus an
inlined LD/LW/LWU/SD/SW path (range test, then an indexed access into the host
buffer, helper on the miss) reached two blocking problems: the first version
exhausted the IR2 itemp pool (`ra_alloc_itemp: out of IR2 itemps`) because it held
three at once alongside `hcall`; reduced to two it translated, but the kernel then
hung with no summary at all. Rather than keep an unverified core-translator change
in the tree it was reverted whole (API included), and the next attempt should
debug it **in `latm-isa`** on the bss-loop shape with the window set - that image
can dump the emitted block and assert the fast/slow split in a second - instead of
in the kernel harness, where every iteration is a 40 s hang.

**Where the 71 words come from, measured in the code.** A sys-memory access is a
single `hcall2(latm_helper_load/store, op, addr)`, and `hcall()` wraps every call
in `lata_gen_call_helper_prologue()` / `epilogue()`, which spill and reload **all
17 guest registers that are mapped onto a1-a7 / t3-t8 / ra** (the ABI registers
the C helper may clobber) around each call: 34 memory operations, about the whole
cost of the block. So the fast path is not a micro-edit: either the translator
learns the RAM window and inlines the access (needs a RAM pointer/window handed
to it, which the design doc's "hand the physical root complex to the guest"
already argues for), or the spill set shrinks to what each helper really touches.
The first is the one that reaches the 15-word figure; the second is a smaller win
with a correctness argument per helper. Neither is a one-line change, and both can
be judged by the same two numbers already pinned in `latm-isa`.

**The earlier synthetic test failed to reproduce the density, and that was useful
too.** `latm-isa` now has `test_regcache_specialisation` (2 checks, image at 62) which translates two hand-written shapes on an env-resident register and counts IR2 branches and host words: a load-modify-load of the *same* register and of two *different* registers both come out at **0 branches and 21 words**, and the constant-store pair at 0 branches and 10 words. So the register cache's own specialisation is already good, and the 43 conditional words in the kernel block come from somewhere else. Turning the system-memory path on for the same shape aborts the translator instead (`regcache_store` itemp assertion), so the next measurement must be taken **on the kernel's own block**: dump its IR2 opcodes (the ISA test already walks `first_ir2`/`ir2_next`) rather than guessing shapes. Recorded so the synthetic-test route is not repeated. Two other measured facts: a 256 MiB `.bss` yields a 303 MB `memsz` segment and QEMU's LoongArch `-kernel` loader then runs the image with **no serial output at all** (64 MiB works), and this PC is the ready-made benchmark for 5.2 chaining |
| SMP | four vCPUs, IPI, cross-core TLB, reservation coherence | P0 |
| 3A4000 platform | IOCSR/EXTIOI/PCH behaviour aligned with Linux drivers | P0 |
| 7A PCI | BAR, MSI, real device IRQ, DMA ownership | P1 |
| Loongson MMI | 128-bit vector ops mapped to LSX/PVIR | P1 |
| Loongson extensions | AMO, special encodings, differential against hardware | P1 |
| LEFI/firmware | real Loongson boot ABI | P1 |
| 3A3000 | HT, LIOINTC, different topology | P2 |
| translation performance | jirl/jiscr, register cache, TB chaining | P2 |

Handing the physical root complex to the guest remains the right entry point for
LATM: existing Loongson Linux drivers keep working. It removes device
*emulation*, not the need for interrupt routing, DMA isolation, device
ownership, config space and address-window virtualisation.

## 2.4 LATX: from OVMF to a real OS

| module | work |
|---|---|
| LAPIC/x2APIC | IRR, ISR, TPR, PPR, EOI, timer, IPI |
| IOAPIC | IRQ routing, level/edge, remote IRR |
| CPU discovery | CPUID, MSR, ACPI MADT consistent with behaviour |
| PCIe | ECAM, BAR, MSI/MSI-X, bridges |
| time | TSC, HPET, PIT, RTC on one virtual clock |
| storage | virtio-PCI or AHCI to finish the OS loader |
| isolation | checked-access and helper memory access completed |
| Windows | only after a full OVMF boot |

Structural problem: `baremetal/bare/tb_sys.c` is 11,144 lines mixing tests,
paging, trap entry, device handling and firmware glue. Production runtime code
must move out; `tb_sys.c` becomes an integration harness only.

## 3. Immediate P0 fixes (source-verifiable, do before new features)

### 3.1 Guest DMA cache integer overflow - FIXED

`src/guest_dma.c:17-72`: the cache-hit test is

```c
gpa + len <= page + entry->span
```

with no overflow check before the fast path. The slow path
(`loongemu_mem_resolve`, `src/phys_mem.c:140-164`) rejects overflow and
`len > avail`, so the hole is exactly the fast path: a valid entry for a RAM page
plus a huge `len` makes `gpa + len` wrap and the test pass, and the following
`memcpy` then writes past the region.

Status: fixed in `src/guest_dma.c` (guard `gpa > UINT64_MAX - len` at entry, and
the hit test expressed as subtraction), with negative tests in
`tests/test_riscv_virtio_blk.c`. Checked against the pre-fix source: the old code
crashes on the new test (a `write()` with `len = UINT64_MAX` memcpy'd past RAM),
the fixed code passes 115/115.

Required form (subtraction, and a guard at entry):

```c
uint64_t offset = gpa - entry->gpa;
if (entry->valid && entry->generation == dma->map->generation &&
    gpa >= entry->gpa && offset <= entry->span &&
    len <= entry->span - offset) { /* hit */ }
```

plus `gpa > UINT64_MAX - len` rejected up front, plus negative tests for both
hit and miss.

### 3.2 DMA write versus LR/SC atomicity boundary - CONTRACT IN PLACE

`src/guest_dma.c:95-119` writes the data and only then calls `dma_notify`. Under
the current cooperative scheduler this is tolerable; with real parallel vCPUs the
store and the reservation invalidation have no shared boundary.

Target: one `GuestWriteTransaction` contract used by CPU stores, AMO, SC, device
DMA and invalidation notifications:

```
guest_write_begin(domain, gpa, len);
guest_write_commit(domain, gpa, data, len);
guest_write_end(domain);
```

Status: **closed for RAM stores.** `loongemu_guest_write_begin/commit/end` exist
and DMA write/fill/write_le route through them; the notification is issued at
`end()` while the lock is still held. Nested `begin()` is refused and `commit()`
outside the reserved range is refused, both tested. The remaining half - CPU
stores sharing that critical section - is now in:

* the domain is the reservation word itself (`latr_guest_store_lock`: bit 0 the
  writer lock, upper bits the epoch), which emitted stores, AMOs, SC, the C
  misaligned path and device DMA all take;
* `LoongemuGuestDma` gained `lock`/`unlock` hooks
  (`loongemu_guest_dma_set_write_domain`), installed by `latr_system_init` against
  `latr_reservation_lock/unlock`, so a device write holds the same domain as a
  CPU store: data store and epoch bump are one critical section;
* the acquire **blocks**. That is only safe because no CPU store path holds the
  domain across a device call - the emitted fast path already called MMIO outside
  it, and the C misaligned path was restructured to match - so a device that DMAs
  synchronously cannot deadlock. Nested device transactions are refused by
  `write_active` rather than by a non-recursive lock;
* `tests/test_guest_write_domain.c` (host, in `make test`) drives the hooks with a
  real second thread: the domain is taken and released around a write, a nested
  `begin` is refused, and an observer holding the domain never sees a torn device
  write. Bypassing the hooks makes it fail (verified), so the test is load
  bearing rather than decorative.

Measured caveat, stated rather than hidden: multi-byte guest writes are not
atomic to a *guest* (RISC-V only guarantees aligned ≤ XLEN); what this buys is
that a device write cannot interleave with a CPU store inside one critical
section, and that its reservation invalidation is inside that same section.

### 3.3 TB recovery ABI must be one contract - DONE (schema + decisions)

All four translators recover guest state differently. The shared record must
carry: `guest_pc`; `host_pc_range`; `register_bindings`; `flags_state`
(EFLAGS/NZCV materialisation); `commit_state` (partially executed); `arch_payload`
(MIPS BD, RVV `vstart`); `tb_generation` (so a mapping cannot survive arena
reuse). This is the prerequisite for ValueVersion, chaining, the persistent cache and
AOT regions.

Status: `shared/backend/loongarch/include/le-recovery.h` now carries every field
in that table - `guest_pc`, `native_start/end`, `aux` (arch payload: MIPS delay
slot/BD, RVV vstart, AArch64 raw length), `effect_id`, `site`/`phase` (commit
state), `bind_*` (register bindings), `flags_state`
(`LE_FLAGS_NONE/MATERIALIZED/DEFERRED`) and `tb_generation` - plus one decision
function, `le_recovery_can_project()`, so "may this host fault become a guest
trap?" cannot drift per port. All three ports assert at compile time that their
span row is the shared struct (`_Static_assert`), each populates
`tb_generation`/`flags_state` in its map insert, LATR's resolver ignores rows
whose generation disagrees with their map, and the LATR host-fault path refuses
deferred-flags spans it cannot replay. `tests/test_le_recovery_abi.c` pins the
fail-closed cases (20 checks, in `make test`).

## 4.1 Modules to keep promoting

| module | current base | next step |
|---|---|---|
| PVIR | in `shared/pvir` | unified vector planning/lowering; LATM MMI |
| IR2 | shared backend | opcode metadata, encoder, side effects unified |
| RegCache | per-port logic | shared ValueVersion |
| TB cache | shared + port-specific | unified lifecycle, keys may stay per-port |
| code arena | LATR is the mature one | LATA/LATM reuse it |
| fault recovery | per-port | shared recovery ABI (3.3) |
| memory | shared phys mem + DMA | one checked/direct/atomic access contract |
| SoftRAS | shared | ISA-specific call/return adapters |
| device model | several machine models | unified bus, IRQ sink, DMA domain |

For PVIR, the next batch worth adding: reduction, shuffle, gather, compress,
widening MAC, fixed point - they serve RVV, SVE and Loongson MMI, and later
x86 SSE/AVX.

## 5.1 IR2 ValueVersion / Next-use

Worth doing, and before any complex AOT. The regcache already avoids some
redundant loads; the real problem is control-flow-boundary flush, state
materialisation and their relationship with recovery.

Order (P0 already measured - see `docs/design/ir2-value-version.md`):

1. count flush/writeback/reload/spill (done: 94-98% of flushes are no-ops,
   `writebacks == stores`, `evictions == 0`);
2. shared ValueVersion and canonical state - **retired by the measurement**, the
   double-writeback case it targeted does not occur;
3. conservative flush elision at internal labels/branches;
4. next-use and dead-write elimination - **retired by measurement** (see
   `ir2-value-version.md` §3.2b): `dead_stores == 0` in both workloads, so there
   is nothing to eliminate, and no writeback happens at a label/exit/block-end
   flush (all 282/605 happen at helper-call boundaries where canonical state is
   mandatory). The remaining next-use work is load hoisting across an edge,
   which needs the successor - i.e. chaining, step 5;
5. cross-TB register residency after chaining exists.

Software NZCV also belongs in this frame: N/Z/C/V as guest state materialised
lazily.

## 5.2 TB chaining and RAS

Keep the two-step order: C-side redispatch first, generated-code chaining second.

| tier | path |
|---|---|
| 3 | direct TB chaining: fixed target, epoch guard, bounded execution |
| 2 | indirect jump cache / RAS: dynamic target, return prediction |
| 1 | fast C redispatch: known target, dispatcher duties preserved |
| 0 | full dispatcher: interrupt, exception, invalidation, reclaim, translate |

Fall back a tier on prediction miss, epoch change, pending IRQ or budget
exhaustion. Never skip WFI, interrupt, SMC, timer, shootdown or precise recovery
for the sake of chaining. `jiscr0` stays an optional LBT-host accelerator; the
`jirl` fallback must always exist.

## 5.3 Persistent code cache

Advance to the relocation test, do not build the whole disk cache yet:

```
translate at arena A -> serialize code + relocations + recovery
  -> relocate into arena B -> execute from B -> compare architectural state
```

Also required: PVIR descriptors, constant pools and recovery metadata join the
lifetime checks; the cache key includes guest code content, ISA profile,
translation mode, host features and backend ABI; SHA-256 detects corruption but
does not authenticate the producer, so an untrusted cache must be refused or
authenticated.

The long-term value is not cold start but a relocatable code format shared by AOT
region compilation and profile-guided DBT.

## 5.4 MMIO / MSI-X / DMA isolation - two corrections

1. **PLIC is an internal interrupt adapter, not a RISC-V MSI architecture.** A
   real PCIe MSI path needs a platform MSI controller such as IMSIC.
2. **virtio-mmio keeps its standard MMIO interrupt.** MSI-X belongs in a separate
   virtio-PCI transport, not bolted onto virtio-mmio.

Layering:

| layer | modules |
|---|---|
| transport | virtio-mmio / virtio-pci |
| device | virtio-blk / net / console |
| IRQ | PLIC / IMSIC / LAPIC / EXTIOI |
| DMA | guest DMA domain / IOVA translation |
| physical passthrough | host PCI backend + IOMMU |

## 6. Physical root complex takeover

If the goal is to emulate as few devices as possible on real Loongson hardware,
a passthrough manager is a separate layer:

```
guest OS drivers -> LoongEMU passthrough manager
  (config filter, BAR/address windows, IRQ remap, DMA/IOMMU, device reset,
   ownership/errata) -> physical Loongson root complex / LS7A
```

Open problems: LS7A bus mastering, EXTIOI/PCH MSI routing, device reset and
host-reserved devices, physical memory windows, cache coherency. Without an
IOMMU or equivalent, this is limited to trusted guests; PLV3 plus shadow paging
is not a DMA security boundary.

## 7. CI and verification

`isa-lata` now runs in the same Debian container as `isa-latr`/`isa-latm`
(`.github/workflows/ci.yml`), so the QEMU version and LSX/LASX support match;
the earlier runner-local `apt-get` failure is fixed. What is still missing is
architecture coverage beyond fixed fixtures:

| test type | coverage |
|---|---|
| ISA differential | four guest ISAs against QEMU |
| random instruction | **done for RV64 integer**: `make latr-rand-diff` (fixed seed) and `latr-rand-sweep` (3 more seeds) run one generated R-type stream on LATR and on qemu-system-riscv64 and compare a 64-bit hash of x5..x31. FP/vector streams and illegal-encoding fuzzing are still open. |
| fault differential | **done for the M-mode core cases**: `make latr-fault-diff` runs one guest (latr_fault_guest.S) on the LATR machine and natively on qemu-system-riscv64; it takes load/store access faults, an instruction access fault, `ecall` and an illegal instruction, the handler records (mcause, mepc, mtval), and the hash plus the raw records must agree. Page faults, misaligned access and privilege checks are still open. |
| memory litmus | TSO, RVWMO, ARM ordering |
| SMP stress | LL/SC, AMO, DMA, shootdown |
| SMC | **cross-page done in the SMP image**: an instruction straddling the 0x80001000 boundary (first two bytes below, last two above) is called (1), rewritten with two halfword stores, `fence.i`'d, and called again (3); the raw bytes are also checked. Cross-core visibility of the *straddling* instruction and alias mappings remain open - see the note below. |
| relocation | identical results from different arena bases |
| feature matrix | scalar, LBT, LSX, LASX |
| OS boot | pinned Linux + Buildroot/BusyBox |
| native hardware | 3A6000 and a LoongArch host without LBT |

Random-instruction differential (implemented): `tools/gen_latr_rand_vectors.py`
emits, from one fixed seed, `build/latr_rand_vectors.h` (C array + initial
x5..x31 + hash constants) and `build/latr_rand_run.inc` (the same words as
assembly plus a hash loop), so the two engines cannot drift apart. The stream is
48 non-trapping R-type ops (`add/sub/sll/srl/sra/slt/sltu/xor/or/and`, the M
multiply/divide family, and the W variants) with random registers; division by
zero and `INT_MIN / -1` occur on purpose. Both sides hash x5..x31 with
`h = (h ^ x) * prime` and print `latr-rand-diff=<hex>`; `make latr-rand-diff`
fails on a mismatch. Seeded sweeps (`LATR_RAND_SEEDS`) multiply the coverage at
one rebuild and one QEMU boot per seed.

Two findings from building it, both recorded because they are the kind that
wastes an afternoon:

* **QEMU's `-kernel` entry is 0x80000000, not the ELF entry point.** Placing the
  generated runner above `_start` in the source file put it at 0x80000000 and the
  guest executed it as boot code - illegal instruction, trap loop, no output. The
  include now sits at the end of the file with a comment saying why.
* **`funct3` is bits 14:12.** The first generator shifted it by 11, which
  overlapped `rd`'s high bits: 48 "random" words, of which QEMU accepted the
  ones that happened to be legal. The differential found its own generator bug
  before it found any translator bug - which is exactly what a differential is
  for.

Fault differential (implemented): the guest installs an M-mode `mtvec`, parks the
resume PC in s0 before each faulting instruction, and the handler records
(mcause, mepc, mtval), steps mepc to s0 and `mret`s. Both engines execute the
*same* flat image (LATR loads it as firmware at 0x80000000 and runs it through the
translator; QEMU takes it as `-kernel` at the same address), and both print the
hash and the five raw records, so a mismatch is diagnosable from either log.

What is compared is what the spec pins: cause and mepc for every case, and mtval
only for the three access faults (the faulting address). mtval for
illegal-instruction is explicitly allowed to be the instruction or zero, so
hashing it would flag a permitted difference - the kind of gate that gets
disabled rather than read.

Two findings from the first runs, both in the test rather than the translator:

* **0x40000000 is mapped on QEMU virt.** The "unmapped" load case silently
  succeeded there and turned into a fetch of garbage, while LATR (which maps only
  RAM plus its device windows) faulted correctly - so the differential's first
  mismatch was its own address choice. It now uses 0x90000000, above RAM on both
  engines.
* **An all-zero `.word` is two RVC halfwords**, and 0x0000 is the reserved
  `C.ADDI4SPN`, so the two engines may legitimately disagree about where the
  instruction ends. The illegal case is now `0x0000007f`, a reserved 32-bit
  opcode.

SMC across a page boundary (implemented in the SMP guest): the instruction at
0x80000FFE has two bytes in each 4 KiB page, which exercises the rule that only
the *first* instruction of a block may cross (`target_disasm`) plus the fetch and
`fence.i` paths. It is rewritten with two halfword stores (a word store there is
misaligned), fenced, and re-called: 1 before, 3 after, with the bytes checked in
memory.

**Closed as a non-defect, with the measurement that does it.** With a probe guest
whose disassembly was verified first (the handler really contains both `jalr`:
`0x80000188` is the second call, `0x8000018c`/`0x80000190` store `a0`/`ra`,
`0x8000019c` publishes the done flag), the joint dump `(entry->code, env->gpr[10])`
per entry gives:

```
X h1 code=...ce8 a0=1 ra=0x8000014c   pre-rewrite call, old body
X h1 code=...de8 a0=1 ra=0x8000017c   handler call 1, retranslated body
X h1 code=...de8 a0=2 ra=0x8000018c   handler call 2, same body
P second=2 ra=0x8000018c flag=0x1234
```

The second call returns 2, its `ra` is right, and the done flag is set. The two
distinct bodies of `0x80002000` are the **old translation and its retranslation**
- they differ in exactly one word because the only difference in the guest code is
the `li a0, N` constant - which is what invalidation plus retranslation is supposed
to produce, not a defect. So the second call through a trap handler into a
re-translated block works, and the whole "returns 0" line is withdrawn rather than
left as an open bug.

What is still not explained is the origination of that hunt: the very first
observed 0 came from a probe whose guest was never disassembly-checked, and no
verified probe has reproduced it. If it resurfaces, it should be treated as a new
observation with the rule below applied from the start.

**The rule, which is the durable output of this hunt:** disassemble the probe guest
before trusting a probe run. A done flag proves hart 0 waited; it does not prove
the instruction that writes the result word exists. `llvm-objdump -d
build/latr_smp_guest.elf` is one command and would have caught all four probe
errors: the data page overwriting the page-straddling instruction, the two
register mix-ups, hart 0 powering off before the store, and the edit loop that
deleted the two-call block.

Litmus tests were considered and deliberately not added: with the cooperative
image the interleaving is deterministic, so MP/LB would pass vacuously and say
nothing about ordering. They need a host that can run generated code
concurrently.

Unified benchmark (implemented): `make latr-bench` prints one table and writes
`build/latr_bench.md` from the logs the targets already write, and CI runs it so
the table lands in every push's log. It is report-only on purpose - re-running a
target from inside the reporter would make the numbers depend on the reporter's
idea of freshness - and each section states which log it read and how old it is.
Deterministic counters (checks, blocks, writebacks, chain links, diff hashes) go
into a fingerprint; host nanoseconds stay out of it, because they are wall clock
on whichever machine ran them.

Baseline (HEAD 30fd024, host = Apple silicon, guest OS = QEMU LoongArch):

| section | metric | value |
|---|---|---|
| ISA image | checks | 1771 |
| | regcache flushes / elided / writebacks / reloads | 14531 / 14020 / 330 / 416 |
| | code-symbol sites | 552 |
| | random diff | dab61bfe2223bc9e |
| SMP image | checks / harts dispatched | 19 / 46+44 |
| | chain links h0 / h1 | 1 / 34 |
| | fault diff | c482341e1af54e1d |
| guest boot | regcache flushes / writebacks / reloads | 10022 / 605 / 776 |
| dispatch (host) | lookup / full dispatch | 1 ns / 26 ns |
| | of which clint tick / interrupt refresh / safepoint / bind | 1 / 6 / 3 / 2 ns |

Not reported, and why: `guest instructions/sec` needs a LoongArch wall clock the
images cannot produce, and `host instructions per guest instruction` needs host
instruction counters. Printing a number that measures QEMU instead would be
worse than the gap.

Performance moves from local PERF counters to one benchmark reporting: guest
instructions/sec, host instructions per guest instruction, TB hit rate,
translation time, writebacks, helper call rate, chaining hit rate, MMIO latency,
fault recovery cost.

## Order of work (single list)

1. 3.1 DMA cache overflow fix + negative tests - done.
2. 3.2 GuestWriteTransaction contract (coarse lock), DMA routed through it - done;
   CPU stores/AMO/SC still to adopt it.
3. 2.1 LATA `translate_tb` capacity ABI, whole-TB reservation, reclaim, I-cache
   sync ownership, safepoint enforcement and SMC epoch - done; next is reusing
   LATR's arena implementation (item 13) and cross-core invalidation with real
   SMP (item 8).
4. 3.3 shared recovery ABI - done (schema, decision function, generation guard).
5. 2.1 LATA host-fault precise recovery - done (`lata_host_fault` uses the shared
   decision, a generation guard and binding replay; the tests cover projection,
   a missing snapshot, a stale generation and a native-internal fault), together
   with EL/vector delivery (`lata_take_exception`), so a projected fault can now
   land in the guest's own EL1 handler. What remains for a full boot contract is
   GIC and the generic timer.
6. 5.1 step 3 (flush elision) - done: per-bank dirty/resident counts let a clean
   bank skip its writeback scan and an empty bank skip its release loop
   (entries visited per flush 128 -> 1.9 on the ISA image, 128 -> 4.9 on the
   guest boot; writebacks unchanged, so semantics are identical). Call-site
   elision at the ten hand-placed flushes stays open and needs its own
   measurement first.
7. 5.3 relocation round trip - done: same-symbol round trip at a second base with
   the original arena poisoned (4 spans, 245 words, guest state identical). It
   also measured the blocker for the disk cache: `li_d` is variable-length, so
   all four spans change width at an ASLR-scale delta (0 do at +64 KiB). P2 must
   either pad relocation sites to a fixed width or re-assemble from serialised
   IR2.
8. 2.2 LATR: RVV 1.0 encoding work is complete (0 base-V mnemonics missing),
   next is real SMP. Progress: 66 -> 0 base-V; Zv*/BF16 extensions deferred.

   **SMP (2.2 continued).** design: docs/design/latr-smp-concurrency.md.
   P0 (fence completion: shootdown sequence, per-hart ack at the safe point,
   bounded wait that fails closed) and P1a (park/wake/exit primitives, MSIP write
   notifies the machine, one tested `next_action` decision point) are in.
   P1b's prerequisite is in: baremetal/host/latr_host_smp.c builds the machine
   for the host (build-host/, pthreads) and drives two harts with scripted
   blocks, because generated code is LoongArch. It asserts the IPI wake -> exit
   at a block boundary, WFI clock fast-forward, cross-cache invalidation from
   SFENCE.VMA, and STOP taking one hart offline.
   P1b is in: latr_machine_run_hart() lets the embedder run one hart per thread,
   and the harness now does exactly that (two pthreads, park/wake, join). Two
   concurrency bugs were found by the harness and fixed in the machine: the
   fence wait included the issuer's own stale sequence (self-ack before waiting),
   and a parked hart could never ack because publication did not wake it. The
   host build also needs LATR_ENABLE_TLS: without it tr_data/lsenv are shared
   globals and concurrent translation aborts (~1 in 10 runs).
   P2 is in for the device layer: a bounded spin lock in LatrSystem (with the
   MMIO bus taking it around one routed access, since generated code calls the
   bus directly), a single ticker owner that wakes harts whose timer came due,
   and a two-thread MMIO-vs-DMA hammer in the harness (no failed acquisition,
   no held writer lock, epoch advanced). A trap worth keeping: the ticker must
   wake (not request an exit) because stimecmp resets to 0, so "always due"
   plus exit-request livelocked the cooperative loop; run_machine also returns
   "no progress" after a fully-yielded pass.
   P3 guest-level: `make latr-smp-run` boots a real two-hart machine on the
   LoongArch image and runs a two-hart M-mode guest through generated code -
   AMO counter, LR/SC loop, CLINT IPI waking a WFI hart, and FENCE.I coherence
   across harts (hart 1 must see the rewritten target). Two findings recorded in
   the design: the driver needs a >=1 MiB stack (256 KiB corrupted a callee-saved
   pointer mid-translation and surfaced as a LoongArch address error), and a
   guest spin loop only yields because machine TBs stop at terminators - the ISA
   harness's ALLOW_FLOW mode would deadlock it.
   (vrsub.vx/vi, vmulh/vmulhu/vmulhsu .vv/.vx landed through the arith_extra C
   path, each with hand-computed lane checks in the ISA image), then the widening
   `.wv/.wx` eight (vd and, for `.wv`, vs1 are 2*SEW; `.wx` takes a scalar, so its
   rs1 must not be treated as a vector group), then `vsmul.vv/.vx`, which needed a
   one-line validator fix: PVIR already implemented SAT_MUL with vxrm rounding and
   vxsat but the validator forced ROUND_DEFAULT on it, so the instruction could not
   execute at all.
   FP FMA: the 16 non-widening forms are in (vv/vf, macc/msac/madd/msub with both
   negations), verified with a discriminating single-rounding case. One episode is
   worth recording: they were first reverted as "softfloat is not fusing", which
   was wrong - the test constants were (0x3f7fffff is 1-2^-24, not 1-2^-23).
   Instrumenting parts_muladd showed the widened product and the post-subtract
   value matching the operands, and tests/test_softfloat_fma.c now pins six
   rounding/flag cases plus two fused-cancellation cases so that mistake cannot
   recur. The 8 widening forms (vfw{m,n}macc/msac) also landed: the same helper
   grows a 2*SEW branch where the f32 product is exact in f64 and the single
   rounding happens at the accumulator width. The integer multiply-accumulates
   (vmacc/vnmsac/vmadd/vnmsub, .vv/.vx) also landed: vd is the accumulator for
   one pair and a multiplicand for the other, and the product wraps at SEW
   before the add. Carry/borrow closed too: v0 is a carry-in there, never a
   mask, so the decoder's forced vm=1 on vadc/vsbc means the carry rule and the
   reserved vd == v0 case both need explicit handling. The last six FP encodings
   finished the base set: vfrsub/vfrdiv take the scalar as the first operand,
   vfrec7/vfrsqrt7 are the spec's 7-bit table-driven estimates (implemented from
   the vfrec7.adoc/vfrsqrt7.adoc tables, validated against the spec's numeric
   examples and never replaced by an exact division), and the fp slide1 pair
   reuses the integer permute path with an f-register scalar.
   Next after that: carry/borrow and the MAC family need the multi-destination
   helper, and the six remaining FP ops need helpers plus the 7-bit approximations
   for vfrec7/vfrsqrt7.
9. 2.3 LATM: vmlinux boot, then SMP, then 3A4000 platform alignment.
10. 2.4 LATX: LAPIC/IOAPIC/time, virtio-PCI or AHCI, and the `tb_sys.c` split.
11. 5.2 chaining tiers (only after 3.3 and 5.1 step 3).  P0 is in: the chain
    state machine (six break points + `tb_generation`), `links` pinned at 0, and
    `test-latr-chain` (40 checks). Baseline on the two-hart SMP guest: hart 0
    epoch 12 (11 miss + 1 fence-i), hart 1 epoch 43 (33 wfi + 9 miss + 1
    interrupt). That baseline already shows P1 must not key on "no insert since
    entry" - inserts rival blocks - and that WFI dominates an idle hart.
    P1 is in: a per-hart 4-entry candidate set reuses a looked-up entry when the
    PC matches, the entry is still valid, the whole TB key is equal and the code
    is still inside the arena. On the two-hart guest the idle hart's wfi loop now
    takes 30 of 39 dispatches without hashing. Measured on the host: lookup 2-4
    ns, dispatch 119-138 ns, of which the interrupt refresh (update_hart) is
    ~47% - so Phase A saves ~2% and Phase B (generated-code chaining) is the
    only lever for the rest, with its own risk budget.
    Follow-up done instead of Phase B: that 47% was a PLIC source scan, not an
    unavoidable duty. `loongemu_plic_pending()` now memoises the per-context level
    under a `state_seq` that every level-changing mutator bumps (set_irq, claim,
    complete, enable/threshold/priority writes) - measured: interrupts 52 -> 3-5
    ns, update_hart 54-69 -> 9-10 ns, cached dispatch 116-165 -> 26-36 ns, with
    `test_riscv_plic` pinning that a mutation invalidates the memo. Phase B is not
    justified on these numbers.
12. 5.4 MSI-X/IMSIC and virtio-PCI transport; DMA domains.
13. 4.1 shared modules (arena, recovery, regcache, IRQ sink, DMA domain).
14. 6.1 passthrough manager, only with an IOMMU or an explicit trusted-guest
    restriction.
15. 7 CI matrix and benchmark.
