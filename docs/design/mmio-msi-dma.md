# MMIO fast path, MSI/MSI-X and DMA isolation: design

Status: two of the three pieces exist and only need extension; MSI/MSI-X and the
isolation policy are new.

## 1. Current state

| piece | where | what it does |
|---|---|---|
| guest-physical MMIO bus | `include/loongemu/mmio_bus.h`, `src/mmio_bus.c` | 16 device windows, width-checked dispatch, **8-slot route cache keyed by (pa, size) with a `generation` bumped on add/remove** |
| route cache tests | `tests/test_riscv_uart.c:123-137` | repeat hit, width in the key, removal invalidates, re-attach works |
| devices | `src/riscv_clint.c`, `src/riscv_plic.c`, `src/riscv_virtio_blk.c`, `src/uart16550.c` | all MMIO, all register on the bus |
| x86 PCI host | `include/loongemu/pci_host.h` | i440FX + PIIX + Bochs VBE, `0xCF8/0xCFC`, 16 MiB ECAM, LS7A physical config passthrough (read-only unless a write callback is installed) |
| device DMA | `include/loongemu/guest_dma.h` | GPA->HPA with 4-slot generation-checked cache, RAM-only, every write notifies the LR/SC reservation domain |
| interrupt description | `include/loongemu/riscv_acpi.h` | APLIC/IMSIC addresses and geometry, described but not implemented |

So: `DeviceFastPath` for register-level MMIO exists (the bus route cache), and
DMA has a mechanism but no policy. MSI/MSI-X does not exist at all.

## 2. What is actually missing

1. **Vector delivery.** No device can raise an interrupt without pinning a
   PLIC/CLINT line; there is no MSI capability, no message decode, no
   per-vector masking.
2. **Policy for DMA.** `guest_dma` answers "is this GPA RAM?" but not "is this
   GPA allowed for *this* device?" A virtio descriptor can therefore target any
   RAM page, including another device's ring.
3. **Register-level fast path beyond the bus.** The bus cache remembers the last
   (pa, size) -> device; a hot doorbell still goes through
   `loongemu_mmio_bus_write` -> function pointer -> device. That is acceptable;
   what is missing is *not reading the device state twice* for the same poll
   (e.g. LSR), which is a device-level memo, and MSI-X suppression so a masked
   vector costs nothing.

## 3. Design

### 3.1 MSI-X in the shared PCI core

Add to `pci_host` (x86 side) and to a small shared core so the RISC-V PCIe
root complex can reuse it:

```c
#define LOONGEMU_MSIX_VECTORS 64
typedef struct LoongemuMsixVector {
    uint64_t msg_addr;
    uint32_t msg_data;
    uint32_t ctrl;      /* bit0 masked; bit1..2 reserved per spec */
} LoongemuMsixVector;

typedef struct LoongemuMsix {
    LoongemuMsixVector table[LOONGEMU_MSIX_VECTORS]; /* host-backed copy */
    uint32_t pba[2];                                  /* pending bits */
    uint8_t  bir;              /* BAR indicator from the capability */
    uint32_t table_offset;
    uint32_t pba_offset;
    uint16_t nvec;             /* N-1 as encoded */
    uint8_t  enabled;
    uint8_t  func_mask;
} LoongemuMsix;

int  loongemu_msix_cap_read(const LoongemuMsix *, uint16_t reg, uint32_t *out);
int  loongemu_msix_cap_write(LoongemuMsix *, uint16_t reg, uint32_t val, unsigned size);
void loongemu_msix_trigger(LoongemuMsix *, unsigned vec, LoongemuMsiSink *sink);
```

Decisions that matter:

* **The table is host-backed**, not a window into guest memory (QEMU does the
  same). Reason: the table lives in a BAR, and a device that DMA-writes its own
  BAR would otherwise rewrite its own interrupt target mid-delivery. Guest
  accesses to the table/PBA range are served from this copy by the device's BAR
  MMIO callback - the same shape as the existing
  `loongemu_bochs_mmio_read/write` pair. Note `LoongemuPciBarMapFn` is *not*
  that path: it only tells the host that the guest relocated a BAR.
* **Delivery is an interface, not a target**: `LoongemuMsiSink` has
  `inject(uint64_t addr, uint32_t data)`. Implementations:
  - RISC-V, first and real: PLIC
    (`src/riscv_plic.c` already owns the claim/priority registers) - an MSI
    becomes "raise the source the PLIC already knows";
  - RISC-V, later: APLIC/IMSIC per `riscv_acpi.h` (`imsic_addr`,
    `imsic_num_ids`), only when an AIA machine exists;
  - x86: a *decoder*, not a device. `LOONGEMU_LAPIC_GPA` (0xFEE0_0000) exists
    today only as an ACPI MADT field (`include/loongemu/acpi_tables.h:16`,
    `src/acpi_tables.c:133`); there is no LAPIC model. So the x86 sink decodes
    address -> APIC id and data[7:0] -> vector into a pending-vector word owned
    by the machine, which is enough to test the PCI side and is explicitly not a
    full LAPIC (no IRR/ISR/EOI/timer).
  This keeps the PCI code free of machine specifics and makes every decode path
  unit-testable on the host.
* **Masking order** is `func_mask || vector.ctrl & 1`; a masked trigger sets the
  PBA bit and returns. Reading the PBA is a side-effect-free copy (the spec's
  "read clears" behaviour does not exist for PBA; it exists for the EOI).

### 3.2 DMA isolation policy

Extend `guest_dma` with a per-device window set, checked before the RAM check:

```c
#define LOONGEMU_DMA_WINDOWS 8
typedef struct LoongemuDmaWindow { uint64_t gpa, len; uint8_t valid; } LoongemuDmaWindow;
typedef struct LoongemuDmaDomain { LoongemuDmaWindow win[LOONGEMU_DMA_WINDOWS]; } LoongemuDmaDomain;

int loongemu_guest_dma_bind(LoongemuGuestDma *dma, LoongemuDmaDomain *dom);
bool loongemu_dma_domain_allows(const LoongemuDmaDomain *, uint64_t gpa, uint64_t len);
```

Rule: a device with a bound domain may only touch its windows; a device with no
domain (legacy) keeps today's behaviour (RAM-only) so existing tests and the
virtio-blk path do not change semantics by accident. The virtio-block device gets
a domain of its own ring + descriptor buffers; that is the first real user and
the reason to add it.

**No passthrough without an IOMMU.** The LS7A backend stays config-space
read-only unless a write callback is installed, and the design states the
requirement explicitly: a device bus-mastering into guest RAM with a
guest-controlled GPA and no IOMMU is an escape from the guest's own isolation
model. MSI-X does not change that; it narrows the interrupt surface only.

### 3.3 Doorbell fast path

For virtio MMIO, the queue-notify register is the hottest MMIO write. Two
additions, both inside the device (not the bus):

1. **Memoised read-only state**: reading an unchanged status register twice in a
   row returns the cached value (invalidated by any write to the device). This
   is the same trick the bus route cache uses, one level down, and it is only
   legal for registers with no read side effects - hence a per-register table,
   not a blanket policy.
2. **MSI-X suppression**: with MSI-X enabled and a vector masked, the doorbell
   records the PBA bit and skips the sink call entirely.

## 4. Phases

| phase | deliverable | gate |
|---|---|---|
| P0 | `msix` core + x86 pending-vector decode (not a LAPIC); unit tests | `make test` (new host tests) |
| P1 | MSI-X capability exposure in `pci_host` config space, host-backed table, PBA | host tests: cap read/write, mask/enable, pending |
| P2 | RISC-V PLIC sink + virtio-blk MSI-X wiring | `test_riscv_virtio_blk` with vector delivery; boot smoke |
| P3 | DMA domains + virtio-blk domain; rejection tests | host tests: MMIO-target and out-of-window descriptors rejected |
| P4 | doorbell memo + suppression; PERF counters (MMIO latency, interrupts) | PERF comparison |

## 5. Invariants

1. An MMIO access is never served from a device cache after a write to that
   device; the memo is invalidated by any write, and the route cache by any
   registry change (`generation`).
2. An MSI-X vector write may not be executed from guest memory; the host copy is
   authoritative and guest-visible only through the BAR window.
3. A DMA store notifies the reservation domain exactly once, as today
   (`guest_dma.h` guarantees it structurally); domains only *reject*, never
   bypass.
4. No lock may be held across `sink->inject()`; a sink may re-enter the bus
   (Level interrupts terminating in a device write are a real pattern).

## 6. Verification

| layer | check |
|---|---|
| decode | `test_pci_msix.c`: capability layout, table/PBA offsets, mask, enable, pending |
| delivery | `test_msi_decode.c`: RISC-V IMSIC address/data -> EIID; x86 LAPIC -> vector |
| DMA | `test_guest_dma_domain.c`: allowed window, straddling window, MMIO target, no-domain legacy |
| integration | `test_riscv_virtio_blk` with MSI-X; boot smoke; `latr-isa` |

## 7. Self-review

**Objection 1: the host-backed MSI-X table means guest writes to the table are
intercepted, which costs a check on a hot path for BAR access.**
Accepted cost, and correct: the alternative (letting the guest write the
hardware table) makes delivery depend on guest-controlled memory that the device
also DMA-writes, which is a self-modifying interrupt target. The check is one
range compare on BAR access, not on the data path.

**Objection 2: DMA domains are a half-IOMMU; a guest can point a descriptor at
another device's window only if the domain is misconfigured, so what is the
threat model?**
The threat is a *buggy or compromised device driver in the guest* writing into
another device's ring, and a device implementation (ours) that trusts
descriptors. Domains make the trust explicit and testable, and they are the
cheap half of the work that the IOMMU would otherwise do. They are not a
security boundary against a malicious host, and the doc does not claim it.

**Objection 3: an earlier draft of this design named IMSIC as the RISC-V sink,
but the machine has PLIC/CLINT and no AIA; and it named the LAPIC for x86, which
does not exist either.**
Corrected in §3.1: the PLIC is the sink that lands, the IMSIC sink waits for an
AIA machine, and the x86 side is a pending-vector decoder that explicitly does
not pretend to be a LAPIC. This is the reason the review pass exists - two of
the three sinks named in the first draft were referenced from ACPI *descriptions*
in the tree, not implementations.

**Objection 4: does this conflict with the `mmio-msi-fastpath` name, which
suggests one change?**
It is three changes with one theme (get devices off the slow path), and they are
independent enough to land separately; the naming in the roadmap bundled them.
Splitting is safer: MSI-X delivery is testable without domains, and domains are
testable without MSI.

**Open question:** whether the RISC-V machine should expose MSI-X at all before
AIA exists, given that the PLIC has no per-vector message semantic. Leaning: yes
for *pending/masking* testing, no for the boot gate, so the virtual machine keeps
PLIC wiring as the interrupt source of record.

## 6. Update: the write domain is shared with CPU stores (roadmap 3.2)

The DMA transaction now takes the same critical section as a CPU store: the
embedder installs `lock`/`unlock` hooks (`loongemu_guest_dma_set_write_domain`)
and `LatrSystem` points them at the reservation word
(`latr_reservation_lock/unlock`). Consequences worth keeping in mind:

- The acquire blocks. This is safe only because no CPU store path holds the
  domain across a device call: the emitted fast path performs MMIO outside the
  lock, and the C misaligned path was moved to match. If a future path takes the
  domain and then calls a device, this becomes a deadlock - the rule is
  "the domain covers memory stores, never device calls".
- Nested device transactions are refused by `write_active`, so a device that
  responds to another device's DMA cannot recurse into the domain.
- Multi-byte guest writes are still not guest-atomic (RISC-V guarantees only
  aligned <= XLEN). What the domain provides is mutual exclusion between a
  device write and a CPU store, plus the epoch bump inside the same section.
