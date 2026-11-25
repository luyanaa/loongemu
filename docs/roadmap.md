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

3. CPU 0 writes IOCSR 0x1040 targeting CPU 1; CPU 1's `cpu_ip` gains the vector,
   and CPU 1's handler runs and clears it - the register half is already covered
   by `latm-uncore`, so the new assertion is that *execution* crosses (needs CP0
   Status.IE / Cause.IP setup in the guest and a handler at the exception base);
4. an LL/SC pair on CPU 1 is invalidated by a plain store from CPU 0 (reservation
   coherence), which needs a shared reservation table the uncore does not have
   yet; this changes the memory path, so it is its own step.

| module | remaining work | priority |
|---|---|---|
| MIPS64 Linux | real vmlinux/initrd boot into `start_kernel()` | P0 |
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
