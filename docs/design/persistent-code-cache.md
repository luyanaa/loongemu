# Persistent code cache: design

Status: nothing is serialised today. The design's job is to state precisely what
makes a generated block reusable, and to refuse the rest.

## 1. What a translated block contains today

A TB is an arena-resident sequence of LoongArch words produced by
`tr_ir2_assemble` (`target/riscv/latr/latr.c:1127`):

* **intra-TB branches** are resolved *before encoding* into instruction-index
  differences (`label_dispose`, `latr.c:1075-1125`, `ir2_opnd_convert_label_to_imm`
  with `positions[id] - ir2_index`), which the LoongArch encoder turns into
  PC-relative `offs16/offs26`. They therefore survive being moved to another
  address - with the caveat that the port never expands a far branch, so this is
  an encoding property, not a relocation mechanism.
* **exits** are `la_jirl($r0, $ra, 0)` (`latr.c:923`, per-exit labels at 928-932):
  a return through `$ra`, i.e. position-independent.
* **helper calls** embed an absolute host address:
  `li_d(target, (intptr_t)&latr_mmio_load); la_jirl($ra, target, 0)`
  (`translate-riscv.c:165-167`, same shape for `latr_cross_load` at 217-219 and
  the fault/mem paths at 293/322). The return address is parked in
  `env->host_return_address` first, so the convention is already explicit.
* **the recovery map**: `latr_tb_insn_map_*` rows keyed by guest PC with
  `native_start/native_end` offsets, rebased by
  `latr_tb_insn_map_rebase(code_buf, entry->code)` when the block lands at its
  final address.
* **the key**: `LatrTbKey { satp_mode, privilege, asid, vspec, root_ppn }`
  plus `guest_pc` (`latr-tb-cache.h:19`).

So a block is *almost* relocatable: relative branches work, exits work, and the
only absolute addresses in emitted code are the helper entry points.

**Measured (P0, done), not assumed:** the LATR ISA image executes 286 blocks; 171
of them embed at least one host address, 485 sites in total, at most 20 in one
block. The first implementation of the registry used 24 slots per block - one
block away from truncating a relocation list - so it is 64 now and overflow is a
hard failure in the harness. The sites are no longer scattered `li_d(target,
&fn)` calls: `latr_emit_code_symbol()` records `(ir2 index, symbol)` and emits the
load, which turns "which words need relocation" into an enumerable list.
Symbols plus the recovery rows are the whole relocation surface.
That is the whole design surface.

## 2. Design

### 2.1 Format

```
header {
  magic "LTBC", u32 format_version,
  u32 backend_kind,            /* LATR / LATM / LATA */
  u64 backend_fingerprint,     /* see 2.4 */
  u64 feature_bits,            /* LBT, LSX, LASX, host page size log2 */
  u32 record_count, u32 record_bytes,
  u8  checksum[32]             /* SHA-256 over header-without-checksum + records */
}
record {
  u32 guest_pc_lo, guest_pc_hi,
  LatrTbKey key,               /* 24 bytes, opaque to the loader */
  u32 icount, u32 code_bytes,
  u32 page_count, sha256_t page_hash[page_count],   /* guest identity */
  u32 reloc_count, reloc[reloc_count]  { u16 offset, u8 kind, u8 sym },
  u32 symbol_count, symbol[symbol_count] { u64 name_hash },  /* resolved at load */
  u32 span_count, LeRecoverySpan span[span_count],
  u8  code[code_bytes],
  u8  record_sha[32]
}
```

`kind` covers what §1 says can move:

| kind | meaning |
|---|---|
| `SYM_ABS64` | a 64-bit word holds an absolute address of symbol `sym`; the loader writes the resolved address |
| `CODE_PCREL` | reserved; upstream already emits offsets, so it should stay unused |
| `SPAN_OFFSET` | a recovery span's `native_start/native_end`, already relative - present only as a self-check |

Symbols are stored by *hash of a name* (`"latr_mmio_load"` etc.), and the loader
resolves them from a table the port publishes at startup. This is the only
correct way to survive PIE/ASLR: an absolute address in a cache file is a
promise about a build that will not be true next boot.

### 2.2 Two conditions to reuse a block, both cheap

1. **Identity**: every guest page the TB covers hashes to the stored
   `page_hash[]`. Pages are hashed from RAM at load time, so self-modifying code
   between sessions cannot resurrect a stale block; within a session the
   existing `fence.i` / shootdown path stays authoritative.
2. **Codegen**: `backend_fingerprint` matches (§2.4).

A record failing either is dropped; the dispatcher translates as it does today.
No partial trust, no "probably fine".

### 2.3 Load path

`mmap` read-only, validate the header checksum, then for each record: verify
`record_sha`, verify identity, resolve symbols, copy `code` into the arena,
apply relocations, `latr_tb_insn_map_rebase`, insert with the stored key.
The arena semantics do not change: the loader consumes arena space exactly as a
translation would, so `reclaim` and the shootdown paths keep working unmodified.

Lazy option (P3): map instead of copy and insert on first hit. Rejected for the
first version because the arena remains the single owner of executable bytes,
which is what every invalidation path assumes.

### 2.4 Backend fingerprint

A git hash is too coarse (a comment change invalidates) and too weak (a local
uncommitted edit does not). Use a **codegen fingerprint**: at startup, translate
a fixed 8-instruction micro-program per port (one `add`, one branch, one load,
one exit) and hash the emitted words. Store that value in the header. Cost: one
tiny translation; benefit: exactly the property the cache depends on - the
emitted code for a given input.

The fingerprint must be combined with `format_version` and `feature_bits`,
because the same codegen on a host without LASX produces different code for
vector blocks (and the `feature_bits` mismatch drops the cache wholesale).

### 2.5 Trust boundary

Cached code is executable content read from a file. The design treats the cache
directory as **untrusted input**:

* header and per-record SHA-256 must match, or the file is ignored entirely;
* a cache path that is world-writable is refused, with the reason in the log;
* `mmap` is `PROT_READ` only, and code is copied into the arena, never executed
  from the mapping;
* the file records `backend_kind`, so one port cannot load another's records.

Without this the cache is a straightforward code-injection vector for anything
that can write the file, and "it's only a cache" is not a defence - the cache
*is* the code.

**Dependency note:** the repository has no SHA-256 implementation today (`grep`
for `sha256`/`crc32` across `src/` and `include/` returns nothing), so P2 needs
either a small self-contained SHA-256 (~120 lines, testable against FIPS-180
vectors, also reusable for the page hashes) or a cheaper non-cryptographic hash
with the trust boundary moved to file permissions. The design assumes the former,
because a checksum whose collision an attacker can compute is not a checksum.

## 3. Phases

| phase | change | gate |
|---|---|---|
| P0 | done: `latr_emit_code_symbol()` + per-TB symbol table (485 sites measured, cap 64, overflow is a harness failure); every absolute address in the LATR translator goes through it | `latr-isa 1607` with the symbol census; sites rewired: 6 in `translate-riscv.c`, 1 each in the CSR/FP/RVV/PVIR files |
| P1 | in-process serialise/deserialise round trip of one TB (memory only) | same test + `latr-isa` with round trip forced on |
| P2 | on-disk format, fingerprint, validation, refusal paths | boot smoke; corrupted-file/negative tests |
| P3 | lazy load, hit/miss counters, size budget/eviction | PERF counters: load time vs translate time |

P0 is the decisive step: if the relocation test cannot be made to pass, nothing
after it can be correct, and the test is cheap.

## 3a. Round-trip result and the one blocker it found

The P1 test (`test_reloc_round_trip` in the LATR ISA image) does exactly the
sequence §4 describes, in one process:

1. translate `sd x9, 8(x8)` into the arena: 245 words, **4 symbol spans**;
2. copy the words to a second, differently aligned buffer;
3. re-emit each span with `li_d(reg, symbol)` for the *same* symbol and require
   the word count to match (it does, so the recorded spans are exact);
4. poison the original arena with zeros, so any unrecorded self-address would
   fault instead of silently working;
5. execute the copy: the guest store lands and the guest state matches the
   original run.

So the records are complete enough to rebuild a block byte-for-byte at a new
address, and the block itself is position-independent apart from the recorded
spans.

**The blocker that decides P2's shape:** `li_d` is a variable-length expansion
(`imm_helper` chooses among `lu12i/ori/lu32i/lu52i`). Measured on the same four
spans:

| relocation delta | spans whose word count changes |
|---|---|
| +64 KiB | 0 of 4 |
| +4 GiB (ASLR scale) | **4 of 4** |

In-place patching therefore only works for small displacements. A cache loaded
into a fresh process faces an ASLR-scale delta, so P2 must either emit relocation
sites in a fixed-width form (for example always four words, padding with `nop`)
or re-assemble the block from serialised IR2. The measurement is in the ISA log
line `relocation spans=4 words=245 resized_64k=0 resized_4g=4`, so the next
attempt starts from data rather than from this paragraph.

## 4. The relocation test that decides everything

1. Translate a fixture block into arena A; record results (guest registers,
   memory, PC) by executing it.
2. Serialise the block with its relocation records; deserialise into arena B,
   a *different* address, applying symbols from the live process.
3. Execute from B, compare every observed value with step 1.
4. Repeat with a second arena base to catch a relocation bug that happens to
   work at one offset.

The test is host-side, needs no QEMU, and can run over the whole `latr-isa`
fixture set later. It also catches the classic failure of this feature: a
backend that starts emitting an absolute address without a reloc record, which
step 3 surfaces immediately as a jump into freed memory.

## 5. Invariants

1. Code is never executed from a file mapping; it is copied into the arena that
   the invalidation paths know about.
2. A record is loaded only if its guest pages hash to the stored values *at load
   time*.
3. Recovery rows are part of the record; a block whose rows fail validation is
   not loaded at all, because running it without rows turns a precise fault into
   an unrecoverable one (the `le-recovery.h` contract).
4. Every absolute address in emitted code has a relocation record. Enforced by
   the relocation test, not by review.

## 6. Self-review

**Objection 1: helper calls are only one of several absolute addresses; what
about `store_pc_constant`, `latr_env_*` offsets, imm64 constants that happen to
look like addresses?**
`store_pc_constant` stores a guest PC (a value, not an address) and
`latr_env_*` offsets are structure offsets, both stable across a restart - they
need no relocation. The risk is *missing* an address, which is why invariant 5.4
makes the test authoritative: translate, relocate, execute, compare. A missed
reloc shows up as a wrong jump, not as a subtle wrong value.

**Objection 2: persisting recovery rows may be worthless if they depend on
register allocation that changes between builds.**
They do depend on it - `bind_first`/`bind_cnt` describe host temporaries. That is
exactly why the fingerprint gates the cache: rows are only reused with the same
codegen. If the row format changes, `format_version` bumps and every old record
is dropped. There is no attempt to migrate rows across builds; that would need a
symbolic re-encoding of bindings, which is the next design, not this one.

**Objection 3: hashing every guest page on every load is expensive for a large
cache.**
Cost is bounded by cache size, and the hash is over 4 KiB pages with a simple
SHA-256 - tens of microseconds per page. Loading is once per boot and can be
made selective (only records whose `guest_pc` falls in the boot path). The
alternative - trusting the page contents because "the file was written by us" -
reintroduces the SMC hazard the hash exists to remove.

**Objection 4: does this duplicate LATX's `ir2-relocate.h` machinery?**
Partly. LATX already has `la_jump_epilogue` (pc-relative stub), `la_data_li`
and `ir2_relocate` for its own relocation scheme, and the design should reuse
the *idea* (a fixup list applied after placement) rather than the code, which is
x86/LATX-specific and pulls in QEMU headers. `ir2-relocate.h`'s comment -
"If src1/2 is LABEL, it will use the offset (from tc.ptr)" - is the same model as
§2.1's `SYM_ABS64`, one level up.

**Objection 5: is a persistent cache worth it for a playground VMM whose boot is
the workload?**
Not obviously, and that is the honest risk. The value is in cold-start latency
for repeated boots and in not re-translating firmware. P3 should measure
"translate time as a fraction of boot" first; if it is small, the feature is
infrastructure looking for a use case, and the right outcome is to keep P0 (the
relocation test, which also hardens the backend) and stop.

**Open question:** whether the cache should be per-hart or shared. The `LatrTbKey`
is per-hart (ASID/privilege), but identical harts produce identical blocks;
sharing would need the key to become a set. Left open deliberately - a shared
cache multiplies the invalidation surface, and the first version should be
per-hart, private directory, no sharing.
