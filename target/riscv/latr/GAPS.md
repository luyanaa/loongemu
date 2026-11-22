# LATR 剩余缺口与修复清单

状态约定：`[ ]` 未做，`[x]` 已实现并有当前测试证据，`[~]` 部分实现/有限制。
本轮明确不做：`la-pvir.h` 的目录迁移（会牵动 ARM64 path 依赖）。

## P0 — 系统闭环 / 会直接破坏 OS 或隔离

- [x] **系统主循环**：新增 LATR system board，串联 ELF/固件装载、`target_disasm`、`tr_ir2_generate`、`tr_ir2_assemble`、`latr_enter_tb`、TB 缓存、中断/WFI、SBI、MMIO。
- [x] **TB 缓存热路径**：`tr_ir2_assemble()` 产出的代码进入 `latr_tb_cache_*`；`sfence_pending`/`fence_i_pending` 在循环中消费并拍击；缓存键补根 PPN（避免同 ASID 改 satp 根后命中旧 TB）。
- [~] **宿主 PC 归因消费**：boot smoke 已调用 `latr_tb_insn_map_resolve()` 并在缓存 rebasing 后验证；真正的宿主信号处理路径仍未接。
- [x] **异步中断/WFI 主循环**：`latr_deliver_interrupt()`、`latr_idle_advance()` 嵌入 TB 调度循环；WFI 唤醒与 Sstc/PLIC 联动。
- [x] **CLINT**：实现 MSIP/MTimeCmp/MTIME（每 hart 的 msip、mtimecmp，全局 mtime），通过 MMIO bus 暴露，驱动 MIP.MTIP/MSIP。
- [x] **PMP 执行语义**：首个“任意字节相交”条目 + 全字节覆盖判定；RWX=000 仍匹配；M-mode 仅 L=0 旁路；TOR/NA4/NAPOT；54 位 pmpaddr 掩码；完整 mseccfg.MML 真值表与 MMWP 默认拒绝。
- [x] **跨页标量访存**：`emit_load`/`emit_store` 在 4 KiB 跨页时切分两页（含故障与 MMIO 处理），不再只用起始页翻译。
- [x] **`mstatus.FS` Dirty + FS=Off gating**：所有标量 FP 指令/FP CSR 在 FS=Off 时非法；执行后置 FS=Dirty。
- [x] **`mstatus.VS` Dirty**：所有向量执行路径置 VS=Dirty（当前仅 vset/向量 CSR 写）。
- [x] **`misa` S/U 位**：RV64 的 misa 补齐 S(18)/U(20)；同步测试期望值。
- [x] **机器信息 CSR**：`mhartid/mvendorid/marchid/mimpid/mconfigptr` 可读；`mhartid` 可配置，用于 SMP/EDK2。
- [x] **页表遍历走 `mem_map`**：PTE 物理地址先经 `LoongemuMemMap` 解析，禁止把 guest PPN 当宿主指针解引用；A/D 写回同样受限。
- [x] **统一访存接口与 LR/SC 预约域**：标量慢路径、跨页、RVV lane、PVIR lane 全部走 `latr_guest_load/store`（按 4 KiB 切块，逐块翻译/PMP/MMIO）；所有 CPU 写路径与设备 DMA（`latr_reservation_notify_write`）共用 `latr_guest_store_lock` 的写锁 + epoch 字，任何写都使预约失效。`guest_atomic_*` 没有 C 调用者：A 扩展的 LR/SC/AMO 仍由发射代码在同一个锁/epoch 域内实现。

## P1 — 特权/中断/工具链兼容

- [x] **MPRV**：M-mode 数据访存按 mstatus.MPP 降权翻译；SRET 清 MPRV；取指不受 MPRV 影响。
- [x] **Svpbmt / Svade**：PBMTE/ADUE 可写；PBMT≠0 且 PBMTE=0、或 PBMT 编码 3 时 fault；ADUE=0 时 A/D=0 报页故障，ADUE=1 时原子置位且 A/D 写回经 S-mode PMP 检查。
- [x] **Vectored 中断**：mtvec/stvec MODE=1 时按 `BASE + 4*cause` 跳转；异常仍跳 BASE。
- [x] **SRET 清 MPRV**。
- [x] **`target_disasm()` TB 收尾语义**：JAL/JALR/MRET/SRET/ECALL/EBREAK/FENCE_I/VSETVL* 后结束；第 2+ 条指令跨页时在边界前结束。保留单 TB 单元测试的显式兼容开关。
- [x] **SBI 扩展**：DBCN（write/read/write_byte）与 legacy EID 0x00–0x08 已实现并测试；PMU 以 `num_counters=0` 明示无事件后端，counter-indexed FID 返回 INVALID_PARAM。
- [x] **Freestanding extern 白名单 + 门禁**：补 `sprintf/strcat/strcpy/strlen`；`latr-check` 接入 `test-latr`。
- [x] **移除 QEMU 源码路径硬依赖**：内置 `insn32.decode`/`insn16.decode`/`decodetree.py` 回退，`QEMU_SRC` 仅作可选覆盖。
- [~] **SMP**：每 vCPU TB 缓存、跨核 ASID 拍击与注册表已有裸机测试；尚无宿主线程 vCPU 调度和并发压力测试。
- [x] **中断/委托细节**：U-mode 委托中断不受 SIE 门控（仅 S-mode 本级检查 SIE）；mideleg/medeleg 写掩码；`mip` 可写 MSIP/SSIP 规则；`mip.STIP` 在 CSR 读时刷新；异步投递同时写 `mcause/scause`；PLIC `2*hart` context 驱动 `MIP.MEIP`（cause 11），`2*hart+1` context 驱动 `MIP.SEIP`（cause 9）。
- [~] **ACPI/固件桥接**：`riscv_acpi.c`/`riscv_fw.c`/`ovmf_board.c` 已进 LATR core；system board 生成 MADT/RHCT/FADT/SPCR/XSDT/RSDP、挂接 fw_cfg 与 pflash 槽；boot smoke 已从 guest 执行 fw_cfg DMA SELECT+READ 并从 pflash XIP 执行同一程序。真实 OpenSBI/EDK2 镜像启动仍未接。

## P2 — 性能、差分与可选扩展

- [x] **软件 TLB**：数据/取指共用 64 项直接映射 TLB；条目按 satp+VA 页校验并回读叶 PTE，SFENCE.VMA（全量/ASID）与 FENCE.I 失效；`sfence_pending` 被 runtime 清零后仍有 `tlb_flush_seq` 序号兜底；PMP/SUM/MXR/A/D 在命中路径重新检查。
- [x] **RVV 差分**：`Makefile` 使用 `-march=rv64gcv`，`tests/latr_isa_qemu_ref.S` / `latr_isa_diff_vectors.h` / `baremetal/bare/latr_isa_test.c` 已接入 RVV 差分序列并与 `qemu-system-riscv64 -cpu max` 对齐（`latr-diff` 通过）。
- [x] **系统差分**：Sv39/48/57、timer、PLIC/UART 的 QEMU 差分（`latr-sys-diff` 的 SYS 行逐字节相同）；riscv-tests/arch-test/vector-tests 仍未接入。
- [~] **RVV 1.0 指令补齐**：已登记 268 条（+3 条 `vset*`），**仍缺 94 条编码**（逐名比对 `insn32.decode:340-760`，明细见 [`RVV_P1_LEGALITY.md`](RVV_P1_LEGALITY.md) §2.1）：carry/borrow 10、整数 MAC 8、`vmulh*` 6、`vsmul` 2、`vrsub` 2、`vw*.w` 8、whole-register 24、`vleff` 4、FP FMA 24、`vfrsub/vfrdiv/vfrec7/vfrsqrt7/vfslide1*.vf` 6。注意 `vandn/vrol/vbrev/vclz` 属 **Zvbb**，不属 RVV 1.0；`vlseg/vsseg` 由 `vle8_v` 的 `nf` 字段承载，不是独立解码入口（当前 `nf>1` 显式非法）。
- [~] **RVV 合规检查**：已实现 `vcpop/vfirst/vms*/viota/vcompress` `vstart=0` 门控、`vmv.s.x`/`vfmv.s.f` `vl==0` 与 `vta=1` 处理、`vmv.x.s x0` 保护、`vsll/vsrl/vsra.vi` 无符号 5-bit 移位、`LatrTbKey.vspec` 向量特化。**未做且优先级更高**：①lane 寻址的 `(reg+idx) & 31`（`rvv.c:120,125,150,161`）会把非法寄存器组回绕到别的寄存器（含 `v0`），必须是 illegal 而非静默错执行；②`require_align/require_noover/require_nf/require_vm` 全套组合合法性检查；③归约族 `vstart=0` 门控；④`vfcvt` 丢弃 `rm` 字段、向量 FP 保留 `frm` 不 trap。设计与负例矩阵见 [`RVV_P1_LEGALITY.md`](RVV_P1_LEGALITY.md) §3–§5。
- [ ] **CPU profile 拆分**：`misa` 现在硬编码 `0x8000_0000_0020_112d`（含 `V` bit21）且只读（`translate-riscv-csr.c:99,222,344`），等于向 guest 宣称完整 RVV 1.0。应引入 `rv64gc`（不宣称 V）与 `rv64gcv-experimental` 两种 profile；推荐实现为「`misa` 去掉 V + `mstatus.VS` RAZ/WI」，现有 `emit_vs_gate` 与向量 CSR 门控即自动生效，无需逐指令开关。见 [`RVV_P1_LEGALITY.md`](RVV_P1_LEGALITY.md) §6。
- [x] **PVIR 覆盖与混合 LSX/LASX lowering**：`pvir-exec.c` 补齐 `FIXED`/`EXTRACT`/`INSERT`/`LANE_INDEX`/`LOAD`/`STORE`（含 `vlm.v/vsm.v` packed mask）/带 mask `CONVERT`（含 `vfncvt.rod`）与 signed `INT_MIN/-1` 保护；加入 64 槽 per-TB PVIR 描述符缓存（`latr_pvir_cache_*`）；新增 `translator/pvir-lower-lsx.c` 实现 3 级自适应混合 lowering（Tier 2 LASX `la_xv*` 32B + Tier 1 LSX `la_v*` 16B 同指令混合，Tier 0 回退 C helper），裸机打开 `CSR.EUEN` (`FPE|SXE|ASXE`) 验证 `LMUL=1` LSX、`LMUL=2` LASX、`LMUL=4, vl=12, SEW=32` 混合（1 LASX + 1 LSX）及 `latx_cpu_set_vector_features` 门控；归约与排列仍走 helper。
- [~] **机器扩展/设备**：mhpmevent3..31/mhpmcounter3..31/hpmcounter3..31 与 mstateen0..3 仅有存储/只读/访问控制语义，无真实事件后端；AIA/IMSIC/APLIC、virtio-mmio、RISC-V PCI ECAM 仍未做。
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

## 本轮验证（2026-11-22）

```
make test-latr            -> latr-isa: PASS checks=1337
                             latr-boot: PASS uart=FW ram-cache=4 pflash-tbs=4 map=4
                             latr-riscv-ref (-march=rv64gcv): PASS
                             latr-diff: PASS
make latr-sys-diff        -> qemu/latr SYS lines identical
make test-riscv-sbi       -> SBI: 60 checks passed
make -C latr test-decode  -> 26 checks
make test-pvir            -> 222 checks
make test-pvir-rvv-shapes -> 54 checks
make test-sve-pvir        -> 347 checks
make -C latr check        -> 11 external symbols, all allowlisted
```

`latr-boot` 路径：`latr_system_init` → ACPI/RSDP 生成校验 → ELF 装载 → `target_disasm` → TB 缓存 lookup/insert → `tr_ir2_generate/assemble` → `latr_enter_tb` → guest UART（`LATR`）→ exit MMIO → PASS；TbInsnMap 在缓存 rebase 后可由 `latr_tb_insn_map_resolve()` 命中。

## 剩余项 blocker 分类

### 无 blocker — 现在可在 LATR 内完成

- [x] 软件 TLB（数据/取指共用；失效钩子可全部落在 `mmu.c` + `satp/sfence` 发射端，裸机可测）。
- [x] 系统差分（Sv39/48/57、timer、PLIC/UART）：`qemu-system-riscv64` 与 LATR board 的 `SYS:` 行逐字节相同。
- [x] HPM/mstateen CSR 子集（存储/只读/访问控制语义；真实事件计数可后置）。
- [x] SBI PMU 协议（`probe=1`、`num_counters=0`；真实计数器依赖 HPM 后端）。
- [x] fw_cfg DMA 交互与 pflash 取指启动 smoke（同一 guest 从 RAM 与 pflash XIP 各跑一遍）。
- [~] 剩余 RVV 1.0 指令与组对齐合规（`vadc/vmadc/vmacc/vleff/segment/whole-reg`）。

### 部分无 blocker — 可先做局部，完整闭环仍缺外部件

- [~] 宿主 PC 归因：`latr_tb_insn_map_resolve()` 的合成测试已通；真正的宿主信号处理需要 VMM 的 CPU/fault 路径。
- [~] SMP：TB 缓存/跨核 shootdown 的并发单测可独立做；真多线程 vCPU 调度需要 VMM RISC-V machine，且当前 `-D__thread=` 抹掉 TLS（`latr/Makefile:20`）。
- [~] ACPI/固件：ACPI 表、fw_cfg、pflash 已挂 board；真实 OpenSBI/EDK2 启动缺 RISC-V 固件镜像与 VMM machine。
- [~] 归约/排列 PVIR 化：`la-pvir.h`/`pvir.c` 按本轮约束不动；目前由 `rvv.c` helper 执行。

### 有明确 blocker

- [ ] 真 SMP 调度：无 VMM RISC-V machine/CPU loop；`src/` 中没有任何 `latr_` 引用；翻译器全局 `lsenv/tr_data` 非线程局部。
- [ ] 真实 OpenSBI/EDK2 启动：仓库 `firmware/QEMU_EFI.fd` 是 LoongArch EDK2（strings 可见 LoongArchVirt），不是 RISC-V 固件；且无 RISC-V machine。
- [ ] 宿主信号/异常归因全链路：缺 VMM 的宿主 fault 分发。
- [ ] RISC-V PCI ECAM 机器：依赖 VMM machine。
- [ ] `la-pvir.h`/ARM64 path 相关 PVIR 核心扩展：按本轮约束不修改 `target/arm/lata/include/la-pvir.h`。
