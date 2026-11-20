# LATM: MIPS64r2 → LoongArch 转译设计研究

状态：**stage-1 已实现并通过 bare-metal/QEMU 回归**。本文前半部分保留设计推导；
实现入口与验证证据见 §13。范围：`mips64el`（little-endian）、MIPS64r2、FR=1（32 个 64-bit FPR）。
不在本轮范围：DSP ASE、MIPS16/microMIPS、MSA、MIPS3D、Release 6 编码、big-endian guest。

已实现：IR1 定长解码、整数 ALU/移位/位域/HI-LO、分支包与延迟槽、
likely 分支、J/JAL/JR/JALR、直接 load/store、LBT `ldl/ldr/ldl.d/ldr.d/stl/str/stl.d/str.d`、
FPU helper（softfloat）以及 prologue/epilogue/寄存器映射。`jiscr0/jiscr1` 的 TB
链接路径仍未启用（见 §13.3）。

## 0. 证据基准

| 角色 | 来源 |
| --- | --- |
| MIPS 语义金标准 | `../QEMU-LVZ`（QEMU 9.0.50；`git log -- target/mips` 仅初始导入，MIPS 未被该 fork 修改） |
| LBT 语义/编码第二基准 | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_lbt.c.inc`、`tcg/op_helper.c` |
| 指令编码 | Homebrew LLVM 23.1.6：`$(brew --prefix llvm)/bin/llvm-mc -triple=mips64el-unknown-linux -mcpu=mips64r2` |
| 现有转译结构 | `target/arm/lata/`、`lata/Makefile`、`baremetal/bare/lata_isa_test.c` |
| LBT IR2 落点 | `target/arm/lata/include/la-append.h`、`target/arm/lata/ir2/la-append.c` |

本文中的“金标准行号”均指 `../QEMU-LVZ` 当前工作树。

## 1. 结论先行

1. **LATM 应作为 LATA 的同构前端，而不是 LATX 的 MIPS 移植。** MIPS64r2 是定长 32-bit RISC，IR1 只需“指令包 + 操作数”，不需要 Capstone；复用 LATA 的 IR2、`la-append`、`ir2-assemble`、`reg-alloc`、TB 切换骨架。
2. **分支与延迟槽在 IR1 层成包。** 每个分支 IR1 强制携带下一条延迟槽 IR1；条件/链接值在延迟槽之前求值，控制流在延迟槽之后发射。这与 QEMU 的 `gen_compute_branch()`（先算条件）→ 延迟槽 → `gen_branch()`（后发跳转）完全同构，见 `translate.c:4574`、`11145`、`15385`。
3. **第一阶段只做可直接寻址的 integer core。** `mips64el` + LBT MIPS memory + 直接 `ldx/stx`；FPU 用 C helper（复用 `fpu/softfloat.c`），CP0/TLB 用 helper。系统态下不能用 LBT 内存指令直接代替会缺页的 guest 访问。
4. **LBT 清单中真正必需的是**：`ldl/ldr/stl/str`（LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR）、`rotr/rotri`（ROTR/ROTRV/DROTR/DROTRV）、`jiscr0`（间接跳转/TB 链接）。`adc/sbc/rcr/rcri` 对 MIPS 没有用处（MIPS 无进位标志）；`x86mfflag/x86mtflag/armmfflag/armmtflag` 也不应挪用（MIPS 的 FPU 条件码是 FCSR 的 FCC 位，不是 N/Z/C/V）。`addu12i` 与 `movgr2scr/movscr2gr` 是可选优化。
5. **`jiscr1` 有参考实现分歧，先禁用。** 指南写 `jiscr1: SCR0=PC; PC=SCR1+imm`，而 QEMU-LVZ 的 `helper_lbt_jiscr()` 对两个 jiscr 都用 `SCR0` 作目标并在 `save_pc` 时覆盖 `SCR0`（`op_helper.c:838`）。LATM 只用 `jiscr0`，把目标显式 `movgr2scr scr0`，避免依赖未确认语义。
6. **构建统一改用 `brew --prefix llvm`**，不再硬编码 `/opt/homebrew/Cellar/llvm/23.1.5_1`；本机为 LLVM 23.1.6。

## 2. 现有 LATA 结构与本轮复用面

### 2.1 流程

```mermaid
flowchart LR
  G[guest PC] --> D[get_ir1_list]
  D --> F[fetch + decode MIPS64r2]
  F --> I[IR1 packet list]
  I --> T[translate_functions]
  T --> R[IR2 list]
  R --> A[ir2_assemble]
  A --> C[LoongArch machine code]
  C --> X[context_switch_native_to_bt]
```

LATA 的关键锚点：

| 功能 | 文件 | 行 |
| --- | --- | --- |
| 解码循环 | `target/arm/lata/translator/translate-a64.c` | `get_ir1_list` 17338 |
| 单条解码 | 同上 | `translate_aarch64_insn` 17455 |
| IR1→IR2 | 同上 | `tr_ir2_generate` 17547 |
| TB 直接跳转 | 同上 | `gen_goto_tb` 583 |
| 间接跳转 | 同上 | `gen_goto_tb_indirect` 445 |
| IR2 汇编 | `target/arm/lata/ir2/ir2-assemble.c` | 全文 90 行 |
| IR2 指令表 | `target/arm/lata/ir2/la-append.c` | 31k 行，生成物 |
| 寄存器分配 | `target/arm/lata/translator/reg-alloc.c` | 全文 |
| 上下文切换 | `target/arm/lata/lata.c` | prologue 460、epilogue 600、alloc 744 |

### 2.2 直接复用

| 组件 | 处理 |
| --- | --- |
| `ir2/ir2.c` | 复用；把 `#include "lata.h"` 条件化为 LATM 头（或 build 时 include 路径覆盖）。 |
| `ir2/ir2-assemble.c` | 原样复用；只依赖生成的 `format-table`。 |
| `ir2/la-append.c` + `la-append.h` | 原样复用；MIPS 所需的 LBT 指令已在表中（`ldl_d` 0x2e800000、`jiscr0` 0x48000200、`rotr_d` 0x001b8000、`rotri_d` 0x004d0000 等）。 |
| `include/la-ir2.h`、`ir2-opcode.h`、`format-table.h` | 原样复用。 |
| `translator/reg-alloc.c` | 复用；提供 LATM 的 `reg-map.h`、`reg_gpr_map[]` 等。 |
| `translator/macro-helper.c` | 复用 `li_d/li_w` 与按宽度 load/store；只依赖 `lata.h` 的 `lsenv`。 |
| `mem.c` | 复用（`mm_malloc` 等）。 |
| `lata.c` 的 prologue/epilogue/context switch | 复制后改 `arm_la_map`→`mips_la_map`、`env_offset_pc`、去掉 ARM FCSR/PSTATE 代码。 |
| `baremetal/bare/lata_isa_test.c` | 作为 `latm_isa_test.c` 模板。 |

### 2.3 不复用/需替换

| 组件 | 原因 |
| --- | --- |
| `ir1/decode_ir1_a64.c.inc`、`ir1.c` | AArch64 专用；LATM 手写 MIPS 解码。 |
| `translator/translate-a64.c` | 17k 行 AArch64 语义；替换为 `translate-mips.c`。 |
| `optimization/clear-high.c` | AArch64 W 写清零高 32 位；MIPS 的 32-bit 写是“符号扩展”，语义不同。用 `store_gpr32_dst()` 统一处理。 |
| `optimization/insts-pattern.c`、`tu.c` | 第一阶段不需要；保留接口不启用。 |
| `include/env.h` | `CPUARMState` 专用；LATM 建 `target/mips/cpu.h` 最小状态。 |

### 2.4 IR2 复用需要的最小改动

`ir2.c` 和 `reg-alloc.h` 只依赖 `lsenv->tr_data` 的字段，不依赖 ARM 寄存器。建议：

```c
#ifdef CONFIG_LATM
#include "latm.h"
#else
#include "lata.h"
#endif
```

LATM 的 `latm.h` 必须提供与 LATA 同名的 `TRANSLATION_DATA`/`ENV` 字段（`ir2_inst_array`、`first_ir2`、`last_ir2`、`itemp_status`、`ftemp_status`、`label_num`、`curr_tb`、`real_ir2_inst_num`）。这样 `ir2.c`、`la-append.c`、`reg-alloc.c`、`ir2-assemble.c` 可零拷贝复用。

## 3. 目标目录与接口

```text
target/mips/latm/
  include/
    latm.h            # ENV/TRANSLATION_DATA/lsenv, mips_la_map
    ir1.h             # MIPS_INSN_TYPE, MipsIR1, packet
    ir1-arg.h         # operand union
    mips-env.h        # CPUArchState + env_offset_*
    reg-map.h         # guest<->LA register map
    translate.h       # DisasContext/DisasContextBase shim
  ir1/decode-mips.c   # 手写定长解码
  translator/
    translate-mips.c  # IR1->IR2
    reg-alloc.c       # 复用/薄封装
  latm.c              # 寄存器表、prologue/epilogue、alloc_gpr_*
latm/Makefile
baremetal/bare/latm_isa_test.c
tests/test_latm_decode.c
```

`DisasContext` 建议：

```c
typedef struct DisasContext {
    DisasContextBase *base;   /* 复用 latx/compat/exec/exec-all.h 的 TranslationBlock */
    uint64_t pc_curr;
    uint32_t insn;
    MIPS_INSN_TYPE insn_type;
    union { MipsArg f; } arg;
    uint32_t hflags;          /* 仅用于异常恢复/TB 入口的延迟槽状态 */
    uint64_t btarget;         /* 分支目标（异常恢复用） */
} DisasContext;
```

`MipsIR1` 采用“分支包”而不是独立指令：

```c
typedef struct MipsIR1 {
    MIPS_INSN_TYPE type;
    uint32_t insn;
    uint64_t pc;
    uint8_t rs, rt, rd, sa;
    int16_t simm;
    uint32_t target;          /* J/JAL 的 26-bit 目标 */
    bool delay_slot;          /* 该条是前一条分支的延迟槽 */
} MipsIR1;
```

分支包的不变量：

- 分支 IR1 的 `pc` 是分支指令地址；
- 下一条 IR1 必须存在，`pc == branch.pc + 4`，`delay_slot == true`；
- 分支包在 TB 内不可拆分；若 `max_insns` 放不下两条，TB 在分支之前结束；
- 延迟槽内出现分支 → 生成 `EXCP_RI`（与 QEMU `gen_compute_branch` 的 `MIPS_HFLAG_BMASK` 检查一致）。

## 4. MIPS64r2 解码

### 4.1 两级 switch

MIPS 定长 32-bit，主 opcode 6-bit：

```c
switch (insn >> 26) {
case 0x00: decode_special();      /* funct */
case 0x01: decode_regimm();       /* rt */
case 0x10: decode_cop0();         /* rs/rd/sel */
case 0x11: decode_cop1();         /* fmt/funct */
case 0x1c: decode_special2();     /* funct */
case 0x1f: decode_special3();     /* funct/op */
default:   decode_primary();
}
```

手写解码约 600–900 行，比引入 QEMU decodetree 更可控；QEMU-LVZ 的枚举（`translate.c:50-450`）可直接作为命名参考，但不要复制其 TCG 实现。

### 4.2 必须按金标准处理的字段陷阱

| 编码 | 陷阱 | 金标准 |
| --- | --- | --- |
| `ROTR` | 与 `SRL` 同 funct，靠 bit21=1 区分；`sa=0` 时退化为符号扩展 | `translate.c:13450` |
| `ROTRV` | 与 `SRLV` 同 funct，靠 bit6=1 区分 | `translate.c:13475` |
| `DROTR/DROTR32` | 分别与 `DSRL/DSRL32` 同 funct，bit21=1 | `translate.c:13530` |
| `DROTRV` | 与 `DSRLV` 同 funct，bit6=1 | `translate.c:13555` |
| `EXT` | `sa=pos`，`rd=size-1`；`pos=0,size=32` 是符号扩展，不是零扩展 | `gen_bitops` 4836 |
| `INS` | `sa=pos`，`rd=msb`（绝对 msb），宽度 `msb-pos+1`；结果符号扩展 32 | 同上 |
| `DEXTM/DEXTU` | `msb += 32` / `pos += 32` 的变体，顺序不能反 | `gen_bitops` 4859 |
| `DINSM/DINSU` | `msb += 32` / `pos += 32`，但 DINS 的 `rd` 是绝对 msb | 同上 |
| `JALR` | `rd` 是链接寄存器，`sa` 是 hint（0 或 16）；不是固定 `$31` | `translate.c:13490` |
| `BGEZAL/BLTZAL` | 链接寄存器无论是否 taken 都写 `PC+8` | `gen_compute_branch` 4800 |
| `BLTZALL/BGEZALL` | likely not-taken 时链接也写，且延迟槽被取消 | `gen_compute_branch` 4720 |
| `BEQ rs,rt` | `rs==rt` 是 always-taken；`BNE rs,rt` 是 always-not-taken，但延迟槽仍执行 | `gen_compute_branch` 4680 |
| `SYNCI` | QEMU 直接结束 TB（`DISAS_STOP`），不执行内存访问 | `translate.c:14641` |
| `RDHWR $2` | 读 Count 后必须结束 TB，允许定时器中断立即进入 | `gen_rdhwr` 11052 |

### 4.3 TB 解码循环

```c
while (n < max_insns) {
    if (n + 1 == max_insns && is_branch(insn)) break;  /* 留出延迟槽 */
    decode(pc, &ir1[n]);
    pc += 4; n++;
    if (ir1[n-1] is branch) {
        decode(pc, &ir1[n]);       /* 延迟槽 */
        ir1[n].delay_slot = true;
        pc += 4; n++;
        break;                      /* TB 在分支包后结束 */
    }
    if (pc crosses page) { is_jmp = DISAS_TOO_MANY; break; }
}
```

`tb->size = pc - tb->pc`，`tb->icount = n`。取指失败/`pc & 3` 生成 `EXCP_AdEL`，与 QEMU `decode_opc` 15260 一致。

## 5. 分支与延迟槽（核心算法）

### 5.1 金标准算法

QEMU 不在解码分支时发射控制流，而是：

1. `gen_compute_branch()`：读 rs/rt，算 `bcond`，算 `btarget`，写链接寄存器；**不跳转**。
2. 下一条指令按 `is_slot` 解码；对于 likely，`decode_opc` 在解码延迟槽前先发“条件不成立 → 跳过延迟槽”的出口（`translate.c:15268`）。
3. 延迟槽翻译完后，`gen_branch()`（`translate.c:11145`）才发 taken/fallthrough。
4. 延迟槽内出现分支 → `EXCP_RI`。

LATM 用 IR1 包实现同样的顺序，不需要 QEMU 的 `hflags` 运行期状态。

### 5.2 IR2 发射模板

**普通条件分支**（`BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ`）：

```text
cond = evaluate(rs, rt)          // 在延迟槽之前，保存到 itemp
<delay slot IR2>                 // 只翻译一次
if (cond) goto target else goto fallthrough
```

**Branch-likely**（`BEQL/BNEL/.../BGEZALL`）：

```text
cond = evaluate(rs, rt)          // 延迟槽之前
if (!cond) goto L_skip
<delay slot IR2>                 // 仅 taken 时执行
goto target
L_skip:
goto fallthrough                 // branch_pc + 8
```

**无条件分支/跳转**：

```text
<delay slot IR2>
goto target
```

**链接**：`JAL/JALR/BGEZAL/BLTZAL/...` 在延迟槽之前写 `GPR[link] = branch_pc + 8`；`JALR` 的 link 是编码中的 `rd`，不是固定 `$31`。

**目标预取**：`JR/JALR` 的目标寄存器必须在延迟槽之前读入 itemp，因为延迟槽可能改写该寄存器；QEMU 的 `btarget` 加载在 `gen_compute_branch` 中，早于延迟槽。

### 5.3 伪代码

```c
static bool trans_branch(DisasContext *s)
{
    IR2_OPND cond = ra_alloc_itemp();
    IR2_OPND target = ra_alloc_itemp();
    IR2_OPND fall = ...;         /* pc + 8 */
    emit_condition_into(cond, s);          /* 读 rs/rt，不写 guest */
    if (s->link) store_link(s->link_reg, s->pc + 8);
    if (s->is_likely) {
        IR2_OPND skip = ir2_opnd_new_type(IR2_OPND_LABEL);
        la_beqz(cond, skip);                /* !cond → skip delay */
        translate_delay_slot(s);            /* 下一条 IR1 */
        emit_goto_guest(target);
        la_label(skip);
        emit_goto_guest(fall);
    } else {
        IR2_OPND taken = ...;
        translate_delay_slot(s);
        la_bnez(cond, taken);
        emit_goto_guest(fall);
        la_label(taken);
        emit_goto_guest(target);
    }
    s->base->is_jmp = DISAS_NORETURN;
    return true;
}
```

`emit_goto_guest()` 内部更新 `env->pc` 并走 `gen_goto_tb` 风格的 TB 出口；直接目标可用 `la_b` patch，间接目标用 `la_jirl(zero, target, 0)` 或 `la_jiscr0`。

### 5.4 异常/中断的 PC

QEMU 的 `exception_resume_pc()`（`exception.c:28`）在 `hflags & MIPS_HFLAG_BMASK` 时把 PC 减 4，即**延迟槽异常回到分支指令重执行**。LATM 应保持同一不变量：

- 分支包内任何 fault/helper exception 都保存 `env->pc = branch_pc`；
- TB 永远不会从“裸延迟槽”开始；
- 若未来支持 TB 入口即延迟槽（例如 EPC 指向延迟槽的调试态），必须在 `env->hflags` 中保存 `BTARGET/BMASK`，并在 IR1 层先合成 pending branch。

第一阶段把 `hflags` 只用于异常恢复，不做 TB 级延迟槽入口。

### 5.5 TB 链接与 `jiscr0`

- 直接分支：`la_b` / `la_beq` 等，26/16-bit 偏移，和 LATA 一样做 `label_dispose`。
- 间接跳转：`JR/JALR` 的 guest 目标先算到 itemp，更新 `env->pc`，走 TB 出口。若启用 TB 链接，把 host TB 地址 `movgr2scr scr0, host_tb` 后 `la_jiscr0(0)`；注意 `jiscr0` 的立即数按 4 缩放（QEMU `helper_lbt_jiscr`：`scr0 + imm*4`）。
- `jiscr1` 不用于 guest `jalr`：它保存的是翻译后代码的 host PC，不是 guest `PC+8`；且参考实现与指南对 SCR1 的分工不一致（见 §7）。

## 6. IR2 降级表

以下 IR2 helper 均已在 `target/arm/lata/include/la-append.h` 中存在；无需新增 IR2 编码（LL/SC 与 AMCAS 见 §6.6）。

### 6.1 立即数与逻辑

| MIPS | 语义 | IR2 |
| --- | --- | --- |
| `ADDIU rt,rs,imm` | `rt = sext32(rs + sext(imm))` | imm∈[-2048,2047]：`la_addi_w`；否则 `li_d(t,imm); la_add_w(rt,rs,t)` |
| `DADDIU` | 64-bit | `la_addi_d`；否则 `li_d(t,imm); la_add_d` |
| `ADDI/DADD` | 溢出异常 | `add_w/add_d` + 溢出检测 + `helper_overflow` |
| `SLTI/SLTIU` | 有/无符号比较 | `la_slti/la_sltui`（si12）；否则 `li_d` + `la_slt/la_sltu` |
| `ANDI/ORI/XORI` | 16-bit 零扩展 | ≤0xfff：`la_andi/ori/xori`；否则 `li_d(t,uimm); la_and/or/xor` |
| `LUI rt,imm` | `sext32(imm<<16)` | `la_lu12i_w(rt, imm << 4)` |
| `AND/OR/XOR/NOR` | 64-bit | `la_and/la_or/la_xor/la_nor` |
| `SLT/SLTU` | 64-bit | `la_slt/la_sltu` |
| `MOVZ/MOVN` | 条件搬移 | `la_maskeqz(t1,rs,rt)` + `la_masknez(t2,rd,rt)` + `la_or(rd,t1,t2)`；MOVN 交换 mask 条件 |
| `MUL`（SPECIAL2） | 32×32→低 32 符号扩展 | `la_mul_w` |
| `CLZ/CLO` | 32-bit count | `la_clz_w/la_clo_w` |
| `DCLZ/DCLO` | 64-bit count | `la_clz_d/la_clo_d` |

注意：`ADDI/DADD` 即使 `rt==$0` 也必须保留溢出异常（QEMU `gen_arith_imm` 2347 只对非 ADDI/DADDI 的 `rt==0` 提前返回）；其余写 `$0` 的 ALU 指令按 NOP 丢弃。

### 6.2 移位/旋转

| MIPS | IR2 |
| --- | --- |
| `SLL/SRL/SRA` | `la_slli_w/la_srli_w/la_srai_w`（32-bit 结果自动符号扩展） |
| `SLLV/SRLV/SRAV` | `la_sll_w/la_srl_w/la_sra_w`（低 5 位） |
| `ROTR/ROTRV` | `la_rotri_w/la_rotr_w`（32-bit，结果符号扩展） |
| `DSLL/DSRL/DSRA` | `la_slli_d/la_srli_d/la_srai_d` |
| `DSLLV/DSRLV/DSRAV` | `la_sll_d/la_srl_d/la_sra_d`（低 6 位） |
| `DROTR/DROTR32` | `la_rotri_d`（DROTR32 的 sa 加 32） |
| `DROTRV` | `la_rotr_d` |

### 6.3 位域/字节

| MIPS | IR2 |
| --- | --- |
| `EXT rt,rs,pos,size` | `pos=sa, size=rd+1`；若 `pos==0 && size==32`：`la_slli_w(rt,rs,0)`；否则 `la_bstrpick_d(rt,rs,pos+size-1,pos)` |
| `INS rt,rs,pos,msb` | `la_bstrins_d(rt,rs,msb,pos)`；随后 `la_slli_w(rt,rt,0)` 做 32-bit 符号扩展 |
| `DEXT/DEXTM/DEXTU` | 按 QEMU `gen_bitops` 的字段加法：`DEXTU` pos+=32；`DEXTM` width+=32；`la_bstrpick_d` |
| `DINS/DINSM/DINSU` | `DINSU` pos+=32；`DINSM` msb+=32；`la_bstrins_d` |
| `WSBH` | `la_revb_2h` + `la_slli_w(...,0)` |
| `SEB/SEH` | `la_ext_w_b/la_ext_w_h` |
| `DSBH` | `la_revb_2h` |
| `DSHD` | `la_revh_2w`（需与 QEMU `gen_bshfl` 4905 做差分测试） |

非法字段组合（`EXT` 的 `pos+size>32`、`INS` 的 `pos>msb`、`DEXT/DINS` 的 `pos+size>64` 等）按 QEMU `gen_bitops` 的 `fail:` 路径生成 `EXCP_RI`，不要静默裁剪。

### 6.4 乘除/HI/LO

| MIPS | IR2 / 说明 |
| --- | --- |
| `MFHI/MFLO` | 从 env `hi/lo` 载入；若目标为 `$0`，NOP |
| `MTHI/MTLO` | 写 env；`rs==$0` 时写 0 |
| `MULT rt,rs` | `la_mul_w(lo,rs,rt)`；`la_mulh_w(hi,rs,rt)` |
| `MULTU` | 同上 low；`la_mulh_wu(hi,rs,rt)` |
| `DMULT` | `la_mul_d` + `la_mulh_d` |
| `DMULTU` | `la_mul_d` + `la_mulh_du` |
| `DIV` | 先按 QEMU 处理 `rs==INT_MIN && rt==-1`、`rt==0`（除数替换为 1），再 `la_div_w/la_mod_w`，结果 32-bit 符号扩展 |
| `DIVU` | 零扩展 32-bit 操作数，`rt==0` 除数替换为 1，`la_div_wu/la_mod_wu`，结果符号扩展 32 |
| `DDIV/DDIVU` | 64-bit 版本，`la_div_d/la_mod_d`、`la_div_du/la_mod_du`，同样先做零/溢出保护 |
| `MADD/MADDU/MSUB/MSUBU` | 64-bit 累加：`acc = (hi << 32) OR (lo & 0xffffffff)`；`prod=mul_d`（MADDU 先零扩展 32-bit 操作数）；`acc±=prod`；`lo=slli_w(acc,0)`、`hi=srai_d(acc,32)`。第一阶段可先用 C helper。 |

### 6.5 访存

| MIPS | IR2（直接映射模式） |
| --- | --- |
| `LB/LBU/LH/LHU/LW/LWU/LD` | `la_ldx_b/la_ldx_bu/la_ldx_h/la_ldx_hu/la_ldx_w/la_ldx_wu/la_ldx_d` |
| `SB/SH/SW/SD` | `la_stx_b/la_stx_h/la_stx_w/la_stx_d` |
| `LWL/LWR/LDL/LDR` | `la_ldl_w/la_ldr_w/la_ldl_d/la_ldr_d` |
| `SWL/SWR/SDL/SDR` | `la_stl_w/la_str_w/la_stl_d/la_str_d` |
| `LL/LLD` | C helper：`lladdr=vaddr; llval=load`；`LL` 结果 `sext32` |
| `SC/SCD` | C helper：`if (vaddr==lladdr) cmpxchg(lladdr,llval,val)`；写 0/1 到 rt |
| `PREF` | `la_preld(hint, base, off)` |
| `CACHE` | NOP（除非 ITC cache） |
| `SYNC` | `la_dbar(0)`；可按 stype 映射 hint |
| `SYNCI` | 结束 TB + 对范围做 TB flush |
| `EHB/PAUSE` | NOP / `idle` |

地址偏移：MIPS 16-bit signed；IR2 `ldx` 用寄存器基址，`ld/st` 用 12-bit SI12。16-bit 偏移超出 SI12 时先 `li_d(t,imm); la_add_d(addr,base,t)`。

### 6.6 LL/SC 为什么不直接用 LoongArch `ll/sc`

QEMU 的 `gen_st_cond()`（`translate.c:2244`）不是硬件 LL/SC 语义，而是：

```text
if (vaddr != lladdr) → rt=0
else rt = (cmpxchg(lladdr, llval, val) == llval)
```

即“地址相等 + 值仍等于上次 LL 读到的值”才算成功；普通 store 只要写回同值，QEMU 仍会成功，而硬件 LL/SC 会因 reservation 被清除而失败。为与金标准一致，第一阶段用 C helper 实现 `lladdr/llval + cmpxchg`；SMP 时用 LoongArch `amcas.d` 或宿主锁保证原子性。LATA 的 IR2 目前没有 `amcas.*`（`latx/inst_template.json` 有，但 LATA 生成的 `la-append.h` 没有），不要为了 LL/SC 去补 IR2，先用 helper。

### 6.7 分支/跳转

| MIPS | 目标 | 链接 | IR2 |
| --- | --- | --- | --- |
| `J target` | 架构：`(pc+4)[63:28] \| (target<<2)`；QEMU-LVZ 实际用 `0xF0000000` 掩码，只保留 `PC[31:28]`（`translate.c:4645`） | 无 | 延迟槽后 `goto`；先复现金标准掩码，再用差分确认 |
| `JAL target` | 同上 | `$31=pc+8` | 同上 |
| `JR rs` | `rs`（提前读入 itemp；QEMU 不额外检查低 2 位） | 无 | `jirl zero,itemp,0` 或 `jiscr0` |
| `JALR rd,rs` | `rs` | `rd=pc+8` | 同上 |
| `BEQ/BNE` | `pc+4+(sext(imm)<<2)` | 无 | 条件 itemp + 延迟槽后分支 |
| `BLEZ/BGTZ/BLTZ/BGEZ` | 同上 | 无 | 同上 |
| `BEQL/BNEL/...` | 同上 | 无 | 延迟槽前 `if(!cond) goto skip` |
| `BGEZAL/BLTZAL` | 同上 | `$31=pc+8`（无论 taken） | 链接在延迟槽前 |
| `BGEZALL/BLTZALL` | 同上 | `$31=pc+8` | likely + 链接 |
| `JR.HB/JALR.HB` | 同 JR/JALR | 同 | 末尾加 `la_dbar(0)`/`la_ibar(0)` |

### 6.8 系统/CP0

| MIPS | 处理 |
| --- | --- |
| `SYSCALL/BREAK` | C helper 设置 `Cause/EPC`，`DISAS_NORETURN` |
| `TGE/TGEU/TLT/TLTU/TEQ/TNE`（含 imm） | C helper：条件成立则 `EXCP_TRAP` |
| `MFC0/MTC0/DMFC0/DMTC0` | C helper `mips_cp0_read/write(env, reg, sel)`；热寄存器后续内联 |
| `DI rt / EI rt` | C helper：返回旧 `Status.IE` 到 rt 并更新 IE；`DI` 结束 TB（`DISAS_STOP`），`EI` 退出 TB（`DISAS_EXIT`）以便立即检查中断（QEMU `translate.c:14758`） |
| `ERET/DERET` | C helper：恢复 PC/Status，`DISAS_NORETURN` |
| `RDHWR` | C helper：0=CPUNum, 1=SYNCI_Step, 2=Count（结束 TB）, 3=CCRes, 29=UserLocal |
| `WAIT` | C helper：halt CPU |

### 6.9 FPU（阶段 3）

第一阶段不实现 FPU；设计选择：

- **用 C helper 走 `fpu/softfloat.c`**，而不是直接把 MIPS FP 指令映射到 LoongArch FPU。原因：MIPS 的 NaN2008/ABS2008、tininess、FCSR cause/enable/flags、FCC 与 LoongArch FCSR 布局不同；QEMU-LVZ `target/mips/tcg/fpu_helper.c` 是金标准。
- `LWC1` 不能用 `fld.s`：LoongArch `fld.s` 会 NaN-box 高 32 位（QEMU-LVZ `trans_fmemory.c.inc:8`），而 MIPS `LWC1` 保留 FPR 高 32 位（`gen_store_fpr32` `translate.c:1385`）。正确序列：`ldx_w` 到 GPR → `la_movgr2fr_w`（deposit 低 32，保留高 32）。
- `SWC1`：`la_movfr2gr_s` → `stx_w`。
- `LDC1/SDC1`：`la_fld_d/la_fst_d`。
- `MFC1/DMFC1/MFHC1`：`la_movfr2gr_s/la_movfr2gr_d/la_movfrh2gr_s`；`MTC1/DMTC1/MTHC1` 对应 `la_movgr2fr_w/la_movgr2fr_d/la_movgr2frh_w`。
- FCSR：在 env 中保存 MIPS 原始 FCSR；每次 FP helper 前把 RM 映射到 `float_status`，helper 返回后把 flags/cause 写回；不要尝试把 MIPS FCSR 直接写进 LoongArch FCSR。

## 7. LBT 使用矩阵（逐条结论）

| LBT 指令 | LATM 用途 | 结论 |
| --- | --- | --- |
| `ldl.w/ldr.w/ldl.d/ldr.d` | `LWL/LWR/LDL/LDR` | **必需**（直接映射模式、little-endian、LBT 可用）。QEMU-LVZ `helper_lbt_mips_ldl/ldr`（`op_helper.c:855/869`）与 MIPS `gen_lxl/lxr`（`translate.c:2000/2026`）的移位/掩码公式一致。 |
| `stl.w/str.w/stl.d/str.d` | `SWL/SWR/SDL/SDR` | **必需**；QEMU-LVZ `helper_lbt_mips_stl/str`（`op_helper.c:884/902`）逐字节写，方向与 QEMU `helper_swl/...`（`ldst_helper.c:75/99/129/173`）一致。 |
| `movgr2scr/movscr2gr` | HI/LO 暂存；`jiscr0` 目标 | **可选**。第一阶段 HI/LO 放 env 内存更简单；阶段 5 可把 HI/LO 放 `scr2/scr3`，但必须在 TB 出口/helper 调用/异常时同步回 env。 |
| `jiscr0` | `JR/JALR`、TB 链接 | **推荐**。目标先 `movgr2scr scr0,target`；立即数按 4 缩放。 |
| `jiscr1` | — | **暂不使用**。指南与 QEMU-LVZ 实现分歧（§7.1）；guest `jalr` 的链接值本来就是 `pc+8`，不需要 `jiscr1` 保存 host PC。 |
| `addu12i.w/.d` | `LUI+ADDIU`/地址合成 | **可选优化**。`lu12i.w`+`ori` 已是两条；是否省一条需基准测试。 |
| `adc/sbc` | — | **不用**。MIPS 整数没有进位标志；QEMU 的 MIPS 语义也不使用 EFLAGS。 |
| `rcr/rcri` | — | **不用**。MIPS `ROTR` 是纯循环移位，直接 `rotr/rotri`；`rcr` 依赖 CF，会引入无关状态。 |
| `rotr/rotri` | `ROTR/ROTRV/DROTR/DROTRV` | **必需**。`ROTR`→`rotri.w`，`DROTR`→`rotri.d`，变量版→`rotr.w/.d`。 |
| `x86mfflag/x86mtflag` | — | **不用**。MIPS 没有 x86 EFLAGS；把 FCC 塞进 EFLAGS 会破坏与 x86/ARM 共享的 LBT4 状态。 |
| `armmfflag/armmtflag` | — | **不用**。MIPS 没有 N/Z/C/V；FCSR.FCC 是独立字段。若未来要复用 `setarmj`，需要显式 FCC↔N/Z/C/V 映射，收益不成立。 |

### 7.1 `jiscr1` 参考分歧

- 指南（`lbt_basics`）：`jiscr1` 把 `PC` 写入 `SCR[0]`，跳到 `SCR[1]+imm`。
- QEMU-LVZ `helper_lbt_jiscr()`：`target = SCR[0] + imm*4`；`save_pc` 时 `SCR[0] = pc`。两个 jiscr 共用 `SCR0` 作为目标。
- LATM 的结论：只用 `jiscr0`，目标显式写 `SCR0`；把 `jiscr1` 的硬件语义列为待 TEG/硬件确认项。

### 7.2 LBT 不可用时的 fallback

LoongEMU 支持无 LBT 的 2K1000LA/2K2000，因此 LATM 不能把 LBT 当唯一实现：

- `ldl/ldr/stl/str`：按 QEMU `gen_lxl/gen_lxr` + `helper_swl/...` 的字节序列实现；或扩展 `src/loongbt_semantics.c` 增加 `LOONGBT_LDL/LDR/STL/STR`，由 `loongbt_exec()` 统一调度。
- `rotr/rotri`：用 `sll/srl/or` 组合或 C helper。
- `jiscr0`：用 `jirl`。
- 在 LATM 翻译时按 CPU feature 选择直接 LBT 或 fallback；不要把 LBT 编码写死在唯一的 lowering 路径里。

## 8. Guest 状态与寄存器映射

### 8.1 状态

第一阶段最小 `CPUArchState`（`target/mips/cpu.h`）：

```c
typedef struct CPUArchState {
    uint64_t gpr[32];
    uint64_t hi, lo;
    uint64_t pc;
    uint32_t hflags;       /* BMASK/btarget 恢复用 */
    uint64_t btarget;
    /* 阶段 2 */
    uint64_t fpr[32];      /* raw 64-bit */
    uint32_t fcsr;
    /* 阶段 4 */
    uint32_t CP0_Status, CP0_Cause;
    uint64_t CP0_EPC, CP0_BadVAddr, CP0_EntryHi, CP0_EntryLo0, CP0_EntryLo1;
    uint32_t CP0_Index, CP0_PageMask, CP0_Wired, CP0_Count, CP0_Compare;
    uint32_t CP0_Config0, CP0_Config1, CP0_Config3, CP0_PRid, CP0_HWREna;
    uint64_t CP0_UserLocal;
    uint64_t lladdr, llval;
    /* TLB、FPU status、异常状态等阶段 4 追加 */
} CPUArchState;
```

初始值对齐 QEMU `MIPS64R2-generic`（`cpu-defs.c.inc:670-695`）：

| 字段 | 值 |
| --- | --- |
| `CP0_PRid` | `0x00010000` |
| `CP0_Config0` | `MIPS_CONFIG0 \| (1<<CP0C0_AR) \| (2<<CP0C0_AT) \| (MMU_TYPE_R4000<<CP0C0_MT)` |
| `CP0_Config1` | 含 FP、63 TLB、I/D cache 参数（照抄 674-677） |
| `CP0_Config3` | `MIPS_CONFIG3 \| (1<<CP0C3_LPA)` |
| `SEGBITS/PABITS` | 42 / 36 |
| `CP1_fcr0` | `F64, 3D, PS, L, W, D, S`（686-688） |
| `CP0_Status_rw_bitmask` | `0x36FBFFFF` |
| `CP1_fcr31_rw_bitmask` | `0xFF83FFFF` |

### 8.2 寄存器映射（初始建议）

保留：

| LA 寄存器 | 用途 |
| --- | --- |
| `$r0` | zero |
| `$r1` | guest `$31`（ra）；helper 调用前后经 env 保存/恢复 |
| `$r2` | env 指针（LATA 用 `s8`；LATM 用 `tp` 可多出一个 guest 槽） |
| `$r3` | host stack |
| `$r4` | runtime/helper 参数 0、TB 出口暂存 |
| `$r12-$r14` | `t0-t2`，3 个 itemp |

可用 guest 槽 24 个（`a1-a7` 7 + `t3-t8` 6 + `fp` 1 + `s0-s8` 9 + `r21` 1）。建议：

| guest | LA | guest | LA |
| --- | --- | --- | --- |
| `$1 at` | `$a1` | `$14 t6` | `$fp` |
| `$2 v0` | `$a2` | `$15 s0` | `$s0` |
| `$3 v1` | `$a3` | `$16 s1` | `$s1` |
| `$4 a0` | `$a4` | `$17 s2` | `$s2` |
| `$5 a1` | `$a5` | `$18 s3` | `$s3` |
| `$6 a2` | `$a6` | `$19 s4` | `$s4` |
| `$7 a3` | `$a7` | `$20 s5` | `$s5` |
| `$8 t0` | `$t3` | `$21 s6` | `$s6` |
| `$9 t1` | `$t4` | `$22 s7` | `$s7` |
| `$10 t2` | `$t5` | `$29 sp` | `$s8` |
| `$11 t3` | `$t6` | `$30 fp` | `$r21` |
| `$12 t4` | `$t7` | `$31 ra` | `$ra` |
| `$13 t5` | `$t8` | | |

溢出到 env 的 guest：`$23 s7`、`$24 t8`、`$25 t9`、`$26 k0`、`$27 k1`、`$28 gp`。这些在典型 Linux 用户态出现频率低；`k0/k1` 只在异常路径使用。

`alloc_gpr_src(0)` 必须返回 `zero_ir2_opnd`；`alloc_gpr_dst(0)` 返回可丢弃的 itemp；`store_gpr_dst(0)` 不发射写。所有 32-bit 结果通过 `store_gpr32_dst()` 统一做 `la_slli_w(...,0)` 符号扩展，避免逐指令漏写。

### 8.3 Prologue/epilogue

复制 LATA 的 `generate_context_switch_bt_to_native/native_to_bt`：

- 进入 TB：保存 host callee-saved；入口约定 `$a0 = CPUArchState *`，prologue 把 `env_ir2_opnd` 固定到 `$tp`（`reg_statics_map[S_ENV] = la_tp`），从 env 载入 guest 直接映射的 GPR/FPR，`jirl zero, tb_addr, 0`。后续 helper 调用仍用 `$a0` 传 env 参数。
- 退出 TB：把 guest 直接映射的 GPR/FPR 写回 env，恢复 host callee-saved，`jirl zero, ra, 0`。
- 不发射 LATA 的 `la_armmtflag`/FCSR 代码；阶段 4 再按 MIPS FCSR 单独同步。
- helper 调用约定：`a0=env`，`a1/a2=参数`，`a3=返回值` 或直接返回；调用前后保存/恢复 guest 映射寄存器（LATA 的 `lata_gen_call_helper_prologue/epilogue` 模板）。

## 9. 构建与验证

### 9.1 构建

`latm/Makefile` 以 `lata/Makefile` 为模板，LLVM 用 Homebrew 前缀：

```make
LLVM  := $(shell brew --prefix llvm)
CLANG := $(LLVM)/bin/clang
AR    := $(LLVM)/bin/llvm-ar
RESINC := $(shell $(CLANG) -print-resource-dir)/include
LATM   := ../target/mips/latm
LATA   := ../target/arm/lata
LATX   := ../latx
DEFS   := -DTARGET_MIPS64 -DCONFIG_LATM -DCONFIG_LATM_STANDALONE \
          -D__WORDSIZE=64 -DCONFIG_LATX -D__thread=
SRCS   := $(LATA)/ir2/ir2.c $(LATA)/ir2/ir2-assemble.c $(LATA)/ir2/la-append.c \
          $(LATA)/mem.c $(LATM)/latm.c $(LATM)/ir1/decode-mips.c \
          $(LATM)/translator/translate-mips.c $(LATM)/translator/reg-alloc.c \
          $(LATA)/translator/macro-helper.c $(LATA)/runtime.c
```

`latm-check` 复用 `tools/check_lata_symbols.sh` 的模式，确保 freestanding 符号 allowlist。

### 9.2 解码单测（host）

- 用 `llvm-mc -show-encoding` 生成每个指令的 little-endian 字节，检查 `decode-mips.c` 的 `type/rs/rt/rd/sa/simm/target`。
- 已用 LLVM 23.1.6 验证的代表编码（可直接作为首批向量）：

| 指令 | 编码（LE bytes） |
| --- | --- |
| `daddiu $2,$3,1` | `01 00 62 64` |
| `daddu $2,$3,$4` | `2d 10 64 00` |
| `drotr $2,$3,5` | `7a 11 23 00` |
| `drotrv $2,$3,$4` | `56 10 83 00` |
| `dmult $3,$4` | `1c 00 64 00` |
| `ldl $2,8($3)` | `08 00 62 68` |
| `ldr $2,8($3)` | `08 00 62 6c` |
| `sdl $2,8($3)` | `08 00 62 b0` |
| `sdr $2,8($3)` | `08 00 62 b4` |
| `ext $2,$3,4,8` | `00 39 62 7c` |
| `ins $2,$3,4,8` | `04 59 62 7c` |
| `dext $2,$3,4,8` | `03 39 62 7c` |
| `dins $2,$3,4,8` | `07 59 62 7c` |
| `wsbh $2,$3` | `a0 10 03 7c` |
| `dsbh $2,$3` | `a4 10 03 7c` |
| `dshd $2,$3` | `64 11 03 7c` |
| `rdhwr $2,$29` | `3b e8 02 7c` |
| `eret` | `18 00 00 42` |
| `synci 0($2)` | `00 00 5f 04` |
| `jalr.hb $2,$3` | `09 14 60 00` |

### 9.3 QEMU-LVZ 差分测试（金标准）

1. **构建 MIPS 用户态 oracle**（MIPS 未改动，使用独立 build 目录，避免污染 LoongArch build）：

   ```sh
   mkdir -p ../QEMU-LVZ/build-mips && cd ../QEMU-LVZ/build-mips
   ../configure --target-list=mips64el-linux-user --disable-werror
   ninja qemu-mips64el
   ```

2. **指令级差分**：为每条指令生成最小 MIPS64EL 程序，把 GPR/FPR/HI/LO/内存结果写到固定地址并以退出码/校验和输出。分别跑 `qemu-mips64el` 和 LATM bare harness，逐项比较。
3. **随机序列差分**：生成不含分支的随机 ALU/移位/位域/乘除/访存序列；每条后用 `rdhwr $2`/store 把状态落盘。分支/延迟槽单独生成 CFG，比较最终状态和内存校验和。
4. **QEMU trace**：`qemu-mips64el -d in_asm,cpu,nochain -D qemu.log` 或 `-singlestep -d cpu` 对比 TB 边界状态。LATM 侧在 `context_switch_native_to_bt_ret_0` 打印同样的 CPU 状态。
5. **LBT 侧**：QEMU-LVZ 的 `target/loongarch/tcg/insn_trans/trans_lbt.c.inc` 与 `tcg/op_helper.c:838-917` 是 LBT 的可执行规范；用它们校验 LATM 生成的 `ldl/ldr/stl/str/jiscr0` 序列，尤其是四种对齐的 merge/store 结果。
6. **程序级**：静态 MIPS64EL `busybox`/小型测试 ELF 在 QEMU 用户态与 LATM 用户态 harness 下比较 stdout/exit code；系统态后续用 `MIPS64R2-generic` + `malta` 或自建板级。

### 9.4 必测负例

- 延迟槽内的分支 → `EXCP_RI`。
- `PC & 3 != 0` → `EXCP_AdEL`。
- `DIV/DIVU` 除零与 `INT_MIN / -1`：按 QEMU 的“除数替换为 1”语义，不 trap。
- `DADD/DSUB/ADD/SUB` 溢出 → `EXCP_OVERFLOW`。
- likely 分支 not-taken：延迟槽不得执行。
- `BGEZAL/BLTZAL` not-taken：链接寄存器仍写 `PC+8`。
- `LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR` 四种对齐 × 旧寄存器高低位模式 × 页边界。
- 32-bit 指令结果高 32 位符号扩展（`ADDU/SLL/ROTR/EXT/INS/WSBH` 等）。
- `fld.s` 与 `LWC1` 的高 32 位差异（阶段 3）。
- `RDHWR $2` 后立即允许定时器中断（结束 TB）。
- `SYNCI` 后自修改代码可见（TB flush）。

## 10. 分阶段实施

| 阶段 | 内容 | 退出条件 |
| --- | --- | --- |
| P0 | 目录/构建/IR1 解码/解码单测 | LLVM 向量全部通过；`latm-probe` 全绿 |
| P1 | integer ALU/移位/分支包/直接访存/bare-metal harness | 能跑整数 CFG 测试并与 QEMU 状态一致 |
| P2 | HI/LO、乘除、位域、非对齐 LBT、LL/SC helper | 所有 ALU/内存差分通过 |
| P3 | FPU helper + FCSR/FCC | MIPS64r2 FP 指令差分通过 |
| P4 | CP0/TLB/异常/系统态 | 能跑最小 MIPS64 Linux 用户态/内核入口 |
| P5 | 性能：寄存器映射调优、HI/LO scratch、TB 链接、`jiscr0`/`addu12i` | 基准不劣于解释器基线；差分回归全绿 |

## 11. 风险与开放问题

1. **LBT 硬件语义未实测**：QEMU-LVZ 的 LBT helper 是当前最可靠规范，但 `jiscr1` 与指南冲突、`ldl/ldr` 的 fault precision 在真实 3A6000 上是否等价仍需硬件验证。LATM 只依赖 `jiscr0`，并把 LBT 内存放在可替换的 lowering 后面。
2. **无 LBT 核**：必须提供 fallback；不能把 `ldl/ldr/stl/str` 写死。`src/loongbt_semantics.c` 目前没有 MIPS memory op，需要扩展或 LATM 自带字节序列。
3. **系统态精确异常**：直接 `ldx/stx` 无法产生 MIPS `BadVAddr/EPC/Cause.BD`；P4 必须走软件 TLB helper，LBT 内存指令只能用于 P1 的直接模式。
4. **FPU 语义**：LoongArch `fld.s` 的 NaN-box 与 MIPS `LWC1` 高半保留不同；MIPS FCSR 与 LoongArch FCSR 位布局不同。helper-first 是正确性优先的选择。
5. **LL/SC 与 SMP**：QEMU 的 value-based SC 与硬件 reservation 不同；第一阶段单核 helper 足够，SMP 需 `amcas`/锁。
6. **自修改代码**：`SYNCI`/`CACHE` 必须触发 TB flush；否则翻译缓存会执行旧代码。
7. **R6 编码混入**：必须显式拒绝 R6-only 编码（compact branch、`aui`、`align`、`bitswap` 等），否则会误译。QEMU 用 `ISA_MIPS_R6` 分支；LATM 固定 R2 路径。
8. **`MADD/MSUB` 的 64×64 语义**：QEMU 在 MIPS64 下按 64×64 产品 + 64-bit HI:LO 累加，再截断 32+32；不要按 MIPS32 的 32×32 直觉实现。

## 12. 参考索引

| 主题 | 位置 |
| --- | --- |
| MIPS 主解码/分支 | `../QEMU-LVZ/target/mips/tcg/translate.c:4574`（`gen_compute_branch`）、`11145`（`gen_branch`）、`15259`（`decode_opc`）、`15385`（`mips_tr_translate_insn`） |
| likely 延迟槽取消 | `translate.c:15268-15276` |
| 异常恢复 PC | `../QEMU-LVZ/target/mips/tcg/exception.c:28` |
| 非对齐 load | `translate.c:2000`（`gen_lxl`）、`2026`（`gen_lxr`） |
| 非对齐 store helper | `../QEMU-LVZ/target/mips/tcg/ldst_helper.c:75/99/129/173` |
| ALU/移位/乘除 | `translate.c:2342/2418/2489/2563/2835/2902/3271` |
| 位域/字节 | `translate.c:4836/4905` |
| RDHWR | `translate.c:11052` |
| LL/SC | `translate.c:2244` |
| SYNC | `translate.c:11483` |
| CPU 模型 | `../QEMU-LVZ/target/mips/cpu-defs.c.inc:670-695` |
| 状态布局 | `../QEMU-LVZ/target/mips/cpu.h:461-530` |
| LBT MIPS memory | `../QEMU-LVZ/target/loongarch/tcg/op_helper.c:849-917` |
| LBT jiscr | `../QEMU-LVZ/target/loongarch/tcg/op_helper.c:838-847` |
| LBT 翻译 | `../QEMU-LVZ/target/loongarch/tcg/insn_trans/trans_lbt.c.inc:514-581` |
| LBT 硬件指令表 | `../QEMU-LVZ/target/loongarch/insns.decode:2300-2324` |
| LATA IR2 复用点 | `target/arm/lata/ir2/ir2.c`、`ir2-assemble.c`、`la-append.c`、`include/la-append.h` |
| LATA TB 流程 | `target/arm/lata/translator/translate-a64.c:17338/17455/17547` |
| LATA 寄存器/上下文 | `target/arm/lata/lata.c:8-120/460-720/744-842` |

## 13. Stage-1 实现与验证（2026-11-20）

### 13.1 代码落点

| 文件 | 内容 |
| --- | --- |
| `latm/Makefile` | freestanding 构建 `latm/build/liblatm_core.a`；复用 LATA 的 `ir2.c`、`ir2-assemble.c`、`la-append.c`、`mem.c`、`macro-helper.c` |
| `target/mips/latm/include/ir1.h` | IR1 包类型、操作码/子操作码、`subop` 扩为 16 bit 以容纳错开的 rotate 子操作 |
| `target/mips/latm/ir1/decode-mips.c` | MIPS64r2 定长两级解码；ROTR/DROTR、EXT/INS、DEXT/DINS、JALR、likely、LBT 内存等 |
| `target/mips/latm/translator/translate-mips.c` | IR1→IR2；分支包、延迟槽、helper 调用桥、直接/LBT 访存、FPU/CP0 入口 |
| `target/mips/latm/translator/reg-alloc.c` | MIPS 寄存器映射与 3 个 itemp（t0–t2） |
| `target/mips/latm/latm.c` | `mips_la_map`、prologue/epilogue、`alloc_gpr_*`、大位移 env 访问 |
| `target/mips/latm/runtime.c` | freestanding 弱符号边界（`tcg_ctx`、`cpu_loop_exit`、TB 钩子） |
| `target/mips/latm/helpers/` | QEMU MIPS helper 适配：整数、CP0、异常、内存、FPU、softfloat 宿主数学 shim |
| `baremetal/bare/latm_isa_test.c` | bare-metal 执行回归：翻译 TB，经 prologue 进入，epilogue 退出，检查 `CPUMIPSState` |
| `tests/test_latm_decode.c` | 纯主机 IR1 解码回归 |
| `tests/test_latm_tlb.c` | 主机 TLB walk / 精确异常回归（27 项） |

根 Makefile 新增：`latm-core`、`latm-isa-image`、`latm-isa-run`、`test-latm-decode`、`test-latm-tlb`、`test-latm`。
`CLANG`/`OBJDUMP` 默认改为 `brew --prefix llvm`，不再硬编码 `23.1.5_1`。

### 13.2 已通过的门禁

- `make test-latm-decode`：21 条向量，覆盖 ALU/IMM、DMULTU、MFLO、ROTR、JAL/JR、BEQ/BEQL、LWL/SDR、SW/SD/LD、SYSCALL/BREAK、ERET/MFC0。
- `make latm-isa-run`：QEMU-LVZ `-M virt -cpu la464`，61 项检查全部通过：
  - 整数 ALU、DSLL/DSRA、DMULTU/MFLO/MFHI、ROTR/DROTR；
  - 分支 taken/not-taken、BEQL taken/not-taken、JAL 链接值、JR 间接目标；
  - `lwl+lwr`、`ldl+ldr`、`sdr+ld` 以及普通 `sw/sd/ld`；
  - `ll/sc` 成功与“中间普通 store 使 reservation 失效”失败路径；
  - `ldc1/add.d/sdc1`（1.5 + 2.25 = 3.75）；
  - COP1X `ldxc1/sdxc1/lwxc1/swxc1/luxc1/suxc1`；
  - 间接跳转 TB 直链：`jiscr0` 与 `jiscr1` 各一条两 TB 链，命中后直接进入目标 TB host code；
  - `CACHE 0`、`CACHE 0x10`、`SYNCI` 分别清除 `pc_map_cache` 中的 TB 链接项；
  - 分支/JR/BC1 的延迟槽内执行 helper（LL）时，分支条件与间接目标经 `latm_spill[]` 保存/恢复；
  - `BC1T/BC1FL` 使用 `cc=(rt>>2)&7`、`tf=rt&1`、`likely=rt&2`，FCC2 测试通过；
  - LL/SC：无 LL 的 SC 失败、LL(A)->SC(B) 失败、普通 store 使 reservation 失效。
- `make test-latm-tlb`：27 项主机检查，覆盖 kseg3 R4K TLB 映射、TLB-backed load、无效 V 位 fault、EPC/Cause/EXL/vector、延迟槽 Cause.BD。
- `make -C latm probe`：全部 LATM/共享/softfloat 源文件语法检查通过。

### 13.3 已知边界

1. **TB 链接是直接映射缓存**：`latm_jmp_cache` 1024 项、每项 32 B，冲突直接覆盖；key 目前只有 guest PC，未包含 ASID/模式，且没有按 vCPU 隔离。已覆盖命中/未命中与 `SYNCI/CACHE` 失效，但尚未接入主循环的 TB 分配器。
2. **`jiscr1` 语义按 QEMU-LVZ 实现**：QEMU 的 `helper_lbt_jiscr` 对两个 jiscr 都用 SCR0 作为目标，`jiscr1` 额外把 host PC 写回 SCR0；LATM 的测试按此语义。若真实硬件遵循指南的 SCR1 目标语义，需要在 feature probe 后选择编码。
3. **CPUMIPSState 位移**：`PC=512`、`hflags≈16116`，后者超出 LoongArch `si12`。`latm_env_ld/st_*` 对超界位移先用 `a0` 物化地址；`a0` 不映射任何 MIPS GPR（r4→a4）。
4. **TLB 精度边界**：已实现 R4000 `map_address`、MIPS64 segment/SegCtl walk、TLB fault 的 BadVAddr/Context/EntryHi/Cause/EPC/BD；尚未实现 TLB shadow 页、TLBINV/TLBINVF 的全量失效语义，也未把 guest physical 与宿主物理内存解耦（测试直接把 PFN 指向宿主 buffer）。
5. **LL/SC SMP 边界**：`SC` 用 `__atomic_compare_exchange`，reservation 使用翻译后的物理地址；普通 store 按物理重叠清除，异常/ERET/TLB 修改也清除。已覆盖单核成功/失败、LL(A)->SC(B)、无 LL 的 SC、store 失效；尚未实现跨核 reservation 总线、写 generation/ABA 严格检测，也未做双核竞争计数器的 QEMU 多 vCPU 测试。
6. **自修改代码边界**：`SYNCI/CACHE` 只清除 link cache 并执行 `ibar 0`；已发出的旧 TB 机器码不回收，正在执行的 TB 也不会被抢占。主循环接入 TB 分配器后需增加代码页回收。
7. **COP1X 数值边界**：load/store 六个变体与四个 MADD 变体已通过。freestanding softfloat 的 `float64_mul` 路径会给出错误结果，因此 `latm_fpu_madd` 暂时使用宿主标量乘加（与 QEMU helper 的两步乘加语义一致）；FCR31 cause/enable/flags 的逐指令回填仍待 softfloat 修复后补齐，不能据此宣称完整 IEEE FMA 签核。
8. **FPU 使能**：bare-metal QEMU 复位后 `EUEN.FPE=0`；执行 softfloat/C helper 中的浮点指令前必须置位 FPE（测试已显式使能）。
9. **翻译器状态仍为进程级静态**：`ir1_list`、`db_rel`、`lsenv/tr_data` 在 freestanding 构建中不是 per-vCPU；SMP 下多个核同时译码/翻译会互相覆盖。需要把 IR1 scratch、translation context、寄存器分配、TB buffer 与异常上下文移入每 vCPU/worker 结构。
10. **其它未覆盖**：MIPS16/microMIPS/MSA/R6、big-endian、TLBINV/TLBINVF、完整系统态指令精确异常、MIPS SMP vCPU/IRQ 集成。
