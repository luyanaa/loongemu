# LATR SMP concurrency model: design

Status: LATR runs every hart on one host thread, round-robin
(`latr_machine.c:884-920`). This design says exactly what may be shared once
harts get their own host threads, what must be fenced, and what will be refused
rather than approximated.

## 1. What exists today (measured)

Read from the tree, not assumed:

| Piece | Where | Concurrency property |
|---|---|---|
| Hart state | `latr-machine.h:71-84` | one `CPUArchState`, one `LatrTbCache`, one arena, one `code_buf`, one translator context **per hart** |
| Translator state | `latr.h:185-222`, `latr.h:304` | per-hart `LatrTranslatorContext`; `tr_data`/`lsenv` are `LATR_TLS` only when `LATR_ENABLE_TLS` is defined |
| TB index | `tb-cache.c:15-27` | per-hart set-associative index with a two-instruction spinlock |
| Shootdown registry | `tb-cache.c:320-380` | lock-free array of `LatrTbCache *`, atomically published/unpublished |
| Arena lifetime | `tb-cache.h:96-120`, `tb-cache.c:136-200` | shootdowns only invalidate; only the owner's safepoint resets the arena |
| Reservation domain | `mmu.c:563-600` | one global word: bit 0 = writer lock, upper bits = write epoch |
| Device DMA | `guest_dma.c:116-190` | DMA lock is a **try**-lock; a second writer fails instead of blocking |
| Device model | `latr-system.h:34-77` | RAM, MMIO bus, CLINT, PLIC, virtio, vclock: no locking at all |
| Virtual clock | `latr_machine.c:896` | one tick per machine step, from the single dispatch loop |
| Address translation | `mmu.c` | no soft TLB; every access resolves through the page tables |

Three consequences follow immediately:

1. **The translation path is already per-hart.** Nothing in the emitter is
   shared except globals reached through `lsenv`, and those are per-thread once
   `LATR_ENABLE_TLS` is on. Threading does not need a translator lock; it needs
   the *build* to define TLS.
2. **There is no per-hart TLB to shoot down.** Only TB caches. That removes the
   hardest class of SMP emulation bug (stale translations after a remote PTE
   change) - at the cost that every guest access re-walks the page table.
3. **The device layer is the single-threaded part.** Anything that is not a
   guest RAM load/store runs through `LatrSystem` and assumes nobody else is
   looking at it.

## 2. Invariants

These are the contract. Each one is testable without a guest (see §9).

- **I1 - code ownership.** A hart executes only code from its own arena, and
  only touches its own `LatrTbCache`. Cross-hart mutation of a cache is limited
  to clearing `valid` under that cache's lock.
- **I2 - no unmapped execution.** Arena memory is recycled only at the owning
  hart's safepoint (`latr_context_safepoint`, `latr_machine.c:782`), which is by
  construction between TBs, so no hart can be inside a block whose storage is
  being reused.
- **I3 - fence completion.** A guest `SFENCE.VMA`/`FENCE.I` does not retire on
  the issuing hart until every other online hart has either acknowledged the
  corresponding shootdown sequence or proved it is outside generated code.
- **I4 - single writer per shared object.** Device model, MMIO bus, CLINT, PLIC,
  virtio and the DMA descriptor walk are mutated under one machine lock. Guest
  RAM is *not* under that lock: it is ordinary shared memory accessed by plain
  host loads/stores and host atomics.
- **I5 - reservation monotonicity.** Every guest-visible write to RAM -
  emitted store, C-level helper store, device DMA - advances the global write
  epoch (`mmu.c:563-600`); an `SC` succeeds only if the epoch it observed at
  `LR` is unchanged and the reservation is still valid.
- **I6 - fail closed.** A bounded spin that expires (shootdown ack, machine
  lock, DMA lock) marks the machine failed and stops it. It never continues with
  a half-applied fence or a dropped store.

## 3. Threading model

- One host thread per online hart. The machine owns no threads: it exposes
  `latr_machine_run_hart(m, hartid, max_tbs)` (one hart, in the calling thread,
  returns 1 = dispatched, 2 = yielded, 3 = STOP honoured and offline, 0 =
  failure) and the embedder runs the service loop: dispatch while RUN, park on
  `latr_hart_park_wait` when idle, break on STOP, join. `latr_machine_run`
  keeps the cooperative round-robin meaning for the freestanding build.
- **Build gate.** Threads require a hosted build with `LATR_ENABLE_TLS` and
  pthreads. The freestanding images (`-ffreestanding`, `__thread` empty,
  `latr/Makefile:20-33`) keep the deterministic round-robin scheduler: CI runs
  the same ISA/sys images in both modes, and the threaded mode is a separate
  target so a CI image that cannot link pthreads never silently changes
  behaviour.
- **Hart lifecycle.** Harts start parked before `boot`, spin on an atomic
  `run_command` word (`PARKED`, `RUN`, `STOP`) with a futex/condvar wait, and
  exit through one join. `online` stays owned by the machine, not by the hart
  thread.
- **WFI.** A guest `WFI` parks the hart on a per-hart wake word set by
  CLINT/IPI delivery or by the ticker. It must release the machine lock before
  parking: a hart that holds the lock while waiting for an interrupt deadlocks
  the machine (this is the same hazard `guest_dma.c:116-124` avoids by failing
  instead of blocking, and it is not acceptable there either - see §6).
- **Determinism.** The threaded scheduler is not deterministic, so every
  invariant is also implemented in the single-threaded build, where the
  interleaving points are explicit: fences, DMA transactions, safepoints. The
  bare-metal harness drives those points in a fixed order (§9.2).

## 4. Translation cache and code lifetime

Rules that follow from I1/I2 and need no new mechanism, only enforcement:

- `latr_tb_cache_lookup`/`insert`/`safepoint` stay owner-only. The cross-core
  registry may call any cache's `shootdown_*`, which takes that cache's lock and
  only clears `valid` (never moves `arena_used`, never frees).
- Publishing new code to the *same* hart is ordered: write words, then
  `latr_icache_sync()` (`latr_machine.c:733-741`), then insert, then execute.
  Other harts never read this arena, so no cross-hart instruction-cache
  protocol is needed beyond I2 - and every hart performs its own
  `dbar/ibar` before executing code it just wrote.
- Eviction never invalidates the *running* block: `lookup` returns an entry
  pointer that stays valid because entries are a fixed array inside the cache
  and reuse only rewrites `valid`/`code` after a shootdown or a safepoint.
  With threads, the entry a hart is about to execute must be **read once into
  locals** (code pointer, size) while holding the cache lock, because another
  hart's shootdown may clear `valid` immediately afterwards. The block still
  runs - that is correct: the shootdown is the guest's promise that any
  translation *started after* the fence sees the new state, not that an
  in-flight block is unwound.
- The arena reset at a safepoint additionally must not race the *shootdown
  issuer*: the issuer may be halfway through walking the registry. Registry
  walk takes each cache's lock, so a reset under the same lock is ordered with
  it. No extra epoch is needed for the arena; the lock is the synchronisation.

## 5. Fence completion (new mechanism)

Today the issuing hart calls `latr_tb_shootdown_asid_all_cores()` /
`latr_tb_fence_i_all_cores()` (`latr_machine.c:771-781`) and retires
immediately. Sequentially that is enough. With threads it is not: the *next*
hart may already be inside a translation that was fetched before the request
landed.

Design:

```
struct LatrShootdownAck {
    _Atomic uint64_t seq;        /* last request this hart has applied */
    _Atomic uint32_t in_code;    /* >0 while executing generated code */
    _Atomic uint32_t wake;       /* set when a request needs this hart to exit */
};
```

- A shootdown becomes a sequenced request: the issuer increments a global
  `shootdown_seq`, publishes `(seq, kind, asid)`, walks the registry applying
  the invalidation, and sets each target's `wake`. **Both halves are load-bearing
  and were found the hard way** (see below): a parked hart never acks unless the
  publication wakes it, and the issuer must ack its own sequence before waiting
  or the wait is guaranteed to time out.
- Each hart's dispatcher already sits between TBs
  (`latr_context_safepoint`, `latr_machine.c:782`). It now also does:
  read `seq`; if behind, apply the pending request locally, release, store its
  `seq`. `in_code` is incremented around `enter_tb` and decremented after, so
  the issuer can distinguish "at a safepoint" from "mid-block".
- Retiring the fence requires `seq` to be acknowledged. A hart that is parked
  (WFI) or at a safepoint acknowledges immediately; a hart mid-block is asked to
  exit by setting a per-hart `tb_exit_request` that the TB epilogue checks (the
  same hook the interpreter-style loop needs for interrupts). If a hart is
  sampled `in_code == 0` but its `seq` is behind, the issuer waits on its
  `wake` word.
- Waiting is bounded by a spin count (`LATR_SMP_ACK_SPINS`, default 1e8), not a
  wall clock: this module is also compiled into the freestanding images, which
  have no clock source, and the bound only has to be long enough for a
  cooperative hart to reach its next safepoint. Expiry fails the machine (I6)
  and records how far behind the worst hart was, rather than silently retiring a
  fence whose effect is not visible - the failure mode this design exists to
  prevent is a guest that "flushes" and then reads stale code.
- `FENCE.I` is machine-wide (all caches, all ASIDs). `SFENCE.VMA` without `rs1`
  is global; with an ASID it is that ASID on every cache; the existing
  `latr_tb_shootdown_asid_all_cores` already implements the second, so the
  sequence number is per (kind, asid) pair - a coefficient of the request, not a
  new registry.

Two measured traps (the threaded harness fails without either fix):

1. **The wait includes the issuer.** `latr_tb_fence_wait` walks every registered
   cache, and the issuer's own `applied_seq` only advances *after* the wait,
   so every fence timed out once and failed the machine. The issuer is inside
   `run_hart` at a block boundary, i.e. provably outside generated code, so it
   acknowledges its own request before waiting.
2. **A parked hart cannot ack.** A hart sitting in WFI will not reach its next
   safe point until something wakes it, so the publication itself must be a wake
   event: `latr_tb_fence_begin` bumps every registered hart's wake word (`struct
   LatrTbCache` carries a `LatrHartSync *park`). Without this, any `SFENCE.VMA`
   issued while another hart is idle burns its bound and fails the machine.

The TB epilogue hook is one word per hart: the translator already ends a block
at a controlled point, so the exit check is an `env` field test plus a branch
to the exit stub. It must be cheap when unset (load + branch), because it runs
once per TB.

## 6. Memory ordering

Guest RVWMO is not host ordering on any of the three hosts, so each construct
is mapped explicitly:

| Guest construct | Host mechanism | Note |
|---|---|---|
| plain aligned load/store | plain host load/store | atomicity up to 8 bytes on every supported host; misaligned or page-crossing accesses already go through the C helpers (`latr_cross_load/store`) |
| `AMO*` | host AMO (`la_am*` on LoongArch, `lwarx/stwcx` loop only for SC) | already emitted this way (`translate-riscv.c:453-500`); the host atomic gives atomicity, not ordering, so `aq/rl` still needs a fence |
| `LR`/`SC` | global epoch word (`mmu.c:563-600`) | `SC` re-checks epoch + reservation; a device DMA advances the epoch through `latr_reservation_notify_write` |
| `FENCE` (guest) | `dbar 0` on LoongArch, `__atomic_thread_fence(SEQ_CST)` elsewhere | emitted for every `FENCE` predicate combination; `fence.tso` maps to the same |
| `SFENCE.VMA` / `FENCE.I` | §5 shootdown + ack | ordering is "before" on the issuing hart, "visible" on the others |
| device MMIO read/write | machine lock + device-side ordering | MMIO is not RAM: every access takes the lock, so no separate fence is needed for MMIO-vs-MMIO |
| DMA store | DMA lock + `latr_reservation_notify_write` under it | must stay under the lock: a store, then a DMA, then an `LR` must not observe the pre-DMA value while the epoch is unchanged |

Two rules that are easy to get wrong and are therefore stated as requirements:

- **`aq`/`rl` bits on AMOs.** Host `la_amadd_d` has relaxed ordering. An `aq` or
  `rl` AMO must emit the host fence on the corresponding side; a plain `AMO` must
  not (guest RVWMO does not require it, and adding it costs a pipeline flush on
  every counter increment).
- **Store-then-DMA deadlock.** A guest store that triggers a DMA inside the
  device model must not hold the DMA lock while calling
  `latr_reservation_notify_write`, and must not hold the reservation epoch
  transaction while taking the DMA lock. Lock order is fixed:
  **machine lock → DMA lock → reservation epoch**. The reservation epoch is the
  innermost and is never held across a device call. (`guest_dma.c:126-190`
  already notifies while holding the DMA lock; that is fine because the epoch is
  a compare-exchange, not a blocking lock.)

## 7. Device model and clock

- **Implemented (P2):** one bounded spin lock in `LatrSystem` guards its
  mutable state and every device window. Guest MMIO reaches the bus directly
  from translated code, so the bus itself carries the lock hook
  (`loongemu_mmio_bus_set_lock`) and takes it around exactly one routed access;
  `latr_system_tick` and `latr_system_update_hart` take the same lock directly.
  Acquisition is bounded (`LATR_SYSTEM_LOCK_SPINS`, 1e24 default) and failures
  are counted in `device_lock_failures` rather than ignored.
- **Never translate or execute guest code while holding it.** A hart takes the
  lock for one MMIO access or one device step and releases it.
- CLINT `MSIP` writes from another hart are atomic stores into the target's
  `mip`; the target observes them at `latr_system_update_hart`
  (`latr_system.h:77`) or at its next safepoint. An IPI does not need to
  interrupt a running block *immediately* - RVWMO allows the delay - but it does
  need to be visible at the next safepoint, which is what the `wake` word is for.
- The virtual clock has one writer: `latr_machine_tick()` (called by the
  embedder's ticker thread, and by the cooperative loop). It advances the clock
  under the device lock and then wakes every online hart whose MTIP or STIP
  comparator has become due (`latr_system_timer_due`): a hart parked in WFI
  cannot sample its own comparator, so without that wake a timer never fires.
- **Measured trap:** the wake must be `latr_hart_wake`, not
  `latr_machine_wake_hart`. The latter also requests a dispatch exit, which is
  right for an IPI but catastrophic for a timer, because `stimecmp` resets to 0
  (always "due"): every cooperative step then became a yield, `run_hart` never
  dispatched, and `latr_machine_run` spun forever. Timers only wake; IPIs and
  fences ask for an exit.
- Belt and braces: `latr_machine_run` now returns "no progress" (2) when a
  whole pass of online harts yielded without dispatching, so a pent-up exit
  request can never livelock a caller.
- Stopping: the ticker stops first, then harts are asked to exit at their next
  safepoint, then joined. Nothing frees `LatrSystem` until every hart has
  acknowledged.

## 8. Build and configuration

- `-DLATR_SMP_THREADS` enables the fence-ack wait (without it the machine retires
  a fence immediately, which is correct only when nobody else can run). With
  threads it requires `LATR_ENABLE_TLS` + pthreads.
  **Measured:** without `LATR_ENABLE_TLS` the host build defines `__thread` away,
  `tr_data`/`lsenv` become plain globals, and two harts translating
  concurrently abort the process (about 1 run in 10 in the harness). With TLS
  on, 200 consecutive runs pass.
- Default stays single-threaded. `make test` runs the host protocol tests
  (pthread) unconditionally; the bare-metal images exercise the deterministic
  interleaving.
- `LATR_SMP_ACK_SPINS` and `LATR_MACHINE_MAX_HARTS` are compile-time knobs with
  defaults (1e8 spins, 4 harts).
- The hosted build (`build-host/`) deliberately does not gate on warnings: it
  compiles the translator with a host compiler (gcc on the CI runner) whose
  diagnostics have never been validated with `-Werror`, and the freestanding
  build remains the warning gate. The harness file itself is compiled with
  `-Werror`.

## 9. Test plan

### 9.1 Host tests (`make test`, pthread, no guest)

- **Hosted dispatcher harness** (`baremetal/host/latr_host_smp.c`,
  `make -C latr test-host-machine`, in `latr check`). The machine is compiled
  for the host (`build-host/`, a separate directory so objects built with
  different flags never mix) and driven with two harts. Generated code is
  LoongArch, so the harness replaces `enter_tb` with a scripted block model and
  drives the same entry points generated code would use: a CLINT MSIP write for
  the IPI, `env->wfi_pending` for WFI, `env->sfence_pending` for SFENCE.VMA,
  and the SiFive test device for exit. It asserts: both harts dispatch and
  produce host code, exactly one MSIP reaches the machine, the woken hart
  honours its exit request at a block boundary, WFI fast-forwards the clock,
  SFENCE.VMA advances the sequence and invalidates **both** caches (generation
  bump on each, not just the issuer's), and STOP takes one hart offline while
  the other keeps running. Guest semantics stay in the bare-metal images; this
  harness covers the C layer around the generated code.

- **Fence ack protocol**: N threads, each at a safepoint loop; issuer requests a
  shootdown; every thread must apply it before the issuer returns. A thread
  parked mid-"block" must be asked to exit and must do so within the bound.
- **Ack timeout is fatal**: a thread that never acknowledges fails the machine
  with its id, and no other thread is left spinning.
- **Epoch monotonicity**: writers (emitted-store simulation, helper store, DMA
  transaction) advance the epoch exactly once each; a reader that samples the
  epoch then writes cannot lose an update.
- **Lock order**: a device step that calls the DMA path cannot deadlock; the
  test runs the machine-lock → DMA-lock → epoch sequence from two threads.
- **Cache safety**: shootdown while another thread is "executing" the entry
  leaves the code bytes intact and only clears `valid`.

### 9.2 Deterministic interleaving (bare-metal image, no threads)

A scheduler that runs the harts' safepoints in a scripted order: A applies a
remote shootdown request while B is between TBs; B is mid-block and must take
the exit; B is parked in WFI and must be woken. Each scenario asserts the same
invariants the host test asserts, so the CI image covers the protocol even where
pthreads do not exist.

**Implemented** as `scripted_scenarios()` in `baremetal/bare/latr_smp_test.c`,
against a second machine booted with `latr_smp_idle.S` (hart 1 starts at `wfi`,
so the driver decides every step; hart 0 must never run, which is itself a
check). Twelve checks, all in `make latr-smp-run` (33 checks total):

- a shootdown applied while hart 1 is between TBs bumps its cache generation and
  leaves no valid entry, and the hart gets back to dispatching with a
  re-translated block;
- a wake is counted, which is what an embedder parked in WFI relies on;
- an exit request raised between TBs runs at most one more block, clears itself,
  and does not wedge the hart.

Two things the image cannot assert, and the host harness owns instead: a hart
**parked** in WFI (the machine fast-forwards the idle advance rather than
parking; parking is the embedder's `latr_hart_park_wait` loop), and the exact
call index that consumes a one-shot yield - the shootdown raises one, and pinning
it to a call made the scenario depend on how many dispatches had happened
before, so the assertion is progress plus re-translation instead.

### 9.3 Guest-level (bare-metal image, `make latr-smp-run`)

Implemented as `baremetal/bare/latr_smp_test.c` + `latr_smp_guest.S`: a real
`LatrMachine` with two harts loads a two-hart M-mode RISC-V program as firmware
and runs it through generated LoongArch code. Checks (9): the guest powers off
through the exit device, both harts dispatch, the AMO counter reaches 32, the
LR/SC loop completes 16 increments with no lost update, hart 1 called the *old*
target before the rewrite (result 1), one software interrupt was delivered, and
both harts see the *new* target (result 2) after hart 0 rewrote it and executed
FENCE.I. The FENCE.I check is the one that fails if the cross-core shootdown
does not actually invalidate hart 1's cached block.

Two traps found while bringing it up, both worth keeping:

- **Stack size is part of the contract.** With a 256 KiB stack the driver died
  inside `run_hart` with a LoongArch *address error* (`ld.d` from a callee-saved
  pointer that had become 0) while translating a block; nothing in the output
  said "stack". 1 MiB fixes it. The translator + machine frames are deeper than
  they look, and the failure mode is register corruption, not a guard page.
- **A guest spin loop inside one TB never yields.** `target_disasm` only stops
  at terminators when `cflags` has no `ALLOW_FLOW` bit (the machine sets 0, the
  unit harness sets the bit), so the machine's blocks do end at branches. That
  is why the guest can spin on a flag written by the other hart at all; the ISA
  harness's multi-flow mode would deadlock the same program.

The threaded half of 9.3 (two harts *concurrently* executing) is what the host
harness covers; a bare-metal image has no pthreads, so the two are complementary
rather than redundant.

### 9.3b Guest-level concurrency (threaded build, still blocked)

- AMO counter: 2 harts, N increments each, exact N*2 at the end.
- LR/SC ping-pong: both harts retry until success; no lost update, no livelock
  past the retry budget.
- IPI latency: hart 0 sets `MSIP`, hart 1's handler increments a count; every IPI
  must be observed (no coalescing that loses the count).
- `FENCE.I` coherence: hart 0 rewrites a block, fences, hart 1 must execute the
  new bytes; with the fence ack missing this test is the one that fails.
- SMC + shootdown stress: rewrite, fence, execute, repeat, with a hart polling.

## 10. Phases

| Phase | Scope | Acceptance |
|---|---|---|
| **P0** | `LatrShootdownAck` sequence + safepoint ack + bounded wait, single-threaded build drives it through the deterministic harness | host test 9.1 (first two items), existing ISA/sys images unchanged |
| **P1a** | exit request at the safe point; park/wake/command primitives; MSIP write notifies the machine; `next_action` decision point | `test-latr-hart-sync` (pthread) + CLINT notify test; ISA/sys images unchanged |
| **P1b** | hart threads, join, TLS-gated build, on the hosted harness | done: `latr_machine_run_hart` + one pthread per hart, 21 checks including the threaded fence completing with no ack timeout |
| **P2** | machine lock around `LatrSystem`; ticker ownership; DMA/store lock order | done for the device layer: bus lock hook, bounded system lock with counted failures, ticker thread and timer wake, two-thread MMIO-vs-DMA hammer asserting no failed acquisition, no held writer lock, and an advanced reservation epoch. Guest-level 9.3 stays blocked on LoongArch execution. |
| **P3** | guest-level 9.3 tests, AES/fence stress, `make latr-sys-smp-run` in CI | all 9.3 green, threaded run reproducible over 100 iterations - **reproducibility done**: `make -C latr test-host-machine-stress HOST_SMP_STRESS=100` runs the threaded harness 100 times and requires a zero exit and an identical summary line each time (39 checks, measured 2.1 s); CI runs it in `host-tests`. Guest-level 9.3 and the sys-mode SMP image stay open |

Each phase lands with its tests and without changing the single-threaded
behaviour; a phase that cannot show its acceptance test does not land.

**Why P1 is split.** The hart-thread half needs a machine that can be linked
against pthreads and can be driven by a test. The hosted harness above now
supplies both (it compiles the machine for the host and links pthreads), so P1b
is unblocked; the remaining work is the scheduler itself and its join. The
earlier blocker, kept here because it explains why this was not done sooner:
today's machine only exists inside freestanding images
(`latr/Makefile` builds `liblatr_core.a` with `-ffreestanding --target=loongarch64`).
The two-hart phase in `baremetal/bare/latr_boot_smoke.c` is the only in-image
user of `latr_machine_run`, and it already fails at HEAD before reaching the
dispatch assertions (host fault at `0x80200018`), so it cannot be the
acceptance test for wiring either. P1b therefore starts by building a hosted
machine harness (compile `latr_machine.c` + system + translator for the host,
boot a tiny guest, one thread per hart); until that exists the scheduler would
be code no test can run, which is exactly what this design refuses to ship.

**P1b prerequisite, done.** The hosted machine harness exists and passes on
`make -C latr test-host-machine`; it is the acceptance floor the thread
scheduler will be added against.

**What P1a does verify.** The decision that makes a hart leave a block lives in
one function (`latr_hart_next_action`), so its policy - stop beats yield, a
yield is one-shot, parked harts do not dispatch - is unit-tested; the wake
counter is tested for loss-free delivery under real threads; and the CLINT
notify hook is tested from both the direct and MMIO paths, including that a
clear does not wake anyone.

## 11. Refusals

- **No shared TB cache between harts.** Two harts with the same ASID could
  share translations, but code ownership (I1) and arena lifetime (I2) would then
  need reference counting on every entry, and the win is bounded by the key
  mismatch rate. Per-hart caches plus a shootdown registry is the boring correct
  design; sharing is a later optimisation with its own design.
- **No per-page locks for the device model.** Start with one machine lock; the
  access rate is dominated by guest RAM, which is not under it.
- **No unbounded spins** anywhere (I6). Every wait in the machine has a
  deadline and a failure that names the hart and the request.
- **No reliance on host TSO.** x86 gives more ordering than the guest needs, but
  LoongArch and AArch64 do not; every guest fence is emitted explicitly, and a
  test that only passes because of x86 ordering is a bug.
- **No mid-block unwinding.** A shootdown that lands while a hart is in a block
  never redirects it; the hart finishes the block and applies the request at the
  next safepoint. That is the only model under which I2 holds without
  publishing recovery spans for arbitrary host PCs.
