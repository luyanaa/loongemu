# LATR 剩余缺口与修复清单

状态约定：`[ ]` 未做，`[x]` 已实现并有当前测试证据，`[~]` 部分实现/有限制。
本轮明确不做：`la-pvir.h` 的目录迁移（会牵动 ARM64 path 依赖）。

## P0 — 系统闭环 / 会直接破坏 OS 或隔离

- [x] **系统主循环**：新增 LATR system board，串联 ELF/固件装载、`target_disasm`、`tr_ir2_generate`、`tr_ir2_assemble`、`latr_enter_tb`、TB 缓存、中断/WFI、SBI、MMIO。
- [x] **TB 缓存热路径**：`tr_ir2_assemble()` 产出的代码进入 `latr_tb_cache_*`；`sfence_pending`/`fence_i_pending` 在循环中消费并拍击；缓存键补根 PPN（避免同 ASID 改 satp 根后命中旧 TB）。
- [~] **宿主 PC 归因消费**：`latr_tb_insn_map_resolve()` 已在缓存 rebase 到 arena 地址后由 `latr_machine_host_fault()` 消费：把 native PC 映射回 guest 指令、恢复 `env.pc`、锁存 `exception/badaddr`、并拍击该 hart 的 TB 让下一次调度从恢复点重新翻译。裸机 smoke 用真实 arena PC 验证通过；VMM 的宿主信号/`ucontext` 入口仍未接。
- [x] **虚拟机器（QEMU virt 风格）**：新增 `system/latr_machine.c` + `include/latr-machine.h`：RAM/固件/内核/initrd 装载契约、`a0=hartid`/`a1=DTB`、M 模式固件入口 → `mret` → S 模式内核、合作式多 hart 调度、每 hart `LatrTranslatorContext`/TB cache/arena。`latr-boot` 的 machine 阶段验证 M→S 转换、DTB magic/内容、UART 与 exit。
- [x] **DTB 生成与传递**：新增 `system/latr_fdt.c` + `include/latr-fdt.h`（FDT v17，struct/strings/rsvmap），生成 `/chosen`（bootargs、stdout-path、initrd-start/end）、`/memory`、`/cpus`（每 hart 的 cpu/intc）、`/soc`（CLINT、PLIC、UART、virtio-mmio）；DTB 放在 RAM 顶部 1 MiB 保留区。
- [x] **OpenSBI 启动链**：stand-in 固件（`mstatus.MPP=S`、`medeleg/mideleg`、`mepc` + `mret`）与**真实 OpenSBI**（QEMU 的 `opensbi-riscv64-generic-fw_dynamic.bin`）两条路径都跑通。
  真实固件从 M 模式入口启动、打出完整 banner，并转换到 S 模式；S 模式 payload 通过 **SBI ecall** 打印，再经 SRST 关机。
  验收：`make latr-opensbi-test` → `PASS opensbi=1 smode=1`（终态 `priv=1 mcause=0x9 exit=0x5555`，`tb==entered==ret`）。
  三层根因（arena 缺 I-cache 同步、`ir2_append` 自环、宿主未开 `EUEN.FPE` 且无 `EENTRY`）见 [`MACHINE.md`](MACHINE.md) §7。
- [x] **ELF 装载器**：修正 program header 字段偏移（`p_vaddr=16`、`p_paddr=24`），补 `ET_EXEC`/`EM_RISCV`、`p_filesz<=p_memsz`、文件/内存区间与整数溢出、装载区间重叠、entry 必须落在已装载段内；负例全部拒绝且不改 RAM。
- [x] **initrd / bootargs 契约**：`latr_machine_load_initrd()` 把 initrd 放在内核之后、DTB 之前并写入 chosen；`latr_machine_set_bootargs()` 写 chosen bootargs。smoke 验证地址区间、字节完整性与 DTB 属性存在。

- [x] **异步中断/WFI 主循环**：`latr_deliver_interrupt()`、`latr_idle_advance()` 嵌入 TB 调度循环；WFI 唤醒与 Sstc/PLIC 联动。
- [x] **CLINT**：实现 MSIP/MTimeCmp/MTIME（每 hart 的 msip、mtimecmp，全局 mtime），通过 MMIO bus 暴露，驱动 MIP.MTIP/MSIP。
- [x] **PMP 执行语义**：首个“任意字节相交”条目 + 全字节覆盖判定；RWX=000 仍匹配；M-mode 仅 L=0 旁路；TOR/NA4/NAPOT；54 位 pmpaddr 掩码；完整 mseccfg.MML 真值表与 MMWP 默认拒绝。
- [x] **跨页标量访存**：`emit_load`/`emit_store` 在 4 KiB 跨页时切分两页（含故障与 MMIO 处理），不再只用起始页翻译。
- [x] **`mstatus.FS` Dirty + FS=Off gating**：所有标量 FP 指令/FP CSR 在 FS=Off 时非法；执行后置 FS=Dirty。
- [x] **`mstatus.VS` Dirty**：所有向量执行路径置 VS=Dirty（当前仅 vset/向量 CSR 写）。
- [x] **`misa` S/U 位**：RV64 的 misa 补齐 S(18)/U(20)；同步测试期望值。
- [x] **机器信息 CSR**：`mhartid/mvendorid/marchid/mimpid/mconfigptr` 可读；`mhartid` 可配置，用于 SMP/EDK2。
- [x] **页表遍历走 `mem_map`**：PTE 物理地址先经 `LoongemuMemMap` 解析，禁止把 guest PPN 当宿主指针解引用；A/D 写回同样受限。
- [x] **统一访存接口与 LR/SC 预约域**：标量慢路径、跨页、RVV lane、PVIR lane 全部走 `latr_guest_load/store`（按 4 KiB 切块，逐块翻译/PMP/MMIO）；所有 CPU 写路径与设备 DMA 共用 `latr_guest_store_lock` 的写锁 + epoch 字，任何写都使预约失效。设备侧现在统一走 `include/loongemu/guest_dma.h`（`loongemu_guest_dma_write/fill/write_le`），通知放在接口内部而不是各调用点，virtio-blk 的 DMA 写、used ring、status byte 与 fw_cfg 的 DMA 写都因此自动生效；`guest_atomic_*` 没有 C 调用者，A 扩展的 LR/SC/AMO 仍由发射代码在同一个锁/epoch 域内实现。

## P1 — 特权/中断/工具链兼容

- [x] **MPRV**：M-mode 数据访存按 mstatus.MPP 降权翻译；SRET 清 MPRV；取指不受 MPRV 影响。
- [x] **Svpbmt / Svade**：PBMTE/ADUE 可写；PBMT≠0 且 PBMTE=0、或 PBMT 编码 3 时 fault；ADUE=0 时 A/D=0 报页故障，ADUE=1 时原子置位且 A/D 写回经 S-mode PMP 检查。
- [x] **Vectored 中断**：mtvec/stvec MODE=1 时按 `BASE + 4*cause` 跳转；异常仍跳 BASE。
- [x] **SRET 清 MPRV**。
- [x] **`target_disasm()` TB 收尾语义**：JAL/JALR/MRET/SRET/ECALL/EBREAK/FENCE_I/VSETVL* 后结束；第 2+ 条指令跨页时在边界前结束。保留单 TB 单元测试的显式兼容开关。
- [x] **SBI 扩展**：DBCN（write/read/write_byte）与 legacy EID 0x00–0x08 已实现并测试；PMU 以 `num_counters=0` 明示无事件后端，counter-indexed FID 返回 INVALID_PARAM。
- [x] **Freestanding extern 白名单 + 门禁**：补 `sprintf/strcat/strcpy/strlen`；`latr-check` 接入 `test-latr`。
- [x] **移除 QEMU 源码路径硬依赖**：内置 `insn32.decode`/`insn16.decode`/`decodetree.py` 回退，`QEMU_SRC` 仅作可选覆盖。
- [~] **SMP**：每 vCPU `LatrTranslatorContext`、TLS 指针（`LATR_TLS`）、独立 TB 缓存/arena/PVIR cache、跨核 ASID/all 拍击与注册表已落地；`latr_machine` 提供 4 hart 的合作式轮转调度，2-hart smoke 验证 hartid/DTB 参数、独立 context/cache 与并发安全点。仍无宿主线程并行 vCPU 和真并发压力测试。
- [x] **TLS / 翻译器上下文**：`lsenv`/`tr_data` 改为 `LATR_TLS`，新增 `LatrTranslatorContext`（IR2 数组、ENV、CPU 指针、TB cache、PVIR cache、代码缓冲）与 bootstrap 兼容上下文；`latr_context_bind/create/destroy/safepoint` 支持每 vCPU 隔离。
- [x] **TB arena 回收安全点**：TB 缓存加自旋锁；shootdown 只失效条目不回收 arena；耗尽时置 `reclaim_pending`，由 owner 在 `latr_tb_cache_safepoint()`（两次 TB 之间）回收；`latr_tb_cache_reclaim_all()` 处理插入失败重试。
- [x] **PVIR cache 并发**：PVIR 描述符缓存从全局改为 per-context（64 槽，无 context 时退回静态 cache）；shootdown 不再清别的 hart 的缓存，改由 context safepoint 在 TB generation 变化时 reset。
- [x] **virtio-mmio block**：新增 `src/riscv_virtio_blk.c` + `include/loongemu/riscv_virtio_blk.h`（modern virtio-mmio v2、单 split virtqueue、RAM 后端、IN/OUT/FLUSH、level IRQ→PLIC 1）；machine 自动加 MMIO window、DTB 节点并支持 `latr_machine_attach_disk()` 的“无盘”降级。描述符链按 queue size 定界并逐跳校验（自环、越界 next、非表内索引都会终止），header/status 方向与长度、`sector * 512` 溢出、`offset + len` 跨盘尾全部拒绝并**仍写入 status byte**；`avail.idx` 超前超过 queue size 时整队丢弃（`dropped` 计数）而不是在前一次 MMIO 写里跑满 65536 条链，`last_avail` 改回 16 位以支持索引回绕。回归见 `make test-riscv-virtio`（88 checks）。

- [x] **中断/委托细节**：U-mode 委托中断不受 SIE 门控（仅 S-mode 本级检查 SIE）；mideleg/medeleg 写掩码；`mip` 可写 MSIP/SSIP 规则；`mip.STIP` 在 CSR 读时刷新；异步投递同时写 `mcause/scause`；PLIC `2*hart` context 驱动 `MIP.MEIP`（cause 11），`2*hart+1` context 驱动 `MIP.SEIP`（cause 9）。
- [~] **ACPI/固件桥接**：`riscv_acpi.c`/`riscv_fw.c`/`ovmf_board.c` 已进 LATR core；system board 生成 MADT/RHCT/FADT/SPCR/XSDT/RSDP、挂接 fw_cfg 与 pflash 槽；boot smoke 已从 guest 执行 fw_cfg DMA SELECT+READ 并从 pflash XIP 执行同一程序。真实 OpenSBI/EDK2 镜像启动仍未接。

## P2 — 性能、差分与可选扩展

- [x] **软件 TLB**：数据/取指共用 64 项直接映射 TLB；条目按 satp+VA 页校验并回读叶 PTE，SFENCE.VMA（全量/ASID）与 FENCE.I 失效；`sfence_pending` 被 runtime 清零后仍有 `tlb_flush_seq` 序号兜底；PMP/SUM/MXR/A/D 在命中路径重新检查。
- [x] **RVV 差分**：`Makefile` 使用 `-march=rv64gcv`，`tests/latr_isa_qemu_ref.S` / `latr_isa_diff_vectors.h` / `baremetal/bare/latr_isa_test.c` 已接入 RVV 差分序列并与 `qemu-system-riscv64 -cpu max` 对齐（`latr-diff` 通过）。
- [x] **系统差分**：Sv39/48/57、timer、PLIC/UART 的 QEMU 差分（`latr-sys-diff` 的 SYS 行逐字节相同）；riscv-tests/arch-test/vector-tests 仍未接入。
- [~] **RVV 1.0 指令补齐**：已登记 308 条（+3 条 `vset*`），**仍缺 58 条编码**（逐名比对 `insn32.decode:340-760`，明细见 [`RVV_P1_LEGALITY.md`](RVV_P1_LEGALITY.md) §2.1）：carry/borrow 10、整数 MAC 8、`vsmul` 2、`vw*.w` 8、FP FMA 24、`vfrsub/vfrdiv/vfrec7/vfrsqrt7/vfslide1*.vf` 6。
  本轮落地 `vrsub.vx/vi` 与 `vmulh/vmulhu/vmulhsu` 的 `.vv/.vx`（共 8 条），走 `latr_rvv_arith_extra` 的 C 路径；
  高位乘在 2·SEW 上求出乘积再右移 SEW，`mulhsu` 按 vs2 有符号、vs1/x 无符号混合。
  **登记位置有陷阱**：把新编码插到 `rv64gc-opcodes.def` 中部（`vmul.vx` 之后）会让 16 项既有 FP 检查（RMM/NaN-box）
  失败，追加到文件末尾则全绿——原因尚未定位，因此**新编码一律追加**，不要按族插入。本轮已落地 whole-register 24 条（`vmv<n>r`/`vl<n>re`/`vs<n>r`）与 `vleff` 4 条。注意 `vandn/vrol/vbrev/vclz` 属 **Zvbb**，不属 RVV 1.0；`vlseg/vsseg` 由 `vle8_v` 的 `nf` 字段承载，不是独立解码入口（`nf>1` 现已真正执行，见 §RVV 合规检查）。
- [x] **RVV 合规检查（操作数合法性 + 运行时门控）**：新增 `translator/rvv-legality.c`（`latr_rvv_check_operands()`，纯函数，可宿主单测），在**翻译期**执行 QEMU `vext_check_*` 全套规则：`require_align`/`require_noover`/`require_nf`/`require_vm`、EMUL ∈ 1/8..8、widening/narrowing 的 2·SEW ≤ ELEN、`vrgather`/`vrgatherei16`/`vcompress`/`viota` 的重叠约束、`vmv.v.v` 的寄存器对规则。`(reg+idx) & 31` 已删除（现在越界组直接 illegal，不再回绕写 `v0`）。`frm ∈ {5,6,7}` 对所有使用舍入模式的向量 FP 指令 illegal；归约族与 `vcpop/vfirst/vms*/viota/vcompress` 统一 `vstart == 0` 门控。负例矩阵：`make -C latr test-legality`（104 checks）+ 裸机 `latr-isa` 26 项陷阱用例。**segment `nf>1`、`vleff`、whole-register、`vlm.v` 的 tail 策略均已落地**（`vlm.v` 改为与 QEMU 默认一致：保持原值），设计与规则对照见 [`RVV_P1_LEGALITY.md`](RVV_P1_LEGALITY.md) §3–§5、§10。
- [ ] **CPU profile 拆分**：`misa` 现在硬编码 `0x8000_0000_0020_112d`（含 `V` bit21）且只读（`translate-riscv-csr.c:99,222,344`），等于向 guest 宣称完整 RVV 1.0。应引入 `rv64gc`（不宣称 V）与 `rv64gcv-experimental` 两种 profile；推荐实现为「`misa` 去掉 V + `mstatus.VS` RAZ/WI」，现有 `emit_vs_gate` 与向量 CSR 门控即自动生效，无需逐指令开关。见 [`RVV_P1_LEGALITY.md`](RVV_P1_LEGALITY.md) §6。
- [x] **PVIR 覆盖与混合 LSX/LASX lowering**：`pvir-exec.c` 补齐 `FIXED`/`EXTRACT`/`INSERT`/`LANE_INDEX`/`LOAD`/`STORE`（含 `vlm.v/vsm.v` packed mask）/带 mask `CONVERT`（含 `vfncvt.rod`）与 signed `INT_MIN/-1` 保护；加入 64 槽 per-TB PVIR 描述符缓存（`latr_pvir_cache_*`）；新增 `translator/pvir-lower-lsx.c` 实现 3 级自适应混合 lowering（Tier 2 LASX `la_xv*` 32B + Tier 1 LSX `la_v*` 16B 同指令混合，Tier 0 回退 C helper），裸机打开 `CSR.EUEN` (`FPE|SXE|ASXE`) 验证 `LMUL=1` LSX、`LMUL=2` LASX、`LMUL=4, vl=12, SEW=32` 混合（1 LASX + 1 LSX）及 `latx_cpu_set_vector_features` 门控；归约与排列仍走 helper。
- [~] **机器扩展/设备**：mhpmevent3..31/mhpmcounter3..31/hpmcounter3..31 与 mstateen0..3 仅有存储/只读/访问控制语义，无真实事件后端；AIA/IMSIC/APLIC、RISC-V PCI ECAM 仍未做（virtio-mmio block 已完成）。
- [x] **文档同步**：已更新 `GAPS.md`、`DESIGN.md`、`RVV_PVIR.md` 反映 RVV/PVIR/混合 LSX+LASX 与系统状态。

## 验证基线（每完成一组重跑）

```sh
make -C latr test-decode
make test-latr
make test-pvir
make test-pvir-rvv-shapes
make test-sve-pvir
make -C latr check
```

## 本轮验证（2026-11-22，SMP + 机器模型轮）

```
make -C latr test-elf      -> 30 checks（ELF64 字段/区间/重叠/entry 负例）
make -C latr test-tb-smp   -> 63 checks（key 分离/淘汰/arena 回收/跨核 shootdown）
make test-riscv-clint      -> 17 checks（MTIMECMP 复位值、per-hart MTIP/MSIP）
make test-latr             -> latr-isa: PASS checks=1542
                              latr-boot: PASS uart=FW ram-cache=4 pflash-tbs=4 map=4
                                         machine=1 disk=1 initrd=1 smp=1 dtb=0x6e0
                              latr-riscv-ref (-march=rv64gcv): PASS
                              latr-diff: PASS
make latr-sys-diff         -> qemu/latr SYS lines identical

# 验收阶梯第 1 级：真实 OpenSBI → S 模式
make latr-opensbi-test     -> OpenSBI v1.8.1 banner（含 platform/domain 信息）
                              [latr] smode up hart=00 / [latr] smode ok
                              rc=1 lastpc=0x802006c priv=1 mcause=0x9 exit=0x5555
                              tb == entered == ret
                              latr-opensbi: PASS opensbi=1 smode=1     (~18 s)
```

`latr-boot` 现分五段：
1. RAM 裸镜像 + pflash XIP；
2. M 模式固件 → `mret` → S 模式内核，`a0=hartid`/`a1=DTB`，DTB magic/内容检查；
3. virtio-mmio block + initrd：MMIO magic、描述符链、used ring、IRQ→PLIC→ACK、initrd 区间；
4. 2 hart 合作式调度：每 hart 独立 context/TB cache/PVIR cache、hartid/a0、UART `KK`；
   新增 **per-hart CLINT 探针**（hart 1 的 MTIMECMP 到期与 MSIP 不得出现在 hart 0 的 `mip`）
   与 **context bind/restore 往返**检查；
5. **无固件直启 S 模式**：校验 `mstatus.MPP=S`、`mideleg=mie=0x222`、`mcounteren=7`、`a1=DTB`。

ELF 装载器已抽成不依赖机器模型的 `system/latr_elf.c`（`include/latr-elf.h`），
这样负例可以在宿主上跑；`p_paddr`（偏移 24）与 `p_vaddr`（偏移 16）分离，
新增 `e_ehsize`、entry 2 字节对齐、`image_end` 越界（firmware 不得压到 `+2 MiB` 内核区）检查。

`MACHINE.md` 记录机器分层、启动契约与验收阶梯。

## 剩余项 blocker 分类

### 无 blocker — 现在可在 LATR 内完成

- [x] 软件 TLB（数据/取指共用；失效钩子可全部落在 `mmu.c` + `satp/sfence` 发射端，裸机可测）。
- [x] 系统差分（Sv39/48/57、timer、PLIC/UART）：`qemu-system-riscv64` 与 LATR board 的 `SYS:` 行逐字节相同。
- [x] HPM/mstateen CSR 子集（存储/只读/访问控制语义；真实事件计数可后置）。
- [x] SBI PMU 协议（`probe=1`、`num_counters=0`；真实计数器依赖 HPM 后端）。
- [x] fw_cfg DMA 交互与 pflash 取指启动 smoke（同一 guest 从 RAM 与 pflash XIP 各跑一遍）。
- [x] ELF 装载器全部字段/区间/重叠/entry/对齐校验与负例（`latr-isa` + 宿主 `test-elf`）。
- [x] 每 vCPU context/TLS/TB cache/PVIR cache 隔离与 2-hart 机器调度（`latr-boot`）；
  TB arena 回收时同步失效宿主 PC 映射；PVIR 缓存加锁；`bind/restore` 供 fault dispatcher 借用。
- [x] virtio-mmio block 设备模型（MMIO/描述符/DMA/IRQ）与 DTB 节点。
- [~] 剩余 RVV 1.0 指令（`vadc/vmadc/vmacc`/FP FMA/`vmulh*`/`vsmul`/`vrsub`/`vw*.w`/`vfr*`）；`vleff`、`segment`、`whole-reg` 已完成，组对齐与重叠合规已完成。

### 部分无 blocker — 可先做局部，完整闭环仍缺外部件

- [~] 宿主 PC 归因：`latr_machine_host_fault()` 已消费 `latr_tb_insn_map_resolve()` 并恢复 guest 状态；VMM 的宿主信号/`ucontext` 入口仍需接。
- [~] SMP：合作式多 hart 与 per-vCPU 架构已完成；真并行 vCPU 需要 VMM 宿主线程，且 freestanding 构建仍以 `-D__thread=` 单线程绑定。
- [~] ACPI/固件：ACPI 表、fw_cfg、pflash 已挂 board；真实 OpenSBI/EDK2 启动缺 RISC-V 固件镜像。
- [~] 归约/排列 PVIR 化：`la-pvir.h`/`pvir.c` 按本轮约束不动；目前由 `rvv.c` helper 执行。

### 有明确 blocker

- [ ] 真并行 SMP 调度：需要 VMM 为每 vCPU 建宿主线程并以 `LATR_ENABLE_TLS` 编译；
  当前 machine 是单线程合作式轮转（`latr_context_bind` 已按线程切换语义实现并返回前值）。
- [~] 真实 OpenSBI 启动：`latr-opensbi-run` 已接入 Homebrew 的真实 `fw_dynamic` 镜像，
  DTB 经 QEMU 验证可启动同一镜像，但 LATR 上固件无 trap 地停在 M 模式自旋、UART 零输出；
  诊断数据与下一步方向见 [`MACHINE.md`](MACHINE.md) §7。
- [ ] 真实 Linux 启动：仓库无 RISC-V `Image`/initramfs/ext4 rootfs；加载器/DTB/initrd/bootargs/virtio 已就绪，缺外部镜像。
- [ ] RISC-V PCI ECAM / MSI 与 AIA/IMSIC/APLIC：需要新的机器设备模型。
- [ ] `la-pvir.h`/ARM64 path 相关 PVIR 核心扩展：按本轮约束不修改 `target/arm/lata/include/la-pvir.h`。
