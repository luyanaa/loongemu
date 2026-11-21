# LATR: RISC-V RV64GC → LoongArch 翻译器（设计与实现）

状态：**P0/P1 原型已实现，非完整 RV64GC**。QEMU 解码、IR1→IR2 子集与 QEMU-LoongArch 裸机烟测已接通。当前 lowerer 支持范围以 `target/riscv/latr/translator/translate-riscv.c` 为准；已解码不代表可执行，未实现指令会拒绝翻译。完整 F/D、CSR/特权执行、guest MMU 与 RVV 尚未实现。
范围：`riscv64` little-endian；当前实现目标为 RV64I/M/A 与已映射的 RVC 用户态子集。RVV 按本轮决定延期。
设计中不处理：RV32、big-endian、H 扩展、加密扩展、RVV、RV128。

## 0. 证据基准

| 角色 | 来源 |
| --- | --- |
| RISC-V 语义金标准 | `../QEMU-LVZ`（QEMU 9.0.50；`git log -- target/riscv` 只有初始导入 `fc0726f`，`git diff HEAD -- target/riscv` 为空，RISC-V path 确实未被该 fork 修改） |
| RISC-V 编码表 | `../QEMU-LVZ/target/riscv/insn32.decode`、`insn16.decode`；RVV 段在 `insn32.decode:350-737` |
| RVV 执行语义 | `../QEMU-LVZ/target/riscv/vector_helper.c`、`vector_internals.h`、`insn_trans/trans_rvv.c.inc` |
| RISC-V 浮点/NaN-box | `../QEMU-LVZ/target/riscv/fpu_helper.c:27-55`、`internals.h:87-104` |
| LBT 语义/编码 | [Unofficial LoongArch Intrinsics Guide: LBT Basics](https://jia.je/unofficial-loongarch-intrinsics-guide/lbt_basics/)、`../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_lbt.c.inc`、`tcg/op_helper.c` |
| LVZ 语义 | `../QEMU-LVZ/README.md`、`target/loongarch/cpu-csr.h:215-260`、`tcg/tlb_helper.c:721-966` |
| 指令编码 | Homebrew LLVM 23.1.6：`$(brew --prefix llvm)/bin/llvm-mc -triple=riscv64 -mattr=+v`；已验证 `vadd.vv v4,v2,v1,v0.t` → `[0x57,0x82,0x20,0x00]`、`addw a0,a1,a2` → `[0x3b,0x85,0xc5,0x00]` |
| 现有转译结构 | `target/arm/lata/`、`lata/Makefile`、`lata/lata_externals.allowlist` |
| PVIR 核心 | `target/arm/lata/include/la-pvir.h`、`target/arm/lata/pvir.c`、`tests/test_pvir.c` |
| 现有 TB 缓存/页表 | `include/loongemu/tb_cache.h`、`include/loongemu/arm64_paging.h`、`src/arm64_paging.c` |

本文中的 `../QEMU-LVZ` 均相对于仓库根目录 `loongemu/`；`target/...`、`lata/...` 等也相对于仓库根目录。

### 0.1 已核对的现状（避免把计划当成已实现）

- **PVIR 核心/API 已实现并通过主机测试**：`make test-pvir` → `PVIR: 114 checks passed`（本轮实测）。
- **PVIR 尚未接入 IR1→IR2**：`target/arm/lata/translator/translate-a64.c` 没有引用 `la-pvir.h`；`translate_functions[]` 只登记了 `AARCH64_A64_SVE_FEATURE_ABSENT`，`AARCH64_A64_SVE_ACCESS_UNSUPPORTED` 为 `NULL`；PTRUE/ADD/LD1B 等 SVE IR1 tag 没有 lowerer（`translate-a64.c:17023-17153`、`ir1.h:148-151`）。
- **SVE 前端是半成品**：`a64_sve_decode()` 只覆盖代表性子集，且 `translate_functions` 对 SVE allowed 路径没有翻译函数；因此不能把 SVE 的现状当成 RVV 的可复用实现。
- **LATA 的路径是完整的**：`get_ir1_list()`（`translate-a64.c:17329`）→ `translate_aarch64_insn()`（`:17454`）→ `tr_ir2_generate()`（`:17547`）→ `ir2_assemble()`/`tr_ir2_assemble()`（`ir2-assemble.c`、`lata.c:600`）。
- **LBT 的 IR2 落点已存在**：`la-append.h` 已声明 `la_rotr_*`、`la_rotri_*`、`la_jiscr0/1`、`la_gr2scr/scr2gr`、`la_addu12i_*`；LATA 自身没有使用 `jiscr0`，LATX 在 `latx/translator/translate.c:3000-3030` 使用。
- **`test-sve-decode` 在本机主机链接失败**（`Undefined symbols: _stderr`），这是当前工作树的既有问题，与本设计无关；本文不据此声称 LATA 全套测试通过。

## 1. 结论先行

1. **LATR 应做成 LATA 的同构前端，而不是 LATX 的 RISC-V 移植。** RV64GC 是定长 32-bit（RVC 为 16/32 混合）RISC，没有条件码；不需要 Capstone，不需要 x86 EFLAGS/LBT4 状态。复用 LATA 的 IR2、`la-append`、`ir2-assemble`、`reg-alloc`、TB 切换骨架，新增 RISC-V 的 IR1 解码与语义翻译。
2. **RISC-V 基本不需要 LBT。** RISC-V 没有 N/Z/C/V 或 x86 EFLAGS；LBT 的 `x86*/arm*` 标志组、`adc/sbc/rcr` 对 RISC-V 无用。LBT 只在可选 Zbb 的 `rol/ror`（`rotr/rotri`）和 TB 链接（`jiscr0/1`、`movgr2scr`）上有边际收益，且必须保留无 LBT 回退。**不能把 LBT 当作 RISC-V 的关键加速路径。**
3. **LVZ 不能加速 RISC-V 指令翻译。** LVZ 是 LoongArch 的虚拟化扩展（guest/host CSR、GTLB、G-stage、guest 定时器/中断）；它不执行 RISC-V 指令，也不解析 RISC-V 页表。LVZ 只可能在“BT 自身运行在 LoongArch guest 中”时降低上下文切换/隔离成本，或者提供 GID/ASID 范围的 `invtlb` 失效思路；**不要把 LVZ 放进关键路径**。
4. **PVIR 是 RVV predication 的正确语义层，但当前 API 是 SVE 形状。** 现有 PVIR 已覆盖 mask/inactive policy、merge/zero、精确/first-fault 内存语义、restart 状态和 mask 展开缓存；这对 RVV 的硬核心（mask + tail + fault + vstart）很有价值。P0 已放宽 LOAD 的 inactive 结果（`ZERO/MERGE/UNKNOWN`）；仍需：每操作数元素宽度、比较条件、标量/immediate operand、1-bit mask 目的、`first_lane`。动态 lane 数可用 TB 特化规避；fault-only-first 用 `NONFAULTING + RESTART/OTHER` 即可，不需要改核心。未扩展前，PVIR 只能覆盖“全 active 或单一块 mask、同宽度、无 widening”的 elementwise 子集。
5. **RVV 的名义配置选 VLEN=128、ELEN=64、V 1.0。** 128-bit 对应一条 LSX 寄存器；LMUL=1 是 1:1，LMUL=2 是一条 LASX，LMUL>2 按 chunk 展开。LASX（VLEN=256）可选；在 direct lowering 通过差分前，不要向 guest 宣称 VLEN>128。
6. **RISC-V 的 F 与 V 寄存器文件独立，LoongArch 的 FPR/LSX 共享同一物理文件。** 不能像 LATA 的 A64 那样把 V0-V31 和标量 FP 同时 1:1 映射。默认方案：标量 FPR 1:1 映射到 LA FPR，RVV 寄存器放 env，LSX/LASX 只作 chunk 临时寄存器；或者反过来，把标量 FP 放 env、RVV 1:1。这个选择必须在 P1 前定案，否则 P3 会返工。
7. **实施顺序：先 RV64IMAC 整数用户态，再 F/D helper，再 RVV helper，最后 LSX/LASX direct lowering。** 每一阶段都以 QEMU-LVZ 的同一输入做差分；不通过差分不进入下一阶段。

## 2. 复用 LATA 的边界

### 2.1 流程

```mermaid
flowchart LR
  G[guest PC] --> D[get_ir1_list]
  D --> F[fetch 16/32-bit RISC-V]
  F --> I[IR1 list: tag + args + pc + len]
  I --> T[translate_functions]
  T --> S[scalar IR2 lowering]
  T --> P[PVIR build + validate]
  P --> L[PVIR lower: LSX/LASX or helper]
  S --> R[IR2 list]
  L --> R
  R --> A[ir2_assemble]
  A --> C[LoongArch machine code]
  C --> X[context_switch_native_to_bt]
```

### 2.2 直接复用

| 组件 | 处理 |
| --- | --- |
| `target/arm/lata/ir2/ir2.c` | 复用；`#include "lata.h"` 条件化为 LATR 头，或让 LATR 提供同名字段。 |
| `target/arm/lata/ir2/ir2-assemble.c` | 原样复用；只依赖生成的 `format-table.h`。 |
| `target/arm/lata/ir2/la-append.c` + `la-append.h` | 原样复用；RISC-V 所需的 base/LSX/LASX/LBT 指令已存在（见 §4、§5）。 |
| `target/arm/lata/include/la-ir2.h`、`ir2-opcode.h`、`format-table.h` | 原样复用。 |
| `target/arm/lata/translator/reg-alloc.c` 的**通用分配器** | 复用 `ra_alloc_*`/`RA_ALLOC_FUNC`/`get_la_ir2_opnd` 的算法；但同一文件里的 `reg_gpr_map[]`、`reg_fpr_map[]`、`reg_statics_map[]`、`la_ir2_opnd_tab[]` 是 x86/A64 专用，不能直接链接。LATR 要么复制该文件并替换这些表，要么把 LATA 的文件拆成 common + arm 两份。 |
| `target/arm/lata/translator/macro-helper.c` | 复用 `li_d/li_w/li_wu`、按宽度 load/store；它和 `include/reg-alloc.h` 都 `#include "lata.h"`，LATR 需要 `#ifdef CONFIG_LATR` 条件化到 `latr.h`，或在 LATR include path 提供转发头（`lata.h` → `latr.h`）。 |
| `target/arm/lata/mem.c` | 复用（`mm_malloc` 等）。 |
| `fpu/softfloat.c`（`fpu -> latx/upstream/fpu`）、`latx/translator/tr-softfpu.c` | F/D helper 的 softfloat 与转换/NaN 处理模板；LATR 的 C helper 复用同一份 softfloat，避免自己写 IEEE-754。 |
| `target/arm/lata/lata.c` 的 prologue/epilogue/context switch | 复制后改寄存器表、去掉 ARM PSTATE/FCSR 代码；换成 RISC-V `frm/fflags` 同步（TB 出口/helper 前后）。保留 `lata_env_vld/vst` 的 SI12 物化策略。 |
| `src/arm64_paging.c` 的 walker 结构 | 作为 Sv39/Sv48/Sv57 walker 的模板（不是直接复用代码）。 |
| `include/loongemu/tb_cache.h` | 复用；增加 RISC-V key 的打包约定（见 §3.5）。 |

**PVIR 的位置**：PVIR 虽然当前放在 `target/arm/lata/`，但它不依赖 ARM（`la-pvir.h` 只用 `<stdbool.h>/<stddef.h>/<stdint.h>`）。LATR 复用前建议把它提升到 `include/loongemu/` + `src/`（或 `libs/`），并同步更新 `lata/Makefile`、根 `Makefile` 的 `test-pvir` 路径。否则 LATR 会从 ARM 目录 include，后续 SVE/RVV 两套前端共享同一个核心时会出现所有权不清的问题。这是一次小重构，不是必须在本轮完成，但应在 P3 前定。

### 2.3 不能复用 / 必须替换

| 组件 | 原因 |
| --- | --- |
| `target/arm/lata/ir1/decode_ir1_a64.c.inc`、`ir1.c` | AArch64 专用；LATR 用 RISC-V 的 16/32-bit 解码。 |
| `target/arm/lata/translator/translate-a64.c` | 17k 行 AArch64 语义；替换为 `translate-riscv.c`。 |
| `target/arm/lata/optimization/clear-high.c` | AArch64 的 W 写是零扩展；RISC-V 的 32-bit 结果是**符号扩展**。LATR 禁止启用 `clearGprHigh`。 |
| `target/arm/lata/include/env.h`、`lata/compat/target/arm/cpu.h` | ARM 状态布局；LATR 建 `target/riscv/latr/include/riscv-env.h`。 |
| A64 SVE 前端 | 半成品，且 SVE 的谓词/FFR 模型不能直接套 RVV 的 v0/vstart/vl。 |

### 2.4 建议的目录与构建布局

```text
target/riscv/latr/
  include/
    latr.h              # ENV/TRANSLATION_DATA/lsenv, latr_la_map
    ir1.h               # RISCV_INSN_TYPE, RiscvIR1
    ir1-arg.h           # operand union
    riscv-env.h         # CPUArchState + env_offset_*
    reg-map.h           # guest<->LA register maps
    translate.h         # DisasContext/DisasContextBase shim
    pvir-riscv.h        # RVV-specific PVIR bridge types
  ir1/
    decode-riscv.c      # 16/32-bit decode, RVC expansion
    decode_riscv.c.inc  # generated from a curated .decode, committed
  translator/
    translate-riscv.c   # IR1 -> IR2 / helper / PVIR
    reg-alloc.c         # LATR maps + generic allocator (copy/adapt from LATA)
    pvir-bridge.c       # IR1 -> PvirInstruction, value IDs/versions
    pvir-lower-lsx.c    # PVIR -> LSX/LASX IR2
    pvir-state.c        # v0/vtype/vl/vstart versions, mask cache
  latr.c                # register tables, prologue/epilogue, GPR/FPR access
latr/Makefile           # mirrors lata/Makefile
latr/latr_externals.allowlist
tests/test_latr_decode.c
tests/test_latr_pvir.c
baremetal/bare/latr_isa_test.c
```

`latr/Makefile` 以 `lata/Makefile` 为模板：

```make
LLVM  := $(shell brew --prefix llvm)
CLANG := $(LLVM)/bin/clang
AR    := $(LLVM)/bin/llvm-ar
RESINC := $(shell $(CLANG) -print-resource-dir)/include
LATR   := ../target/riscv/latr
LATA   := ../target/arm/lata
LATX   := ../latx
DEFS   := -DTARGET_RISCV64 -DCONFIG_LATR -DCONFIG_LATR_STANDALONE \
          -D__WORDSIZE=64 -DCONFIG_LATX -D__thread=
SRCS   := $(LATA)/ir2/ir2.c $(LATA)/ir2/ir2-assemble.c $(LATA)/ir2/la-append.c \
          $(LATA)/mem.c $(LATA)/pvir.c $(LATA)/translator/macro-helper.c \
          $(LATR)/latr.c $(LATR)/translator/reg-alloc.c \
          $(LATR)/ir1/decode-riscv.c $(LATR)/translator/translate-riscv.c \
          $(LATR)/translator/pvir-bridge.c \
          $(LATR)/translator/pvir-lower-lsx.c \
          $(LATR)/translator/pvir-state.c \
          $(LATX)/runtime/cpu_features.c
```

`latr-check` 复用 `tools/check_lata_symbols.sh` 的 allowlist 检查；`latr-probe` 复用 LATA 的逐文件 `-fsyntax-only` 模式。

F/D helper 需要把 `fpu/softfloat.c`（以及它 include 的 `softfloat-specialize.c.inc` 等）加入 SRCS；LATX 已经在 freestanding 构建里验证过这套 softfloat，LATR 不要另写一套浮点实现。上面的 SRCS 为了简洁没有列出 softfloat，P2 时补上并更新 allowlist。

## 3. RISC-V 前端：IR1 与解码

### 3.1 IR1 记录

RISC-V 需要比 A64 多两个字段：**指令长度**和**原始 PC**。RVC 让 PC 不是简单的 `+4`。

```c
typedef struct RiscvIR1 {
    uint64_t pc;            /* 本条指令地址 */
    uint32_t insn;          /* 已解压的 32-bit 语义指令（RVC 已展开） */
    uint16_t raw_len;       /* 2 或 4，用于 pc+len 链接和页边界判断 */
    uint16_t type;          /* RISCV_INSN_TYPE */
    union {                 /* 复用 LATA 风格的 typed arg */
        RiscvArgI  i;
        RiscvArgR  r;
        RiscvArgS  s;
        RiscvArgB  b;
        RiscvArgU  u;
        RiscvArgJ  j;
        RiscvArgV  v;       /* vd/vs1/vs2/vm/nf/... */
        ...
    } arg;
    uint8_t flags;          /* 分支/跳转/系统/向量配置 */
} RiscvIR1;
```

`DisasContext` 保留 `base`、`pc_curr`、`insn`、`insn_len`、`type`、`arg`，并增加 RVV 的 `vtype/vl/vstart/vill`（若采用 TB 特化，见 §5.5）。不要复用 A64 的 `AARCH64_A64_*` 枚举名；新增 `RISCV_INSN_*`，避免两个 ISA 的 tag 混用。

### 3.2 解码策略：生成式优先

RISC-V 的 32-bit 主空间规整，但 RVC、V、Zba/Zbb/Zbs 的编码分散；手写 600-900 行是可行的（LATM 的方案），但 RISC-V 的扩展组合更多，手写更容易漏掉保留编码和冲突。建议：

1. **以 QEMU-LVZ 的 `insn32.decode`/`insn16.decode` 为编码金标准**，不重新发明位域。
2. 以 QEMU `scripts/decodetree.py` 生成 16/32-bit 解码器；薄包装把 `trans_<name>(DisasContext *, arg_*)` 归一化到 LATR `RiscvIR1`。支持的 opcode 由 manifest 管理，其余生成拒绝包装。
3. QEMU 表含 H、crypto、Zfh、Zfa、V 等扩展；LATR 不声明/实现的扩展不得被当作可执行支持。
4. 当前构建时直接依赖相邻 `../QEMU-LVZ` 中的 `.decode` 与 `decodetree.py`，不提交生成物；因此独立分发需提供该 QEMU 源树或另行 vendoring。
5. 对 RVC：先解码成规范化 32-bit 操作并保留 `raw_len`；链接值用原始指令长度计算。

备选方案是手写两级 switch（LATM 风格）。如果生成式工具链在 freestanding 构建里太麻烦，手写也可以，但必须用 QEMU 的 decode 表逐项对拍，尤其是 `OP-V` 的 312 条和 compressed 的 16-bit 空间。

### 3.3 TB 循环与页边界

RISC-V 的取指规则（QEMU-LVZ `translate.c:1123-1252`）：

- `insn_len(first_word) = (first_word & 3) == 3 ? 4 : 2`。
- 一条 4-byte 指令可以跨页，但**只有 TB 的第一条**允许跨页；后续指令跨页则 `DISAS_TOO_MANY` 结束 TB。
- RVC 的 2-byte 指令也要检查 `pc_next + len - 1` 是否同页。
- 没有延迟槽。向前的直接分支仅在目标是当前 TB 中更后的已解码指令边界时内联；自环/后向分支、间接跳转、系统指令和 `vsetvl` 必须写回 PC 并退出，避免翻译块形成无界循环。
- `tb->size = pc_next - pc_first`，`tb->icount = n`；IR1 的 `pc` 必须逐条保存，不能假设 `pc_first + 4*i`。

建议的循环：

```c
while (n < max_insns) {
    uint16_t first = fetch16(pc);
    int len = insn_len(first);
    if (n != 0 && !same_page(pc, len)) { is_jmp = DISAS_TOO_MANY; break; }
    ir1[n] = decode_one(pc, first, len);
    pc += len;
    n++;
    if (ir1[n-1].type is branch/jump/system/vsetvl) { is_jmp = DISAS_NORETURN; break; }
    if (n == max_insns) { is_jmp = DISAS_TOO_MANY; break; }
}
```

### 3.4 IR1 分支语义

| 指令 | 链接值 | 目标 | TB 处理 |
| --- | --- | --- | --- |
| `BEQ/BNE/BLT/BGE/BLTU/BGEU` | 无 | `pc + sext(imm)` | 仅向前块内目标内联；其他目标走 TB 出口 |
| `JAL` | `rd = pc + len` | `pc + sext(imm)` | 仅向前块内目标内联；其他目标走 TB 出口 |
| `JALR` | `rd = pc + len` | `(rs1 + sext(imm)) & ~1` | 总是走 TB 出口 |
| `C.J` | 无 | `pc + sext(imm)` | 同 `JAL` |
| `C.BEQZ/C.BNEZ` | 无 | `pc + sext(imm)` | 同条件分支 |
| `C.JAL`（仅 RV32） | `x1 = pc + 2` | 同上 | 同 `JAL` |
| `C.JR` | 无 | `rs1 & ~1` | 总是走 TB 出口 |
| `C.JALR` | `x1 = pc + 2` | `rs1 & ~1` | 总是走 TB 出口 |
| `ECALL/EBREAK` | 无 | trap | 是 |
| `MRET/SRET` | 无 | `mepc/sepc` | 是 |
| `WFI` | 无 | 等中断 | 是 |
| `SFENCE.VMA` | 无 | 清 TLB/TB | 是 |
| `FENCE.I` | 无 | 清 TB/icache | 是 |
| `VSETVL/VSETVLI/VSETIVLI` | `rd = vl` | 下一条 | 是（见 §5.5） |

直接分支只有在目标位于当前 TB 的**后续指令边界**时才可直接 `la_b/la_beq/...`；自环、后向或外部目标更新 `env->pc` 并走 TB 出口。`JALR` 的 bit 0 必须清零；`C.JR` 的 `rs1=0` 是保留编码，按非法指令处理。

LATR 的 `gen_goto_tb(s, n, diff)` 不能照抄 LATA 的固定 `4`：fall-through 的 `diff` 必须是本条的 `raw_len`（2 或 4），链接值 `pc+len` 同理。否则 RVC 的 `C.JAL/C.JALR` 和 fall-through 都会错 2 字节。

### 3.5 TB flags 与向量上下文

RVV 的 `vl/vtype/vstart` 是运行时状态，但 QEMU 的 `vl` 是 TCG global，不是 TB 常量（`translate.c:40`、`:1194` 只有 `VL_EQ_VLMAX`）。LATR 若要把 PVIR 的静态 `lanes` 用起来，必须**把向量上下文放进 TB key**：

```c
/* 建议的 RISC-V TB key 打包，不改变 TbCacheEntry 的字段宽度 */
pc       = guest pc;
cr3      = satp;                       /* 地址空间 */
cpl      = priv;                       /* M/S/U */
cs_base  = (vtype & 0xff) | ((uint64_t)vl << 8) |
           ((uint64_t)vstart << 24) | ((uint64_t)frm << 40) |
           ((uint64_t)mstatus_mxr_sum << 48) | ((uint64_t)vill << 50) |
           ((uint64_t)ext_profile << 51);
mode_flags = (satp_mode & 0xf) | ((fs & 3) << 4) | ((vs & 3) << 6);
```

- `vsetvl/vsetvli/vsetivli` 结束 TB 并更新 `env->vl/vtype/vstart`；下一条 TB 用新的 key 重新翻译。
- `vstart != 0` 的 TB 只允许 helper 路径（或单独 key），因为 direct lowering 需要从 lane `vstart` 开始。
- `frm` 变化（写 `fcsr`）也应结束 TB 或纳入 key；QEMU 在写 system register 时结束 TB（`translate.c` 的 CSR 路径），LATR 照做即可。
- 写 `vstart`/`vxrm/vxsat/vcsr` 也要结束 TB 或纳入 key：`vstart` 决定向量指令的起始 lane，固定点/饱和类 lowering 依赖 `vxrm`，不能拿旧 TB 复用。
- `vill=1` 必须纳入 key（上面的 `cs_base` bit 50）；vill 时所有向量指令按 illegal instruction，不能复用 vill=0 的 TB。
- 这样 PVIR 的 `lanes = vl`、`active_lanes`（tail 前缀）就是翻译期常量；v0 的 mask 仍是运行时值，用 `PVIR_PREDICATE_VECTOR_MASK` 表达。

## 4. RV64GC 的 IR2 降级

### 4.1 寄存器映射（建议）

RISC-V 有 31 个可写 GPR（x0 恒零）。LATA 的 A64 映射只直映 21 个 guest 槽（`arm_la_map[]` 中非 -1 的项），其余走 env；LATR 的容量相同，因此至少 10 个 guest GPR 必须 spill。下面是一张**自洽的候选表**（不是最终性能调优结果）：

保留给运行时的 LA 寄存器：

| LA | 用途 |
| --- | --- |
| `$zero` | 常量 0 |
| `$sp` | host 栈指针（TB prologue/epilogue/helper 调用） |
| `$tp` | host TLS（宿主 `__thread`；bare-metal 构建为空，但不要假设） |
| `$s8` | `env` 指针（沿用 LATA 的 `S_ENV`） |
| `$a0` | TB 出口/helper 参数 0 |
| `$t0-$t5` | 6 个 itemp |

候选直映（21 个 guest）：

| guest | LA | guest | LA |
| --- | --- | --- |
| `x1 ra` | `$ra` | `x13 a3` | `$a4` |
| `x2 sp` | `$s7` | `x14 a4` | `$a5` |
| `x3 gp` | `$fp` | `x15 a5` | `$a6` |
| `x4 tp` | `$r21` | `x16 a6` | `$a7` |
| `x5 t0` | `$t6` | `x17 a7` | `$s2` |
| `x6 t1` | `$t7` | `x18 s2` | `$s3` |
| `x7 t2` | `$t8` | `x19 s3` | `$s4` |
| `x8 s0/fp` | `$s0` | `x20 s4` | `$s5` |
| `x9 s1` | `$s1` | `x21 s5` | `$s6` |
| `x10 a0` | `$a1` | `x22 s6` | spill |
| `x11 a1` | `$a2` | `x23 s7` | spill |
| `x12 a2` | `$a3` | `x24 s8` | spill |

其余 spill（走 env）：`x25 s9`、`x26 s10`、`x27 s11`、`x28 t3`、`x29 t4`、`x30 t5`、`x31 t6`。

这张表只是候选，**不要直接照抄**：`$a0` 是 TB 出口/helper 参数，`$s8` 是 env 指针，`$tp` 是宿主 TLS，`$ra` 需要保存 host 返回地址（LATA 把 `$ra` 当 guest x0，LATR 不能这么做）。LATR 应重新做一次寄存器压力测量后再定表；原则是：

- `x0` 永远返回 `zero_ir2_opnd`，`alloc_gpr_dst(0)` 返回可丢弃的 itemp，`store_gpr_dst(0)` 不发射写。
- `sp` 必须映射到可用的 LA 寄存器（不能是 host `$sp`）；TB 出口/helper 前后要与 env 同步。
- `ra` 是普通 guest 寄存器；LATR 不用 `$ra` 做 guest x0（那是 LATA 的 A64 特例）。
- spilled guest 寄存器通过 `la_ld_d/la_st_d` 访问 env，复用 LATA 的 `alloc_gpr_src/dst` 模式。
- `$t0-$t5` 作为 itemp 时，不能同时作为 guest 映射；helper 调用前后要保存/恢复 guest 映射。
- **重写 `reg_statics_map`**：LATA 把 `S_UD0/S_UD1` 放在 `$fp/$r21`（`target/arm/lata/translator/reg-alloc.c`），而本表把这两个寄存器分给 guest x3/x4。LATR 不能沿用这两个 statics；要么删掉未使用的 `S_UD*`，要么把它们指到不用的槽。`S_ENV=$s8`、`S_EFLAGS` 对 RISC-V 无意义（RISC-V 无 flags），只保留 `S_ENV`。

### 4.2 32-bit 符号扩展

RISC-V RV64 的 `*W` 指令把 32-bit 结果符号扩展到 64。LoongArch 的 `add.w/sub.w/sll.w/srl.w/sra.w/addi.w` 正是这个语义。规则：

- 所有 `*W` 指令用 `.w` 形式，**不要**用 `bstrpick.d` 零扩展。
- 如果某条序列以 64-bit 操作产生 32-bit 语义结果，最后补一条 `la_slli_w(rd, rd, 0)`（符号扩展），不要补 `bstrpick.d`。
- **禁用 LATA 的 `clearGprHigh` 优化**（`target/arm/lata/optimization/clear-high.c`）；它做的是 AArch64 的零扩展。

### 4.3 整数/分支/访存

| RISC-V | IR2 | 备注 |
| --- | --- | --- |
| `LUI rd, imm20` | `la_lu12i_w(rd, imm20)` | 32-bit 符号扩展到 64，匹配 RISC-V。 |
| `AUIPC rd, imm20` | `la_pcaddu12i(rd, imm20)` | `pc + sext(imm20<<12)`；LA 的 si20 语义匹配。 |
| `ADDI/ADDIW` | `la_addi_d/la_addi_w` | imm12 直接；匹配。 |
| `ADD/SUB/AND/OR/XOR` | `la_add_d/la_sub_d/la_and/la_or/la_xor` | 64-bit。 |
| `ADDW/SUBW` | `la_add_w/la_sub_w` | 结果符号扩展。 |
| `SLL/SRL/SRA` | `la_sll_d/la_srl_d/la_sra_d` | 低 6 位。 |
| `SLLW/SRLW/SRAW` | `la_sll_w/la_srl_w/la_sra_w` | 结果符号扩展。 |
| `SLT/SLTU` | `la_slt/la_sltu` | 64-bit。 |
| `SLTI/SLTIU` | `la_slti/la_sltui` | si12；否则 `li_d`+`slt`. |
| `ANDI/ORI/XORI` | `la_andi/la_ori/la_xori` | 12-bit 无符号；否则 `li_d`+逻辑。 |
| `BEQ/BNE/BLT/BGE/BLTU/BGEU` | `la_beq/la_bne/la_blt/la_bge/la_bltu/la_bgeu` | 16-bit 分支偏移足够覆盖 RISC-V ±4 KiB。 |
| `JAL` | `la_b` 或 TB 出口 | 目标在 TB 内用 `la_b`；否则更新 `env->pc`。 |
| `JALR` | `la_jirl(zero, target, 0)` 或 TB 出口 | 目标先 `(rs1+imm)&~1`。 |
| `LB/LBU/LH/LHU/LW/LWU/LD` | `la_ld_b/la_ld_bu/la_ld_h/la_ld_hu/la_ld_w/la_ld_wu/la_ld_d` | 地址 `rs1+sext(imm12)`；匹配 SI12。 |
| `SB/SH/SW/SD` | `la_st_b/la_st_h/la_st_w/la_st_d` | 同上。 |

注意：RISC-V 的 `LW` 是符号扩展，`LWU` 才是零扩展；不要混用 `la_ld_w`/`la_ld_wu`。`LD` 是 64-bit。store 的宽度由指令决定，不因寄存器宽度改变。

### 4.4 M/A 扩展

#### M

| RISC-V | IR2 | 备注 |
| --- | --- | --- |
| `MUL` | `la_mul_d` | 低 64 位。 |
| `MULH` | `la_mulh_d` | 有符号×有符号高 64。 |
| `MULHU` | `la_mulh_du` | 无符号×无符号高 64。 |
| `MULHSU` | `la_mulh_d` + 修正 | `mulhsu(a,b) = mulh.d(a,b) + (b<0 ? a : 0)`；需要 `slti`/`masknez` 或分支。 |
| `DIV/DIVU` | `la_div_d/la_div_du` + 显式修正 | 见下。 |
| `REM/REMU` | `la_mod_d/la_mod_du` + 显式修正 | 见下。 |
| `MULW` | `la_mul_w` | 低 32 位，结果符号扩展。 |
| `DIVW/DIVUW/REMW/REMUW` | `la_div_w/la_div_wu/la_mod_w/la_mod_wu` + 显式修正 | 32-bit 结果符号扩展。 |

**除法边界是必测项**。RISC-V 定义（RV64）：

- `x / 0 = -1`（所有位 1）；`x % 0 = x`。
- `INT64_MIN / -1 = INT64_MIN`；`INT64_MIN % -1 = 0`。
- 无 trap。

LoongArch 的 `div.d/mod.d` 不保证这些值；QEMU-LVZ 的 LA 翻译甚至显式把除数和 `min/-1` 归一化为 1 以避免宿主除零（`target/loongarch/tcg/insn_trans/trans_arith.c.inc:111-145`）。因此 LATR 必须：

1. 先判断 `rs2 == 0`：DIV 写全 1，REM 写 rs1，结束。
2. 再判断 `rs1 == INT_MIN && rs2 == -1`：DIV 写 INT_MIN，REM 写 0。
3. 否则才发 `div/mod`。

用分支 + itemp 发射，或用一个 C helper。第一阶段建议 helper，避免每条除法都展开 4-5 条。

#### A

| RISC-V | IR2 | 备注 |
| --- | --- | --- |
| `LR.W/LR.D` | `la_ll_w/la_ll_d` 或 helper | 见下。 |
| `SC.W/SC.D` | `la_sc_w/la_sc_d` + `xori rd,rd,1`，或 helper | LA `sc rd,rj,si14` 用 `rd` 同时作存储值和结果；RISC-V 的 `rs2` 是存储值、`rd` 是结果。`rd != rs2` 时先 `mov rd,rs2`。LA 返回 1=成功/0=失败，RISC-V 返回 0=成功/1=失败，需取反。 |
| `AMOSWAP.W/D` | `la_amswap_w/d` | rd=旧值。 |
| `AMOADD.W/D` | `la_amadd_w/d` | rd=旧值。 |
| `AMOAND/OR/XOR` | `la_amand/amor/amxor` | 同上。 |
| `AMOMAX/MIN` | `la_ammax/ammin` | 有符号。 |
| `AMOMAXU/MINU` | `la_ammax_*u/ammin_*u` | 无符号。 |
| `aq/rl` | `_db` 变体 | `am*.db` 提供 acquire/release；普通 AMO 用非 db。 |

**LR/SC 的语义选择**：QEMU-LVZ 的 LA `sc` 不是硬件 reservation，而是 `lladdr/llval + cmpxchg` 的值比较（`trans_atomic.c.inc:20-46`）：地址相等且内存仍等于上次 LR 读到的值才成功。注意它写回的是 **LA 约定：1=成功、0=失败**（`gen_sc` 在地址不匹配时写 0，成功时写 1）；RISC-V 的 SC 是 **0=成功、1=失败**。因此即使复用这个 helper，也必须把返回值取反，或让 LATR helper 直接按 RISC-V 约定返回。RISC-V 的 LR/SC 允许 reservation-based 实现，也允许 spuriously fail，但**如果用 LA 硬件 `ll/sc`，同值 store 后的 SC 会失败，而 QEMU 的值比较会成功**。为了与 golden 差分一致，第一阶段用 QEMU 风格的值比较 helper（env 里存 `load_res/load_val`，SC 做 cmpxchg，返回 RISC-V 的 0/1）；在真实硬件上再评估是否切到 LA `ll/sc`。SMP 时 helper 需要原子 cmpxchg（LA `amcas` 或宿主锁），单核先做。注意：`amcas` 在 `latx/inst_template.json` 里有编码，但 LATA 生成的 `la-append.h` 没有包装；要用就得重新生成 `la-append` 或走 C helper。

`AMO` 的 `aq/rl` 映射到 `_db` 变体是近似：`dbar` 的 hint 语义比 RISC-V 的 `aq/rl` 弱/强关系需要逐条核对；第一阶段可以统一用 `_db` 保守实现，性能阶段再区分。

**AMO 操作数顺序**：LA `amadd.w rd, rk, rj` 的地址在 `rj`、值在 `rk`；RISC-V `amoadd.w rd, rs2, (rs1)` 的地址在 `rs1`、值在 `rs2`。发射时是 `la_amadd_w(rd, rs2, rs1)`，不要按源码顺序照抄。`LA am*` 的 `rd` 是旧值，RISC-V 也是旧值，这一点一致。

### 4.5 F/D 扩展：helper 优先

F/D 是“看起来能直接映射、实际最容易差一位”的部分。建议第一阶段全部走 C helper（复用 `fpu/softfloat.c`），只把 `FLW/FSW/FLD/FSD` 的直接访存和 `FMV.X.W/FMV.W.X` 直接映射；等差分稳定后再逐条 direct lower。

关键差异：

1. **NaN-boxing**：RISC-V 的 32-bit FP 值在 64-bit FPR 中必须高 32 位全 1；读 `fadd.s` 时如果高 32 位不是全 1，输入按 canonical NaN（`0x7fc00000`）处理。QEMU-LVZ 的 `check_nanbox_s()` 做这个检查（`internals.h:97-104`）。LoongArch 的 `fld.s` 在 QEMU-LVZ 里也会 NaN-box（`target/loongarch/tcg/insn_trans/trans_fmemory.c.inc:6-11`），但 LA 的标量 FP 指令是否对非 boxed 输入做同样检查，需要硬件/QEMU 验证；不能假设。
2. **Canonical NaN**：RISC-V 规定算术产生的 NaN 是 canonical（`0x7fc00000`/`0x7ff8000000000000`）；LA 的默认 NaN payload 可能不同。逐位差分时必须用 helper 或后处理。
3. **fflags**：RISC-V 的 NV/DZ/OF/UF/NX 是 sticky；QEMU 的 `riscv_cpu_get_fflags()/set_fflags()`（`fpu_helper.c:27-55`）给出位映射。LA 的 FCSR flag 位布局不同，不能直接整字搬运。
4. **frm**：RISC-V 的 `frm` 字段是 3-bit（RNE/RTZ/RDN/RUP/RMM/DYN）；`ROD`（round-to-odd）在 QEMU 内部编码为 8，不在这 3-bit 字段里，只有特定指令/向量转换使用。LA 的 FCSR rounding 也是 3-bit 但编码不同；需要映射表，且 `ROD` 走 helper。
5. **fmin/fmax**：RISC-V 对 NaN 和 ±0 的规则与 LA `fmin/fmax`/`fmina/fmaxa` 不完全一致；helper 优先。

直接映射的候选（差分通过后）：

| RISC-V | IR2 |
| --- | --- |
| `FADD.S/D` | `la_fadd_s/d` |
| `FSUB.S/D` | `la_fsub_s/d` |
| `FMUL.S/D` | `la_fmul_s/d` |
| `FDIV.S/D` | `la_fdiv_s/d` |
| `FSQRT.S/D` | `la_fsqrt_s/d` |
| `FMADD/FMSUB/FNMSUB/FNMADD` | `la_fmadd/fmsub/fnmsub/fnmadd_*` |
| `FSGNJ/FSGNJN/FSGNJX` | `la_fcopysign_*` + 符号操作，或 helper |
| `FCLASS` | `la_fclass_*`（位定义需对拍） |
| `FCVT.*` | `la_ftint*/ffint*/fcvt_*`（舍入/flag 需对拍） |
| `FEQ/FLT/FLE` | `la_fcmp_cond_*`（NaN 语义需对拍） |
| `FLW/FSW` | `la_fld_s/fst_s`（QEMU-LVZ 的 LA `fld.s` 已 NaN-box；硬件需确认） |
| `FLD/FSD` | `la_fld_d/fst_d` |

`FLW` 用 `la_fld_s` 是安全的，因为 QEMU-LVZ 的 LA `fld.s` 会 NaN-box；`FSW` 只写低 32 位。`FMV.X.W` 需要 `la_movfr2gr_s`；`FMV.W.X` 需要 `la_movgr2fr_w`（并确保高 32 位 NaN-box）。

### 4.6 C / Zicsr / Zifencei

- **C**：在 IR1 解码阶段解压，不进入 IR2；链接值用 `raw_len`。RVC 的 `C.ADDI4SPN` imm=0、`C.JR rs1=0`、`C.JALR rs1=0` 等保留编码按 illegal instruction。
- **Zicsr**：`CSRRW/CSRRS/CSRRC` + immediate 形式。第一阶段全部走 helper（读/写 env 的 CSR 数组，处理只读/只写、`mstatus` 副作用、`fcsr` 的 `frm/fflags` 同步）。热 CSR（`cycle/time/instret/fcsr/vl/vtype/vstart`）后续内联。
- **Zifencei**：`FENCE.I` 必须 flush 当前 guest 地址空间的 TB（`tbc_invalidate_range`/`tbc_flush_asid`），并 `la_ibar(0)`；不能只发 NOP。
- **FENCE**：用 `la_dbar(0)` 作保守全屏障。基础实现忽略 `fm`（包括保留值）；`FENCE.TSO` 的较弱序同样由全屏障实现，正确但可能偏慢。
- **SFENCE.VMA**：清软件 TLB + `tbc_flush_asid`（按 `satp`/ASID）；如果使用硬件页表，需要新增 LISA `invtlb` 包装——当前 `la-append.h` 只有 `dbar/ibar`，没有 CSR/TLB/特权指令包装，P5 必须扩展 `la-append` 或走 C helper。

#### 4.6.1 可选 Zba/Zbb/Zbs

这些不是 P0 的必需项；如果 guest 声明了扩展，再打开。

| RISC-V | IR2 | 备注 |
| --- | --- | --- |
| `SH1ADD/SH2ADD/SH3ADD` | `la_alsl_w/d(rd, rs1, rs2, 1/2/3)` | `alsl` = `(rj << sa) + rk`；`sh1add` 用 sa=1，以此类推。 |
| `ADD.UW` | `la_bstrpick_d(t, rs1, 31, 0)` + `la_add_d(rd, t, rs2)` | 零扩展 32-bit 后 64-bit 加。 |
| `SLLI.UW` | `la_bstrpick_d(t, rs1, 31, 0)` + `la_slli_d(rd, t, shamt)` | shamt 0-63。 |
| `ANDN/ORN/XNOR` | `la_andn/la_orn/la_nor` | 直接映射。 |
| `CLZ/CTZ` | `la_clz_w/d`、`la_ctz_w/d` | 32-bit 版本结果符号扩展？按 RISC-V 语义核对。 |
| `CPOP` | helper / bit-twiddle | `la-append.h` 没有标量 `cpop`，只有 LSX `vpcnt`；不要为此强行用向量。 |
| `REV8` | `la_revb_d` | Zbb `rev8` 是字节反转；`bitrev.d` 是位反转，不要混。 |
| `ROL/ROR/RORI` | LBT `la_rotr_*`/`la_rotri_*`，或 `sll+srl+or` | 只有 Zbb 需要；无 LBT 时回退。 |
| `BCLR/BEXT/BSET/BINV` | `la_bstrpick_d`、`la_bstrins_d`、`la_andn/la_or/la_xor` | 立即数形式优先用 `bstrpick/bstrins`。 |
| `ORC.B` | helper / bit-twiddle | 没有单条 LA 对应。 |
| `CLMUL/CLMULH/CLMULR` | helper | 没有 LA carry-less multiply；Zbc 不是必需。 |

### 4.7 系统态

第一阶段不做系统态；P4 再做。最小状态：

```c
typedef struct CPUArchState {
    uint64_t gpr[32];
    uint64_t pc;
    uint64_t fpr[32];        /* raw 64-bit；F 值 NaN-boxed */
    uint32_t fcsr;           /* fflags[4:0] | frm[7:5] */
    uint32_t priv;           /* M/S/U */
    uint64_t mstatus, sstatus;
    uint64_t mie, mip, mideleg, medeleg;
    uint64_t mtvec, mepc, mcause, mtval;
    uint64_t stvec, sepc, scause, stval, sscratch;
    uint64_t satp;
    uint64_t pmpcfg[16], pmpaddr[64];
    uint64_t mhartid, misa, mvendorid, marchid, mimpid;
    uint64_t load_res, load_val;   /* LR/SC helper */
    /* RVV */
    uint32_t vtype, vl, vstart, vcsr, vxrm, vxsat;
    uint8_t  vreg[32][LATR_VLENB]; /* VLEN=128 -> 16 B/reg, 512 B total */
    uint64_t vlenb;                /* read-only CSR 0xc22 */
    /* 异常/中断/定时器/设备状态 */
} CPUArchState;
```

**FS/VS dirty 跟踪**：RISC-V 的 `mstatus.FS/VS` 表示 FP/vector 状态是否 Dirty。LATR 必须在写 FPR/vector 寄存器时置 Dirty（QEMU 的 `mark_fs_dirty()`/`mark_vs_dirty()` 是参考），并在 TB 出口/上下文切换时按 Dirty 位保存 FPR/vector。不能只在 prologue/epilogue 无条件保存全部状态；那会把 `mstatus.FS/VS` 的语义做错，也会拖慢不用的 TB。

系统态顺序：

1. 异常/中断入口：`mcause/mepc/mtval/mstatus`，`mret/sret`，`medeleg/mideleg`。
2. Sv39 walker：`src/riscv_paging.c`，用 `arm64_paging.c` 的 callback 结构；先 Sv39，再 Sv48/Sv57。必须处理 canonical VA 检查（Sv39 的 VA[63:39] 必须等于 VA[38]）、leaf PTE 的 A/D 位（未设置时按硬件行为置位或 fault）、U/R/W/X 权限、`mstatus.MXR/SUM`、`MPRV`，以及 superpage 对齐；QEMU `riscv_cpu_tlb_fill()` 是行为参考。
3. PMP：物理地址检查；先做 NAPOT/TOR 的基本形式。
4. 设备：CLINT（mtime/mtimecmp）、PLIC、16550 UART；QEMU `virt` 的内存映射见 `hw/riscv/virt.c:75-82`（CLINT `0x02000000`、PLIC `0x0c000000`、UART0 `0x10000000`、virtio `0x10001000`）。
5. 启动：先跑 flat S-mode payload，再跑 OpenSBI（M-mode），最后尝试 Linux。
6. `misa` 必须只声明实际实现的扩展；不要为了跑 Linux 虚报 V/F/D。

**IR2/LISA 缺口**：当前 `la-append.h` 是用户态子集，没有 `csrrd/csrwr/csrxchg`、`invtlb/tlbsrch/tlbrd/tlbwr`、`idle` 等特权指令的包装（`la-append.h` 只有 `dbar/ibar` 等）。而且 `latx/inst_template.json` 里也**没有**这些特权指令（`amcas` 有，`csrrd/invtlb/idle` 没有；本轮已 grep 确认），所以不能只靠“重新生成 la-append”得到它们。P5 有两条路：

- 扩展 `latx/inst_template.json` + `latx/convert.py`，加入特权指令编码，再生成包含它们的 `la-append`（推荐，保持 IR2 统一；代价是要维护编码表）；
- 或者为 CSR/TLB 写 C helper，用内联汇编执行（快，但绕开 IR2 的寄存器分配/汇编器，后续优化困难）。

无论哪条，`tr_ir2_assemble()` 与 `format-table.h` 都要支持新增 opcode；不要用 `.word` 手工拼机器码绕过 IR2。

## 5. RVV 与 PVIR

### 5.1 RVV 状态模型

RVV 1.0 的核心状态：

- `vtype`：`vlmul[2:0]`（LMUL=1/2/4/8，或 1/8、1/4、1/2）、`vsew[5:3]`（SEW=8/16/32/64）、`vta[6]`、`vma[7]`、`vill[XLEN-1]`。
- `vl`：当前活动元素数；`vl <= VLMAX = VLEN*LMUL/SEW`。
- `vstart`：向量指令的起始元素索引；trap 后由硬件写入。
- `v0`：mask 寄存器，每元素 1 bit（MLEN=1），不是每元素一个字节。
- `v1-v31`：数据寄存器；LMUL>1 时按寄存器组使用。
- `vcsr`：`vxrm`（定点舍入）在 CSR `0x00a`，`vxsat`（饱和标志）是 `vcsr`（CSR `0x00f`）的 bit 0。
- `vl/vtype/vlenb` 只读（CSR `0xc20/0xc21/0xc22`），`vstart` 可写（CSR `0x008`）。

QEMU-LVZ 的实现是 golden：`vsetvl` helper 计算 `vlmax/vl/vill` 并清 `vstart`（`vector_helper.c:34-84`）；`GEN_VEXT_V` 从 `env->vstart` 循环到 `vl`，masked-off 元素按 `vma` 写 1s 或保留，尾部按 `vta` 写 1s 或保留（`vector_internals.h:150-174`）。QEMU 的默认 `rvv_ta_all_1s=false`、`rvv_ma_all_1s=false`（`cpu.c:2678-2679`），即 **agnostic 默认实现为“保持原值”**，这是合法的 RVV 行为。

### 5.2 PVIR 现有能力

PVIR 的核心抽象（`la-pvir.h`）：

- `PvirPredicate`：`ALL_TRUE/ALL_FALSE/PREFIX/SUFFIX/BITSET/VECTOR_MASK`，带 `source`（kind/id/version）和 `lanes`（`:71-80`）。
- `PvirInactivePolicy`：`NOT_APPLICABLE/MERGE/ZERO/UNKNOWN`（`:131-137`）。
- `PvirVectorOperation`：单一 `PvirVectorType`、predicate、inactive policy、destination、最多 4 个 operand、merge value（`:151-162`）。
- `PvirEffectMetadata`：memory access/ordering/inactive-lane effect、fault behavior/order/result/unknown_from_lane、side effects、FP exception effect、arch state（`:246-292`）。
- `PvirInstruction`：`opcode` + effects + union（`:295-302`）。
- `pvir_predicate_expand()`：把 predicate 展开为每 lane 全 0/全 1 的 `uint64_t`（`pvir.c:273-330`）。
- `pvir_expanded_mask_cache_get()`：按 source kind/id/version、form、active_lanes、element_bits、lanes 缓存展开结果（`pvir.c:736-810`）。

这些抽象与 RVV 的 mask/tail/fault/restart 概念高度同构，**方向正确**。问题在于字段形状是 SVE 的。

### 5.3 PVIR ↔ RVV 映射表

| RVV 概念 | PVIR 字段 | 现状 | 需要的动作 |
| --- | --- | --- | --- |
| `v0` mask | `PVIR_PREDICATE_VECTOR_MASK` + source version | ✅ 可表达 | 每次写 v0 递增 version；mask cache 失效。 |
| `vl` tail | `predicate.lanes` | ⚠️ 静态 `size_t` | TB 特化 `vl`，或扩展动态 lane 数。 |
| masked-off lanes | `inactive_policy` | ✅ `MERGE/ZERO/UNKNOWN` | 见 §5.4 策略统一。 |
| tail lanes | 同一个 `inactive_policy` | ⚠️ 不能区分 vta/vma | 先统一为 undisturbed；要精确建模则加 `tail_policy`。 |
| `vta/vma` | `inactive_policy` | ✅ 可映射 | agnostic 选 undisturbed 合法；QEMU 默认也是保持。 |
| masked-off load 结果 | `PVIR_OP_LOAD` 的 inactive policy | ✅ 已允许 `ZERO/MERGE/UNKNOWN`（本轮 P0） | RVV 用 `MERGE`（vma=0，旧 vd）或 `UNKNOWN`（vma=1）；不要用 `ZERO`。 |
| `vstart` | `PVIR_ARCH_STATE_DOMAIN_RESTART` | ⚠️ 只有状态，没有 lane 起点 | 加 `first_lane`，或非零 vstart 走 helper。 |
| fault-only-first | `PVIR_FAULT_NONFAULTING` + `unknown_from_lane` | ✅ 可表达（不交付 fault；`unknown_from_lane` 是失败边界）；arch_state 用 `RESTART/OTHER` 更新 `vl` | 注意 `NONFAULTING` 要求 `lane_order=UNSPECIFIED`、`fault_result=UNKNOWN`；`FIRST_FAULT` 是 SVE FFR 的语义，不要套到 RVV。 |
| `vl` 更新 | `PvirArchStateOperation` | ⚠️ 只有 `clear_from_lane` | 用 `output_value` 表示新 `vl`，或加专用字段。 |
| widening/narrowing | 单一 `PvirVectorType` | ❌ | 每 operand 一个 `PvirVectorType`/EEW。 |
| `.vx/.vi` 标量操作数 | `PvirValueId` | ❌ 无类型 | 加 `PvirOperandKind`（vector/scalar/imm）。 |
| 比较条件（EQ/NE/LT/LE/…） | `PVIR_OP_COMPARE` 无字段 | ❌ | 加 `PvirCompareCondition`，或把条件作为 immediate operand。 |
| LMUL 寄存器组 | `vector_bits` | ⚠️ 只能扁平表示 | 前端把组映射为 chunk；PVIR 不建模 overlap。 |
| mask 目的（`vd=v0`） | `PVIR_OP_COMPARE` 的 destination | ⚠️ PVIR 无 1-bit element 类型 | COMPARE 先产生 element-width 全 1/0，再 pack 成 v0；或 bypass 到 helper。 |
| reductions/permutes/mask popcount | 无 opcode | ❌ | 绕过 PVIR（helper/IR2），或扩展 opcode。 |
| FP rounding/NaN | 无字段 | ❌ | helper 或 frontend metadata。 |
| memory order/fault | `PvirMemoryEffect`/`PvirFaultEffect` | ✅ | 直接使用。 |
| merge/zero | `PVIR_INACTIVE_MERGE/ZERO` | ✅ | `merge_value` = 旧 vd。 |

### 5.4 策略统一：把 vta/vma 都实现为 “undisturbed”

RVV 的 `vta/vma=1` 是 **agnostic**：允许保留原值，也允许写全 1。规范不要求写全 1。因此一个合法的实现可以对所有 inactive lanes **保持目的寄存器原值**：

- 对 `vta=0`（undisturbed）：必须保持。
- 对 `vta=1`（agnostic）：选择保持，合法。
- 对 `vma=0`：必须保持。
- 对 `vma=1`（agnostic）：选择保持，合法。

QEMU-LVZ 的默认 `rvv_ta_all_1s=false`/`rvv_ma_all_1s=false` 正是这个选择（`cpu.c:2678-2679`）。

**RVV 的 agnostic 不是 zero**：规范只允许“保持原值”或“写全 1”，不允许写 0。因此 LATR 对 RVV 只能使用 `MERGE`（undisturbed）或 `UNKNOWN`（agnostic，lowerer 可选保持或全 1），**不能使用 `PVIR_INACTIVE_ZERO`**；`ZERO` 保留给 SVE 的 zeroing load。这个区别必须在 bridge 的断言里锁死。

于是：

1. **tail 和 masked-off 可以共用同一个 `PVIR_INACTIVE_MERGE`**，`merge_value = 旧 vd`。
2. 只要 `vl` 是 TB 常量，`lanes = vl`，tail 根本不进入 PVIR 的操作范围；尾部的“保持”由“不写”自然满足。
3. 对 `vm=1`（无 mask）的指令，predicate = `ALL_TRUE(lanes=vl)`，仍然保持尾部。注意 `pvir_instruction_validate()` 对非 STORE 不接受 `PVIR_INACTIVE_NOT_APPLICABLE`；切片内全部 active 时用 `MERGE`（`merge_value=旧 vd`）或 `ZERO`，语义等价。
4. 对 `vm=0`，predicate = `VECTOR_MASK(source=v0, lanes=vl)`；masked-off lanes 用 `MERGE` 保持。
5. 只有“需要写全 1”的 guest 代码才会与 QEMU 默认不同；这不是规范要求，差分测试可以选择 QEMU 默认作为 golden。

这个统一**显著缩小了 PVIR 的扩展面**：不需要为每条 RVV 指令区分 tail policy 和 mask policy。代价是：如果未来要模拟 `rvv_ta_all_1s=true` 的宿主配置，需要再加一个 policy 字段；可以在 PVIR 里加 `tail_policy`/`mask_policy` 作为可选扩展，不破坏现有 SVE 调用者。

### 5.5 TB 特化与 `vl/vtype/vstart`

RVV 的 `vl` 是运行时 CSR，`vsetvl` 会改变它。QEMU 的处理是：`vsetvl` helper 更新 `env`，翻译器结束 TB（`trans_rvv.c.inc:162-207`）；但 QEMU 的 helper 是运行时循环，不依赖编译期 `vl`。LATR 若要把 PVIR 的 `lanes` 当常量用，需要：

1. `vsetvl/vsetvli/vsetivli` 结束 TB，更新 `env->vl/vtype/vstart`。
2. TB key 包含 `vl/vtype/vstart`（§3.5）。
3. 新 TB 翻译时，`DisasContext.vl = env->vl`，`PVIR.lanes = vl`。
4. `vstart != 0` 的 TB 走 helper；或给 PVIR 加 `first_lane`，direct lowering 从该 lane 开始。
5. `vtype.vill=1` 时，所有向量指令按 illegal instruction；`vl=0` 时，向量指令不写任何元素（但仍要检查 vstart/CSR 副作用）。注意 PVIR 的 `lanes` 必须 >0（`pvir_predicate_validate()` 拒绝 0），所以 `vl=0` 时不要构造 PVIR 指令，直接跳过向量操作。

代价是 TB 数量可能增加：不同 `vl/vtype` 会生成不同 TB。RVV 代码通常在循环外 `vsetvl` 一次，循环内复用，所以可接受；需要基准验证。如果 TB 爆炸严重，退化为“helper 为主 + direct lowering 只用于 `vl=VLMAX`”的策略。

**`vstart` 的清除**：向量指令成功完成后必须把 `vstart` 清 0；trap 时硬件把 `vstart` 设为出错元素。QEMU 的 `GEN_VEXT_V` 在循环后写 `env->vstart=0`，`VSTART_CHECK_EARLY_EXIT` 在 `vstart>=vl` 时也清 0（`vector_internals.h:27-31,150-174`）。LATR 的 helper 和 direct lowering 都要维护这个不变量；不要只在 TB 出口清。

**`vl` 的其他写入者**：除了 `vsetvl*`，fault-only-first load（`vle*.ff.v`）也会把 `vl` 改成成功元素数（QEMU `vext_ldff()` 在 `vector_helper.c:472-546` 里写 `env->vl`）。如果 TB 特化了 `vl`，`vleff` 必须结束 TB（或跳到按新 `vl` 重新翻译的入口），否则后续指令还在用旧的 `lanes`。这是 TB 特化带来的额外约束，不能漏。

### 5.6 掩码展开与 cache

RVV 的 v0 是 packed bits，LSX 需要每元素全 0/全 1 的 mask 向量。`pvir_predicate_expand()` 正好产生 `uint64_t` per lane；`pvir_expanded_mask_cache_get()` 按 v0 version/SEW/lanes 缓存。落地方式：

1. 翻译期：`pvir_expanded_mask_cache_get(&cache, &pred, SEW, v0_words, v0_word_count, &expanded)`。
2. 生成 IR2：把 `expanded[0..lanes-1]` 打包成 LSX 常量（如果 v0 是编译期常量）或运行时展开序列（如果 v0 动态）。
3. 运行时展开：`v0` → 字节 mask 的快速序列。VLEN=128、SEW=8 时有 16 个 lane；可以用 256 字节查表 + `vshuf.b`，或逐 bit `vset`/`vor.v`。第一版可以调用 helper `latr_expand_v0(env, vtype)` 返回一个 16/32 字节 mask，后续再内联。
4. cache 失效：写 v0、写 `vtype/vl`、`vsetvl`、TB 退出/异常返回时，`pvir_expanded_mask_cache_invalidate_source`/`invalidate_all`。任何 helper 调用都保守地 `invalidate_all()`，除非 helper 明确声明不写 v0；不要假设 helper 只写目标寄存器。
5. `PVIR_PREDICATE_PREFIX` 可用于 `vl` 的 tail：如果不用 TB 特化，`pvir_predicate_prefix(source=vl_version, lanes=VLMAX, active_lanes=vl)`；但 `active_lanes` 是静态 `size_t`，所以仍需要 TB 特化或运行时 mask。

### 5.7 PVIR 的缺口与最小扩展

按“是否影响 SVE 现有语义”分为两类。

**A. 语义必需（建议改 `la-pvir.h`/`pvir.c`，additive）**

1. **LOAD 的 inactive policy — 已完成（P0）**：`pvir.c` 已移除“LOAD 必须 `ZERO`”的检查，现在允许 `ZERO/MERGE/UNKNOWN`；`make test-pvir` 从 114 增到 118 checks。RVV 仍**不能**用 `ZERO`（agnostic 只能保持或全 1），LATR 用 `MERGE`（vma=0）或 `UNKNOWN`（vma=1）；`ZERO` 保留给 SVE。bridge 里应有断言禁止 RVV 走 `ZERO`。
2. **每 operand 的元素宽度**：`PvirVectorOperation` 加 `PvirVectorType operand_types[PVIR_MAX_OPERANDS]` 或 `uint16_t element_bits[PVIR_MAX_OPERANDS]`；用于 widening/narrowing（`vwadd`、`vnsrl`、`vzext/vsext`）。保留 `type` 作为目的类型，旧调用者不受影响。
3. **operand 类型**：加 `PvirOperandKind { VECTOR, SCALAR, IMMEDIATE }`；用于 `.vx/.vi`。`PvirValueId` 本身不区分。
4. **比较条件**：`PVIR_OP_COMPARE` 需要 `PvirCompareCondition { EQ, NE, LT, LE, GT, GE, LTU, LEU, GTU, GEU }`（或等价的 enum）；否则 `vmseq/vmslt/vmsle/vmsgt/...` 无法区分。`SELECT` 的条件由 predicate 给出，不需要额外字段。
5. **`first_lane`**：建议加在 `PvirPredicate`，而不是 `PvirVectorOperation`。原因：`VECTOR_MASK` 展开必须按 `first_lane + lane` 去取 v0 的位；如果 `first_lane` 只在 operation 上，`pvir_expanded_mask_cache_get()` 看不到它，`vstart>0` 时会把 bit 0 当成 slice 起点，缓存还会把不同 `vstart` 的展开结果串用。正确做法是：
   - `PvirPredicate` 加 `size_t first_lane`（默认 0）；
   - `pvir_predicate_validate()` 检查 `first_lane + lanes` 不溢出；
   - `pvir_predicate_expand()` 对 `VECTOR_MASK` 取 `vector_mask_words[(first_lane + lane)/64]` 的对应位，容量检查按 `first_lane + lanes` 算；
   - `PvirExpandedMaskCacheEntry` 的 key 加 `first_lane`；
   - `PvirVectorOperation` 的 destination/source 偏移由 `predicate.first_lane` 提供。
   如果第一阶段不走 direct vstart（非零 vstart 全部 helper），这条可以延后；但不要只加在 operation 上，否则缓存语义是错的。
6. **fault-only-first 的语义选择**：RVV 的 `vleff` **不交付 fault**，失败时把 `vl` 截断到失败元素；这对应 `PVIR_FAULT_NONFAULTING`，不是 `PVIR_FAULT_FIRST_FAULT`（后者是 SVE FFR 的累计成功语义，校验强制 `FIRST_FAULT` domain + `CUMULATIVE_SUCCESS`）。`NONFAULTING` 已经允许 `arch_state.domain = RESTART/OTHER`、`action = UPDATE`、`update_kind = NONE`、`input/output` 为旧/新 `vl`（`pvir.c:368-410,447-482`），所以这条**不需要改核心**；只需在 LATR 的 bridge 里正确选择 `NONFAULTING` 并给出 `unknown_from_lane`。如果同一个 `vleff` 还要写 `vstart=0`，用一个额外的 `PVIR_OP_ARCH_STATE` 或 lowering 不变量处理。
7. **动态 lane 数（可选）**：如果不用 TB 特化，`PvirPredicate.active_lanes` 需要支持 value ID；这是较大的改动，建议先用 TB 特化规避。

**B. 覆盖率扩展（建议放 LATR，不污染 ISA-independent core）**

- **mask 目的**：RVV 的 `vd=v0` 写的是 packed 1-bit mask，PVIR 的 `element_bits` 只接受 8/16/32/64。`vmseq/vmslt/... v0` 需要先产生 element-width 的 all-ones/zero，再用 `vmskltz`/`vpack` 或 helper 打包；不要试图把 1-bit 塞进 `PvirVectorType`。
- `MIN/MAX`、`SATURATING_ADD/SUB`、`AVERAGE`、`REDUCE`、`PERMUTE/GATHER`、`MASK_POPCOUNT/FIRST`、`FP_CONVERT` 等，不要全部加进 `PvirOpcode`；在 `translate-riscv.c` 里：
  - 能用 PVIR 组合表达的（如 `min/max` = `COMPARE` + `SELECT`）先组合；
  - 不能表达的（如 `vrgather`、`vcompress`、`vredsum`）直接发 IR2/helper，PVIR 只负责 predication/fault 外层。
  - 如果某类操作在 SVE 和 RVV 都需要（如 `MIN/MAX`、`REDUCE`），再提升到 `PvirOpcode`。

这样 PVIR 保持“predicated elementwise + fault/restart”的核心，不被 RVV 的全部 362 条 decode 条目撑爆。

### 5.8 降级策略：helper 还是 LSX/LASX

| RVV 类别 | P1 策略 | P2/P3 direct 策略 |
| --- | --- | --- |
| `vsetvl*` | helper 更新 vtype/vl/vstart，结束 TB | 同左；vtype/vl 进入 TB key。 |
| 单位步长 load/store（全 active） | helper | `vld/vst`（VLEN=128 一条 LSX）；LMUL>1 按 chunk。 |
| 单位步长 load/store（masked/partial） | helper | 逐元素 `vldrepl`/`vstelm` 或 mask 序列；页边界必须精确。 |
| strided/indexed/segment/fault-only-first | helper | 先不做 direct；helper 是正确性基线。 |
| 整数 elementwise（add/sub/and/or/xor/shift/compare/min/max） | helper | LSX `vadd/vsub/vand/...` + `vbitsel.v`（mask/merge）。 |
| widening/narrowing/saturating/average | helper | LSX `vaddw/vaddwev/vpack/vssrln/...`，逐类对拍。 |
| FP elementwise | helper | LSX `vfadd/vfsub/...`，NaN/fflags 对拍后再开。 |
| reduction | helper | 水平 `vadd`/`vdp` 序列；或继续 helper。 |
| permute/gather/compress | helper | `vshuf.b` 处理 128-bit 内；跨 chunk 继续 helper。 |
| mask 逻辑/popcount/first | helper | `vand.v/vpcnt/vmskltz` + 水平归约。 |
| whole-register move | 直接 `vld/vst` 或 `vor.v` | 同左。 |

**对齐注意**：`la_vld/la_vst` 是 16-byte 对齐的 LSX 访存，`la_xvld/la_xvst` 是 32-byte 对齐的 LASX 访存；RVV 的 unit-stride 访问只保证元素对齐，地址不一定是 16/32-byte 对齐。`vld/vst`/`xvld/xvst` 只能用于“地址对齐且元素连续”的快路径；`vldx` 只是把基址放在寄存器里，仍有同样的对齐要求，不能用来修非对齐。非对齐/跨页的情况用逐元素 `vldrepl`/`vstelm` 或 helper。不要用 `vld/xvld` 覆盖非对齐地址，否则会在宿主上产生 guest 不应看到的 alignment fault。

**LSX/LASX 特性门控**：`latx_cpu_has_lsx()/lasx()` 读 CPUCFG.2 bit 6/7（`latx/runtime/cpu_features.c:44-58`）。LATA 的 bare build 用 `-mno-lsx -mno-lasx` 只影响 C 编译器自动向量化，**不影响** `la-append` 发射 LSX/LASX 机器码；但宿主 CPU/QEMU 必须真的开启 LSX/LASX（QEMU 用 `-cpu la464,lsx=on,lasx=on`）。LATR 必须：

- 无 LSX：所有向量指令走 helper；不要发出任何 LSX 编码。
- 有 LSX、无 LASX：VLEN=128；LMUL=1 一条 LSX，LMUL>=2 用多条 LSX（2/4/8 条）。
- 有 LASX：可选 VLEN=128（LMUL=2 一条 LASX，LMUL=4 两条，LMUL=8 四条）或 VLEN=256（LMUL=1 一条 LASX，LMUL=2 两条）。
- 不向 guest 宣称 `VLEN` 超过实际 lowerer 能处理的宽度；`vlenb` 必须与 `vtype/vl` 一致。

### 5.9 LMUL、寄存器组与 overlap

PVIR 只表达“一个扁平向量值 + lanes”，不表达 RVV 的寄存器组和 overlap 规则。LATR 前端必须：

1. **PVIR 描述的是活动切片，不是整个寄存器组**：`vector_bits = vl * SEW`、`lanes = vl`。这样 `predicate.lanes == vector_bits / element_bits` 自动成立（`pvir.c:531-535`）。架构组大小 `VLEN*LMUL` 只用于 chunk 数量、地址和 tail 计算；tail `[vl, VLMAX)` 不进入 PVIR，保持原值（undisturbed）。
2. `vl == VLMAX` 时切片就是整组；`vl < VLMAX` 时切片更短，lowering 只写 `vl` 个 lane。对 `LMUL < 1`，切片不超过 `VLEN*LMUL`（例如 VLEN=128、LMUL=1/8、SEW=8、vl=2 → 16 bit），物理上只使用宿主寄存器低 `VLEN*LMUL` 位；**不能直接 `vld/vst` 16 bytes**，那会读/写相邻寄存器，用 `vldrepl`/`vstelm`/mask 或 helper。不要用 `vector_bits = VLEN*LMUL` 配 `lanes = vl`（除非 vl=VLMAX），否则 PVIR 校验会因 `predicate.lanes != vector_bits/element_bits` 失败。
3. 在 IR1 解码时执行 QEMU 的 `require_align()`/`require_noover()` 检查（`trans_rvv.c.inc:100-160`）；非法编码按 illegal instruction。
4. 目的组与源组重叠时，先读源到临时 chunk，再写目的；不要假设 LSX 指令的读/写顺序。
5. `v0` 作为 mask 不能与目的组重叠（`require_vm`）；IR1 检查。

### 5.10 F/V 寄存器文件冲突（必须在 P1 前定案）

RISC-V 的 `f0-f31` 与 `v0-v31` 是**独立**寄存器文件；LoongArch 的标量 FPR 和 LSX/LASX 共用同一个 32 项、每项 256-bit 的物理文件。LATA 的 A64 映射能 1:1，是因为 A64 的 V0-V31 与 D0-D31 架构上就别名。RISC-V 不能照搬。

可选方案：

| 方案 | 标量 FPR | RVV 寄存器 | 优点 | 缺点 |
| --- | --- | --- | --- | --- |
| A | 1:1 映射到 LA FPR（可预留 f28-f31） | env 内存，LSX 作 chunk temp | 标量 FP 快；实现简单 | 向量每条指令 load/store chunk；预留 temp 会让少量 guest FPR 走 env |
| B | env 内存 | 1:1 映射到 LA 向量寄存器 | 向量快 | 标量 FP 每条 load/store；与 F/D 基线冲突 |
| C | 部分 1:1（如 f0-f23） | 部分 1:1（v24-v31 作 temp） | 折中 | 映射复杂；spill 规则多 |
| D | 按 profile 二选一（RV64IMACV 无 F/D → B；RV64GCV → A） | 同左 | 每种 profile 最优 | 两套 lowering/测试 |

建议：**第一阶段选 A 的变体**：标量 FPR f0-f27 直映，f28-f31 作为 RVV chunk temp（guest f28-f31 走 env），RVV 寄存器全部放 env。这样标量 FP 大多数指令仍是单条 LA 指令，向量 direct lowering 有 4 个固定 temp，不需要在每条向量指令前后 spill。等 direct lowering 稳定后，再评估 C/D。方案 A 的每条向量指令成本是“load 2-3 个 16B chunk + 计算 + store”，对 VLEN=128 仍比 QEMU 的逐元素 C helper 快。

注意：LATA 的 `reg_ftemp_map` 已占用 f9-f14（`target/arm/lata/translator/reg-alloc.c`）；LATR 不要直接复用这组 ftemp 作为 vector temp，否则与标量 FPR 映射冲突。要么把 `reg_ftemp_map` 改到 f28-f31，要么在 LATR 建独立的 `latr_vec_temp[]`。

### 5.11 两个具体例子

#### 例 1：`vadd.vv v4, v2, v1, v0.t`（masked，VLEN=128，SEW=32，LMUL=1，vl=4）

IR1 记录：`type=RISCV_VADD_VV`，`arg={vd=4, vs2=2, vs1=1, vm=0}`。TB 已特化 `vl=4/vtype=(SEW=32,LMUL=1,vta=0,vma=0)`。

PVIR 构造：

```c
PvirInstruction insn = {0};
insn.opcode = PVIR_OP_ADD;
insn.operation.vector.type = (PvirVectorType){
    .kind = PVIR_ELEMENT_INTEGER, .signedness = PVIR_UNSIGNED,
    .element_bits = 32, .vector_bits = 128,       /* VLEN=128 */
};
insn.operation.vector.predicate = pvir_predicate_vector_mask(
    (PvirPredicateSource){ PVIR_PREDICATE_SOURCE_VECTOR, 0, v0_version },
    4);                                           /* lanes = vl */
insn.operation.vector.inactive_policy = PVIR_INACTIVE_MERGE;
insn.operation.vector.merge_value = value_id(v4, old_version);
insn.operation.vector.destination = value_id(v4, new_version);
insn.operation.vector.operands[0] = value_id(v2, ver2);
insn.operation.vector.operands[1] = value_id(v1, ver1);
insn.operation.vector.operand_count = 2;
insn.effects.memory.access = PVIR_MEMORY_NONE;
insn.effects.fault.behavior = PVIR_FAULT_NONE;
insn.effects.fault.unknown_from_lane = PVIR_NO_VALUE; /* FAULT_NONE 要求 */
```

Lowering（direct，方案 A）：

```text
f28 = vld(env + off(v2), 0)        // 128-bit chunk
f29 = vld(env + off(v1), 0)
f28 = vadd.w(f28, f29)             // 4 x 32-bit，复用 f28
f30 = build_mask(v0, SEW=32, vl=4) // 4 lanes -> 0xffffffff / 0
f31 = vld(env + off(v4), 0)        // old vd
f28 = vbitsel.v(f28, f31, f30)     // active -> f28, inactive -> old
vst(f28, env + off(v4), 0)
```

如果 `vm=1` 且 `vl=VLMAX=4`，跳过 mask 和 `vbitsel.v`，直接 `vst(f28)`。这里用 f28-f31 四个 temp，与 §5.10 的“预留 f28-f31”一致；如果 LMUL>1 需要同时保留更多 chunk，则按 chunk 逐个处理，不要一次把整个组装进 temp。

#### 例 2：`vle32.v v4, (a0), v0.t`（masked load）

PVIR 构造：

```c
insn.opcode = PVIR_OP_LOAD;
insn.effects.memory.access = PVIR_MEMORY_READ;
insn.effects.memory.ordering = PVIR_MEMORY_ORDER_RELAXED;
insn.effects.memory.inactive_lanes = PVIR_INACTIVE_MEMORY_NO_TRANSACTION;
insn.effects.fault.behavior = PVIR_FAULT_PRECISE;
insn.effects.fault.lane_order = PVIR_FAULT_ORDER_LANE_ASCENDING;
insn.effects.fault.unknown_from_lane = PVIR_NO_VALUE; /* PRECISE 要求 */
insn.operation.vector.predicate = pvir_predicate_vector_mask(v0_source, 4);
insn.operation.vector.destination = value_id(v4, new_version);
insn.operation.vector.operands[0] = value_id(address_base, addr_version);
insn.operation.vector.operand_count = 1;              /* PVIR_OP_LOAD arity = 1 */
insn.operation.vector.inactive_policy = PVIR_INACTIVE_MERGE; /* P0 起已合法；vma=0 的 undisturbed */
insn.operation.vector.merge_value = value_id(v4, old_version);
```

**P0 已解锁这个例子**：`pvir_instruction_validate()` 现在允许 LOAD 的 `ZERO/MERGE/UNKNOWN`（`target/arm/lata/pvir.c`；`make test-pvir` 118 checks）。RVV 的 vma=0 用 `MERGE`、vma=1 用 `UNKNOWN`；**不要用 `ZERO`**，RVV 的 agnostic 只能保持或全 1。对 masked-off lane 的访存语义仍由 `PVIR_INACTIVE_MEMORY_NO_TRANSACTION` 保证：inactive lane 不产生物理事务，也不会 fault。

如果 P0 没做，之前的 workaround 只有“走 helper”或“先复制旧 vd、发 ZERO load、再合回去”；两者都比直接允许 `MERGE` 差。现在不需要了。

## 6. LBT 能否加速

| LBT 指令/组 | RISC-V 用途 | 结论 |
| --- | --- | --- |
| `x86add/x86sub/x86adc/x86sbc/x86and/x86or/x86xor/x86shift/x86mul` | 无 | **不用**。RISC-V 没有 EFLAGS。 |
| `x86mfflag/x86mtflag/x86settag/x86mftop/...` | 无 | **不用**。 |
| `armadd/armsub/armadc/armsbc/armand/.../setarmj` | 无 | **不用**。RISC-V 没有 NZCV。 |
| `adc/sbc/rcr/rcri` | 无 | **不用**。没有进位标志。 |
| `rotr/rotri` | Zbb `rol/ror/rori` | **可选加速**。1 条 vs `sll+srl+or` 3 条；无 LBT 回退。 |
| `jiscr0/jiscr1` | TB 链接/间接跳转 | **边际**。`jiscr0` 需要先 `movgr2scr`，与 `jirl` 同为 1-2 条；只有把目标常驻 SCR0 时才省一条。`jiscr1` 的指南/QEMU 语义有分歧（指南说 SCR0=PC、SCR1+imm；QEMU `helper_lbt_jiscr()` 两个 jiscr 都用 SCR0 作目标），**不要依赖**。 |
| `movgr2scr/movscr2gr` | 保存 TB 指针/guest PC | **可选**。SCR 是宿主 LBT 状态，TB/异常切换时要保存；收益不如 LATA 的 `pc_map_cache` 直接跳转。 |
| `ldl/ldr/stl/str` | 非对齐 load/store | **不建议**。RISC-V 非对齐访问可以 trap；LBT 的 MIPS 语义是字节合并，**跨页 fault 的精确位置**与 RISC-V 不同。非对齐走 helper，按 RISC-V 规则逐字节/逐半字处理。 |
| `addu12i.w/.d` | 地址合成 | **不用**。`auipc` 是 20-bit 移位，`pcaddu12i` 已在 base LA；`addu12i` 只有 5-bit×12，不匹配。 |

结论：**LATR 的核心 lowering 不应依赖 LBT**。`rotr/rotri` 作为 Zbb 的可选特化，`jiscr0` 作为 TB 链接的可选实验；两者都必须在 CPUCFG 门控下，且有无 LBT 回退。`src/loongbt*` 的 portable 实现已有 `ROTR/ROTRI`，但没有 `JISCR`（以及 LBT MIPS memory）操作；如果要走 LBT，需要扩展 `LoongbtOp` 或直接在 LATR 发射 LISA 指令。

LBT 可用性的具体位：QEMU-LVZ 把 `CPUCFG2.LBT_X86/LBT_ARM/LBT_MIPS` 放在 bit 18/19/20（`target/loongarch/cpu.h:170-173`），`avail_LBT_ANY()` 是三者之或（`translate.h:32`）。QEMU 的 `lbt_rotr_*`/`lbt_rotri_*` 用 `avail_LBT_ANY()` 门控（`trans_lbt.c.inc:583-610`），不是单独的 “LBT integer” 位。LATR 如果要发射 `rotr/rotri`，必须读这三位并确认宿主真的实现了整数 rotate；不要只看 “LBT 存在” 就发。

## 7. LVZ 能否加速

LVZ 是 LoongArch 的虚拟化扩展（QEMU-LVZ README；`cpu-csr.h:215-260`）：

- guest/host 两套 CSR、TLB、定时器；
- `GSTAT`、`GCSR`、`GTLB`、`GINTCTL`；
- GVA→GPA 在 guest TLB，GPA→HPA 在 host TLB；
- guest 模式的 `HVCL/GSPR/MMIO/CPUCFG/IDLE` 例外回到 host。

**它不能加速 RISC-V 翻译的原因**：

1. LVZ 只改变 LoongArch 的地址翻译和特权状态；RISC-V 指令仍需 LATR 解码/翻译/执行。
2. RISC-V 的 guest 页表是 Sv39/Sv48/Sv57 格式；LVZ 的 G-stage 只做 GPA→HPA，不会解析 RISC-V PTE。guest VA→GPA 仍要软件 walker 或阴影页表。
3. RVV 的向量运算不会因为 LVZ 变快；LASX/LSX 才是向量加速。
4. 在 QEMU-LVZ 上，LVZ 本身是模拟的，加入它只会增加例外/上下文切换开销。

**LVZ 唯一可能有用的地方**：

- **隔离/安全**：把 BT 运行时放在 LoongArch guest 中，guest 崩溃不拖垮 host；这是安全性，不是性能。
- **TLB/ASID 失效模型**：LVZ 的 GID/ASID 范围 `invtlb`（`tlb_helper.c:721-966`）可以作为 `tbc_flush_asid`/`sfence.vma` 的参考；普通 LoongArch ASID 也能做，不需要 LVZ。
- **嵌套虚拟化测试床**：如果未来要在 LoongArch 上跑“RISC-V BT + 设备模型”的多租户，LVZ 可提供 guest 隔离；但这不是当前单 guest 翻译的目标。

建议：**LVZ 不进入关键路径**；`baremetal/efi/lvz_probe` 保留为能力探测。真正的加速优先级是：直译（减少 helper）、寄存器分配、TB 链接、LSX/LASX 向量化。

## 8. Golden reference 与差分测试

### 8.1 构建 QEMU-LVZ 的 RISC-V 目标

RISC-V path 未改动，直接用 QEMU-LVZ 的源码构建独立目录，避免污染 LoongArch build：

```sh
cd ../QEMU-LVZ
mkdir -p build-riscv && cd build-riscv
../configure --target-list=riscv64-linux-user,riscv64-softmmu \
             --disable-werror --disable-docs
ninja qemu-riscv64 qemu-system-riscv64
```

用户态：

```sh
./qemu-riscv64 -cpu rv64,v=true,vlen=128,elen=64,vext_spec=v1.0 ./prog
```

系统态：

```sh
./qemu-system-riscv64 -M virt -cpu rv64,v=true,vlen=128,elen=64 \
    -m 256M -nographic -bios default -kernel ./payload
```

QEMU 的 `-d cpu` 会打印 RVV CSR 和向量寄存器（`target/riscv/cpu.c:846-880`），可作为寄存器级 golden。

### 8.2 指令级/程序级/系统级

| 层级 | 方法 | 观察点 |
| --- | --- | --- |
| 指令级 | 用 LLVM/`riscv64-linux-gnu-as` 生成单指令或短序列；同一 ELF 在 QEMU-LVZ 与 LATR 上跑 | GPR/FPR/CSR/`vtype/vl/vstart/v0-v31`/内存校验和 |
| 随机序列 | 生成不含控制流的 ALU/M/A/F/D/RVV 序列；每条后把状态写到固定块 | 逐字段比较；掩码/NaN 用位模式比较 |
| 分支/CFG | 生成带分支、JAL/JALR、RVC 的 CFG | 最终 PC、寄存器、内存；TB 边界不作为语义 |
| 程序级 | `riscv-tests`（rv64ui/um/ua/uf/ud/uc）、`riscv-arch-test`、`riscv-vector-tests` | exit code、stdout、内存哈希 |
| 系统级 | OpenSBI + Linux 或 flat S-mode payload | 串口输出、退出码、设备状态；先不比较时序 |

QEMU 自带的 `tests/tcg/riscv64` 只有少量用例（`test-div.c`、`test-fcvtmod.c` 等），不能作为完整回归；外部 `riscv-tests`/`riscv-arch-test` 才是主力。RVV 用 `riscv-vector-tests`（或 QEMU 的 RVV helper 作为第二 golden）。

### 8.3 必测负例

- `JALR` 目标 bit 0 置 1：必须清 0；`C.JR rs1=0` 非法。
- `LW/LWU/LD` 的符号/零扩展；`ADDW/SLLW/SRAW` 的高 32 位符号扩展。
- `DIV/REM` 的除零、`INT_MIN/-1`、32/64-bit 版本。
- `MULHSU` 的负数×无符号边界。
- `LR/SC` 的同值 store、地址不匹配、跨 TB；与 QEMU 值比较语义对拍。
- `AMO` 的 `aq/rl` 顺序；`AMOMAXU` 的无符号边界。
- `FLW` 的 NaN-box；非 boxed 输入按 canonical NaN；`fmin/fmax` 的 NaN/±0；`fflags` sticky。
- RVC 的 2-byte 指令跨页；4-byte 指令作为 TB 首条跨页。
- `FENCE.I` 后自修改代码可见；`SFENCE.VMA` 后旧翻译不可用。
- RVV：`vl=0`、`vl<VLMAX`、`vstart>0`、`vta/vma` 四种组合、masked-off load 不访问内存、fault-only-first 的 `vl` 更新、`vill=1`、LMUL<1/ >1、widening/narrowing、overlap 规则。
- RVV 页边界：masked-off lane 指向未映射页时不得 fault；active lane 跨页时 fault 位置正确。

## 9. 分阶段实施

| 阶段 | 内容 | 退出条件 |
| --- | --- | --- |
| P0 | 目录/构建/IR1 解码（RV64I + RVC）/host 解码测试 | LLVM 编码向量全部通过；`latr-probe` 全绿；`latr-check` 无 libc 泄漏 |
| P1 | RV64IMAC 整数用户态：GPR、分支、load/store、M/A | `rv64ui/um/ua` 通过；分支/RVC/页边界负例通过 |
| P2 | F/D + Zicsr/Zifencei：helper 优先 | `rv64uf/ud` 通过；NaN-box/fflags/fmin/fmax 差分通过 |
| P3 | RVV helper 路径 + PVIR 语义层（VLEN=128） | `riscv-vector-tests` 的整数/掩码/load-store 子集通过；PVIR 校验覆盖所有测试指令 |
| P4 | RVV direct LSX lowering（elementwise + mask/merge） | 与 helper 路径逐位一致；无 LSX 时自动回退；性能基准记录 |
| P5 | 系统态：CSR/trap/Sv39/PMP/CLINT/PLIC/UART | flat S-mode payload 启动；OpenSBI 启动；Linux 控制台输出 |
| P6 | 性能：寄存器映射、TB 链接、LASX、`rotr`、helper 内联 | 相对 P3 有可测提升；差分回归全绿；不引入 LBT/LVZ 依赖 |

## 10. 风险与开放问题

1. **PVIR 的 LOAD 规则 — 已修复（P0）**：`PVIR_OP_LOAD` 现在允许 `ZERO/MERGE/UNKNOWN`；RVV 不要用 `ZERO`，vma=0 用 `MERGE`、vma=1 用 `UNKNOWN`。仍建议在 LATR bridge 里加“RVV 不得使用 ZERO”的断言，防止后续把 SVE 的 zeroing 习惯带进来。
2. **PVIR 的静态 lane 数**：`PvirPredicate.lanes/active_lanes` 是 `size_t`；RVV 的 `vl` 是运行时值。TB 特化可以规避，但会增加 TB 数量；需要基准。
3. **F/V 寄存器映射**：方案 A 的向量 chunk load/store 开销、方案 B 的标量 FP 开销，需要实测；不要在 P1 前把映射写死到所有 lowering。
4. **F/D 精确性**：NaN payload、canonical NaN、NaN-box 检查、`fmin/fmax`、`fflags`/`frm` 都可能与 LA 硬件不同；helper-first 是正确性优先。
5. **PVIR 扩展的回归**：PVIR 同时服务 SVE；任何字段/校验改动都必须跑 `test-pvir` 和 SVE 解码测试，且不能改变现有 SVE 语义。
6. **LBT 硬件语义**：`jiscr1` 指南与 QEMU 冲突；`jiscr0` 的 imm 缩放（QEMU `helper_lbt_jiscr` 按 4 缩放）也需要硬件确认。LATR 只把 LBT 当可选优化。
7. **LVZ 不是加速器**：不要为了“可能加速”把系统态设计绑到 LVZ；先做普通 PLV0 阴影页表/软件 TLB。
8. **SMP 与 LL/SC**：单核 helper 足够；SMP 需要原子 cmpxchg/锁，且 LR/SC 的 reservation 要按 hart 隔离。
9. **自修改代码**：`FENCE.I`/`SFENCE.VMA` 必须清 TB/TLB；否则会执行旧翻译。
10. **扩展声明**：`misa`/`vlenb`/`vtype` 必须与实现一致；不要为了跑某个测试虚报 V/F/D。
11. **系统态设备模型**：Linux 需要 PLIC/CLINT/UART/virtio；这是独立的大工程，不能和 P1-P4 混在一起。

## 11. 参考索引

| 主题 | 位置 |
| --- | --- |
| LATA IR1→IR2 主循环 | `target/arm/lata/translator/translate-a64.c:17329`（`get_ir1_list`）、`:17454`（`translate_aarch64_insn`）、`:17547`（`tr_ir2_generate`） |
| LATA 分发表 | `target/arm/lata/translator/translate-a64.c:17023-17153` |
| LATA TB 直接/间接跳转 | `translate-a64.c:583`（`gen_goto_tb`）、`:445`（`gen_goto_tb_indirect`） |
| LATA 上下文切换 | `target/arm/lata/lata.c:341`（bt→native）、`:401`（native→bt）、`:456/477`（prologue/epilogue） |
| LATA GPR/FPR 访问 | `target/arm/lata/lata.c:744-845` |
| LATA clear-high（禁止复用） | `target/arm/lata/optimization/clear-high.c` |
| PVIR 头 | `target/arm/lata/include/la-pvir.h:71-80`（predicate）、`:131-137`（inactive）、`:151-162`（vector op）、`:246-302`（effects/instruction） |
| PVIR 校验 | `target/arm/lata/pvir.c:502-621`；LOAD 强制 ZERO 在 `:584-588` |
| PVIR mask 展开/cache | `target/arm/lata/pvir.c:273-330`、`:736-810` |
| PVIR 测试 | `tests/test_pvir.c`；`make test-pvir` → 114 checks |
| IR2/LISA 指令 | `target/arm/lata/include/la-append.h`、`target/arm/lata/ir2/la-append.c` |
| LISA 汇编 | `target/arm/lata/ir2/ir2-assemble.c`、`include/ir2-la-assemble.h` |
| RISC-V 解码表 | `../QEMU-LVZ/target/riscv/insn32.decode:350-737`（RVV）、`insn16.decode` |
| RISC-V TB 循环/页边界 | `../QEMU-LVZ/target/riscv/translate.c:1123-1252` |
| RVV vsetvl | `../QEMU-LVZ/target/riscv/vector_helper.c:34-84`、`insn_trans/trans_rvv.c.inc:162-207` |
| RVV mask/tail 循环 | `../QEMU-LVZ/target/riscv/vector_internals.h:150-174` |
| RVV fault-only-first | `../QEMU-LVZ/target/riscv/vector_helper.c:472-546` |
| RVV 配置默认值 | `../QEMU-LVZ/target/riscv/cpu.c:1346-1351`（vlenb=128/8、elen=64）、`:2678-2679`（ta/ma all_1s 默认 false） |
| RISC-V FP/NaN-box | `../QEMU-LVZ/target/riscv/fpu_helper.c:27-55`、`internals.h:87-104` |
| RISC-V 非对齐 fault | `../QEMU-LVZ/target/riscv/cpu_helper.c:1260-1283` |
| LA FP 载入 NaN-box | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_fmemory.c.inc:6-11` |
| LA 除法归零/溢出 | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_arith.c.inc:111-145` |
| LA LL/SC 值比较 | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_atomic.c.inc:6-46` |
| LA AMO | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_atomic.c.inc:48-111` |
| LBT 指南 | `https://jia.je/unofficial-loongarch-intrinsics-guide/lbt_basics/`、`.../lbt/integer/`、`.../lbt/jump/` |
| LBT QEMU 语义 | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_lbt.c.inc`、`tcg/op_helper.c` |
| LVZ | `../QEMU-LVZ/README.md`、`target/loongarch/cpu-csr.h:215-260`、`tcg/tlb_helper.c:721-966` |
| QEMU virt 内存映射 | `../QEMU-LVZ/hw/riscv/virt.c:75-82` |
| 现有 TB 缓存 | `include/loongemu/tb_cache.h` |
| 现有 ARM64 walker 模板 | `include/loongemu/arm64_paging.h`、`src/arm64_paging.c` |
| MIPS 同构设计参考 | `target/mips/latm/DESIGN.md` |
