# RVV 1.0 → PVIR 翻译研究（VLEN=128 / ELEN=64 基线）

状态：**实现稿**。整数/浮点子集、访存、掩码、归约、排列、扩缩窄/饱和已实现；PVIR 解释器与前端发射已落地（详见 §8.5–8.6），但 RVV 1.0 仍有未覆盖指令（见 §8.6 与 DESIGN.md）。
证据基准：`target/arm/lata/include/la-pvir.h`、`target/arm/lata/pvir.c`、`tests/test_pvir.c`（PVIR 语义核）；
`../QEMU-LVZ/target/riscv/insn32.decode:350-737` 与 `insn_trans/trans_rvv.c.inc`（RVV 1.0 编码与语义）。

---

## 0. 结论摘要

1. **PVIR 足以承接 RVV 的"规则网格"部分**：同宽度 elementwise 整数/FP 运算、向量/标量/立即数混合、比较出掩码、merge、unit-stride 访存、掩码+尾语义，都可以用现有 PVIR 表达；**不需要为 RVV 改 PVIR 核心**（P0 已完成的对 LOAD inactive policy 的放宽正是前提之一）。
2. **RVV 的 `vta/vma` 四个组合可以用 PVIR 的单一 `MERGE`（undisturbed）语义全覆盖**：RVV 1.0 规格原文明确允许"in-order 实现忽略这两个位、全部按 undisturbed 执行，但 `vtype` 里仍须提供 `vta/vma`"（v-spec `sec-agnostic` 的 NOTE）。因此 P0 只需一种 inactive policy，不需要两个独立策略位。`UNKNOWN` 只在目的向量可证明死亡时作为优化。
3. **动态 `vl` 用"运行时打包掩码 + `PVIR_PREDICATE_VECTOR_MASK`"表达**：把 `v0[i]` 与 `i < vl` 在进入矢量指令前折进同一组 packed words；PVIR 的 predicate 已是运行时掩码 + versioned 源，expanded-mask cache 直接可用。PVIR 不接受 0 lane，`vl=0` 由前端分支跳过。
4. **TB 特化键 = `(vsew, vlmul)`**（32 种组合），`vl/vstart/mask` 保持运行时；`vsetvli/vsetvl` 结束当前 TB，下个 TB 按新 vtype 重新翻译。这样 PVIR 的 `lanes` 是编译期常量（`VLEN*LMUL/SEW`，最大 128 lane），且避开 PVIR 的 lanes>0 限制。
5. **加宽（widening）可直接映射**：PVIR 允许每个 operand 声明自己的 `PvirVectorType`，只要求 lane 数相同（`pvir.c:518-540`）。`vwadd` 的结果类型 2·SEW、操作数 SEW 合法。**收窄（narrowing）需要先约定"混合位宽语义"**（见 §6.1）：`vnsrl` 这类"宽移位后截断"目前没有显式 PVIR op，建议加 `PVIR_OP_TRUNCATE` 或在 lowering 契约里把 SHR 定义为"按操作数宽度运算、结果按结果类型截断"。
6. **约 44% 的 RVV 指令不能靠现有 PVIR op 直接表达**（跨 lane、归约、转换、饱和、跨步/索引/段访存、mask 寄存器访存），P0/P1 走 helper（先正确后优化），P3 再用 LSX/LASX 内联。按 QEMU 列表统计（359 条非 vset*）：整数 135、FP 92、mask/compare/merge 49、load 33、store 22、跨 lane/move 28。
7. **PVIR 没有执行器**：`pvir.c` 只做验证与掩码展开。RVV 前端必须同时给出 **lowering 契约**（SELECT 操作数顺序、FMA 语义、混合位宽转换、MASK 值到 v0 位的布局），否则"PVIR 能表达"只是纸面能力。本文 §5 给出 P0 需要的契约表。

---

## 1. RVV 前端状态模型（需新增到 `CPUArchState`）

| 字段 | 宽度 | 说明 |
| --- | --- | --- |
| `vreg[32][VLEN/8]` | 32×16 B（VLEN=128） | v0–v31；LMUL 组占连续寄存器（v4 组 = v4..v7） |
| `vtype` | 32 bit | `vlmul[2:0]`、`vsew[5:3]`、`vta[6]`、`vma[7]`、`vill[31]` |
| `vl` | 64 bit | 当前活动 lane 数（≤ vlmax） |
| `vstart` | 64 bit | 重启点；P0 只在内存故障路径写非零 |
| `vxrm` | 2 bit | 定点舍入模式（`vaadd/vssrl/vnclip` 等） |
| `vcsr` | 8 bit | `vxrm` + `vxsat` |

`vlmax = VLEN/8 * LMUL / (SEW/8)`；`LMUL = 2^vlmul`（vlmul=0..5 → 1,2,4,8；6/7 → 1/8,1/4 fractional）。分数 LMUL 的 `vlmax` 更小，寄存器内只占前 `VLEN*LMUL` 位，剩余位保持（undisturbed）。

**与标量 FPR 的冲突**：LA 的 FPR/LSX/LASX 共享物理文件，而 RVV 需要 32 个 VLEN 位寄存器。P0 采用"**v 寄存器放 env，LSX/LASX 仅作临时**"的方案（与 DESIGN §5.10 的结论一致），标量 F/D 继续 1:1 映射到 LA FPR，互不占用。

---

## 2. PVIR 能力与硬约束（代码核对）

| 能力 | 位置 | 对 RVV 的意义 |
| --- | --- | --- |
| lanes = `vector_bits/element_bits`，MASK 的 `element_bits=1` | `pvir.c:63-98` | 一条 PVIR 覆盖整个 LMUL 组（最大 128 lane），无 64-lane 上限 |
| lanes 必须 > 0 | `pvir.c:97` | `vl=0` 不能表达 → 前端运行时分枝跳过 |
| 每 operand 独立 `PvirVectorType`，只要求 lane 数一致 | `pvir.c:518-540` | widening 直接映射；narrowing 需要混合位宽契约 |
| COMPARE 两个 operand 必须都是 VECTOR 且类型相同，结果 MASK | `pvir.c:619-630` | RVV 的 `.vx/.vi` 比较必须先 BROADCAST |
| SELECT/FMA 三个 operand 都必须是 VECTOR | `pvir.c:663-668` | `vfmerge.vfm`、`vmacc.vx` 等需要 BROADCAST |
| SCALAR operand 的类型字段必须全 0（类型由操作推断） | `pvir.c:596-602` | 标量宽度不单独声明，跟随 operand/操作 |
| MASK 类型只支持 EQ/NE 比较 | `pvir.c:554-579` | RVV 没有对 mask 位做大小比较的需求，够用 |
| INACTIVE：MERGE（需 merge_value）/ ZERO / UNKNOWN | `la-pvir.h:145-153` | 对应 RVV 的 undisturbed / agnostic-zero / agnostic-any |
| 内存：base 值或 SP + displacement、单元素宽度、inactive-lane 效果、fault 行为 | `la-pvir.h:200-260` | unit-stride 直接映射；strided/indexed/segment 不具备每 lane 地址 |
| FAULT_FIRST_FAULT 强制 `arch_state` = FIRST_FAULT/UPDATE/CUMULATIVE_SUCCESS | `pvir.c:435-441` | RVV `vleff` 更适合用 **NONFAULTING + RESTART**（见 §6.4） |
| 谓词生成：CONSTANT / LANE_COMPARE（运行时 `lane<vl`）/ STATE_READ | `la-pvir.h:344-370` | `vl` 掩码可用 LANE_COMPARE；也可前端直接算 packed words |
| 掩码展开缓存 key = source id/version + form + first_lane + element_bits + lanes | `la-pvir.h:410-440` | 每个 `v0` 写入有 version，天然支持 RVV 的 mask 版本化 |
| 没有执行器 | `pvir.c` 全文 | ↓ 见 §5 契约 |

**PVIR 现有 op 全集**：ADD/SUB/MUL/DIV/FMA/AND/OR/XOR/NOT/SHL/SHR/SAR/COMPARE/SELECT/LOAD/STORE/ARCH_STATE/PREDICATE_GENERATE/BROADCAST（`la-pvir.h:126-144`）。
缺口：MIN/MAX、REM、SQRT、整数↔FP 转换、SATURATING、跨 lane（shuffle/slide/compress）、归约、mask 寄存器访存、每 lane 地址。

---

## 3. 关键语义决定

### 3.1 由 `vta/vma` 到单一 inactive policy（重要简化）

RVV 1.0 对 tail-agnostic 与 mask-agnostic 的定义是"实现可选择保持原值或全部置 1"。因此：

> **把 agnostic 实现为 undisturbed（保持原值）永远合法。**

于是四种组合都可用 `PVIR_INACTIVE_MERGE` + `merge_value = 旧 vd` 表达；P0 完全不区分 `vta/vma`，也不需要 PVIR 增加第二个 inactive 策略。`UNKNOWN`/`ZERO` 留作后端优化（仅当目的寄存器可证明死亡，或前后端数据流分析证明后续必写）。

### 3.2 `vl` 与 `v0` 的合成

对指令 `vl` 个 lane 生效、且被 `v0` 掩码的语义：

```
active[i] = (i < vl) && (vm ? v0[i] : 1)
```

P0 在进入向量指令之前，由前端生成一组 packed words（每 64 lane 一个 word）：

```
words[k] = mask_word[k] & vl_bits[k]
```

`vl_bits[k]` 由 `vl` 计算（最后不完整 word 用 `(1<<(vl%64))-1`）。这组 words 作为一个 PREDICATE 值（`PVIR_PREDICATE_VECTOR_MASK`，source 为该 words 值的 id/version）传给 PVIR；`vm=1`（无掩码）时 words 只含 vl 位。

- **收益**：同一条 PVIR 指令在 `vl` 变化时无需重译（谓词是运行时的），只需在 `vl/v0` 改变后更新 words 并推进 version。
- **代价**：每条向量指令前需要几条标量/位运算来准备 words；可在 `vsetvli` 或 `v0` 写入后缓存（与 expanded-mask cache 的 source version 对应），避免逐指令重算。

### 3.3 `vl = 0`

PVIR 拒绝 0 lane。前端在矢量指令前插入 `beqz vl, skip`；`skip` 只做 `vstart=0`（RVV 规定 `vl=0` 时全部元素为 tail/inactive，不产生故障，不影响 vstart 外的状态）。

### 3.4 TB 特化键

`(vsew, vlmul)` → 32 种；`vsetvli/vsetvl/vsetivli` 写 `vtype/vl` 后**结束 TB**（`pc=下一条`）。这样：

- PVIR 的 `lanes`、`vector_bits` 是编译期常量；
- `vstart`、`vl`、mask 内容保持运行时可变量；
- 不做 `vl` 级特化（避免代码爆炸），只有 `(vsew,vlmul)` 级。

---

## 4. 逐族映射表

图例：**D** = 直接（现有 PVIR op）；**S** = 现有 op 组合（compare+select / broadcast+op）；**H** = helper（P0/P1 用标量 C 循环）；**X** = 需要 PVIR 小扩展。

### 4.1 整数 elementwise（135 条中的大部分）

| RVV 族 | 映射 | 说明 |
| --- | --- | --- |
| `vadd/vsub/vrsub`（vv/vx/vi） | D/S | `vadd.vx/vi` = BROADCAST 后 ADD；`vrsub` = BROADCAST 后 SUB（标量在前） |
| `vand/vor/vxor`（vv/vx/vi） | D/S | 同上，AND/OR/XOR |
| `vsll/vsrl/vsra`（vv/vx/vi） | D/S | SHL/SHR/SAR；立即数用 IMMEDIATE operand |
| `vmul/vmulh/vmulhu/vmulhsu` | D/H | `vmul` = MUL；`vmulh*` 无高位乘法 op → H（或用 MULH 的 lowering 契约扩展） |
| `vdiv/vdivu/vrem/vremu` | D/H | DIV 直接；REM 无 op → H（或 `a-(a/b)*b` 三条 op，`vrem` 语义含除零/溢出，用 H 更稳） |
| `vmin/vmax/vminu/vmaxu` | S | `COMPARE` + `SELECT`（两三条 op）；NaN 无关（整数） |
| `vsadd/vssub/vsaddu/vssubu` | S/H | 饱和加减可 compare+select 合成；P0 先用 H（含 `vxrm` 的 vaadd 也走 H） |
| `vaadd/vasub/vaaddu/vasubu`（定点） | H | 依赖 `vxrm`，helper |
| `vsmul/vssrl/vssra/vnclip*` | H/X | 收窄 + 舍入 + 饱和；P1 helper，P3 视需求扩展 |
| `vwadd/vwsub/vwaddu/vwsubu`、`vwadd.w*` | D | operand 类型 SEW、结果 2·SEW；`.w` 形式是"宽累加"= operand0 宽、operand1 宽并用 ADD |
| `vwmul/vwmulsu/vwmulu` | D/H | 窄×窄→宽：MUL 的 operand 类型为 SEW、结果 2·SEW 可直接表达（是否允许混合宽度由 §6.1 契约确认） |
| `vmacc/vnmsac/vmadd/vnmsub`（vv/vx） | S | FMA（`+/-` 通过 0-x 取反合成）或 MUL+ADD；`.vx` 需 BROADCAST |
| `vwmacc/vwmaccsu/vwmaccu/vwmaccus` | S/H | 宽 FMA；P1 先 H |
| `vzext.vf2/4/8`、`vsext.vf2/4/8` | X/H | 无显式扩展 op；建议加 `PVIR_OP_EXTEND`（带 signedness），否则 H |
| `vnsrl/vnsra`（wv/wx/wi） | X/H | 宽移位后截断；需要裁剪 op 或混合位宽契约（§6.1） |

### 4.2 mask / compare / merge（49 条）

| RVV | 映射 | 说明 |
| --- | --- | --- |
| `vmseq/vmsne/vmslt(u)/vmsle(u)/vmsgt(u)` | D/S | COMPARE → MASK；`.vx/vi` 先 BROADCAST；`vmsgtu.vi` 是 `vmsltu` 的操作数互换改写 |
| `vmfeq/vmfne/vmflt/vmfle/vmfgt/vmfge` | D/S | FLOAT COMPARE（quiet，见 `la-pvir.h:175`） |
| `vmadc/vmsbc`（vvm/vxm/vim） | H | 进位/借位 + 掩码更新；含 `v0` 作为输入与输出，helper 更清晰 |
| `vmand/vmnand/vmandn/vmor/vmnor/vmorn/vmxor/vmxnor` | D/S | 对 packed mask 位做位运算：用 BITS/MASK 值 + AND/OR/XOR/NOT；注意 `vnand` 等需要 NOT 与掩码域裁剪 |
| `vmerge.vvm/vxm/vim`、`vfmerge.vfm` | S | SELECT（操作数 0 = 展开后的 v0 向量，1/2 = 两个值）；`.vx/vi` 需 BROADCAST |
| `vmv.v.v/v.x/v.i` | S | `.v.v` = ARCH_STATE/复制；`.v.x/.v.i` = BROADCAST |
| `vcpop.m`、`vfirst.m`、`vid.v`、`viota.m` | H | 归约/索引类；helper |
| `vmsbf/vmsif/vmsof` | H | mask 前后缀扫描；helper |
| `vmv.x.s`、`vmv.s.x` | H | 标量↔向量第 0 元素；直接写 env |
| `vmv<n>r.v`、`vl<n>re*/vs<n>r` | X/H | 整寄存器搬移/访存；P0 用 H，后续可用 ARCH_STATE READ/WRITE 两条 |

### 4.3 load/store（33 + 22 条）

| RVV | 映射 | 说明 |
| --- | --- | --- |
| `vle8/16/32/64.v`、`vse*` | D | LOAD/STORE，`memory_element_bits=SEW`，`vector_bits=VLEN*LMUL` |
| `vle*ff.v`（fault-only-first） | D | `PVIR_FAULT_NONFAULTING` + `ARCH_STATE RESTART`（P0 用 NONFAULTING，语义见 §6.4） |
| `vlm.v` / `vsm.v`（mask load/store） | H | 8-bit packed mask 访存 + 位重排；P1 helper |
| `vlse/vsse`（strided）、`vlxei/vsxei`（indexed） | H | PVIR 只有单地址；P2 helper（或每 lane 一条 LOAD 的展开） |
| `vlseg/vsseg`（segment, nf） | H | 多寄存器输出；P2 helper |
| `vl1re/vl2re/vl4re/vl8re`、`vs1r..vs8r` | H | 整组加载/存储；P1 helper |

### 4.4 FP（92 条）

| RVV | 映射 | 说明 |
| --- | --- | --- |
| `vfadd/vfsub/vfmul/vfdiv`（vv/vf） | D/S | FLOAT 类型 + 对应 op；`.vf` 需 BROADCAST |
| `vfrsub`、`vfrdiv` | S | 标量在前，BROADCAST 后 SUB/DIV |
| `vfsgnj/vfsgnjn/vfsgnjx` | S | `(a & ~sign)|(b & sign)` / 取反 / XOR；需要 sign mask 常量（BROADCAST 或 IMMEDIATE 位模式） |
| `vfmin/vfmax` | S/H | NaN/±0 语义特殊；P0 用 H（compare+select 不能精确复制 NaN 规则） |
| `vfsqrt`、`vfrec7`、`vfrsqrt7` | H | 无 SQRT 等 op；helper（软浮点） |
| `vfcvt/vfwcvt/vfncvt`（含 RTZ/ROD 变体） | H | 无转换 op；helper（软浮点），注意 `frm`/`fflags` |
| `vfmacc/vfmsac/vfnmacc/vfnmsac/vfmadd/vfmsub/vfnmadd/vfnmsub` | S | FMA + 取反合成；`.vf` 需 BROADCAST |
| `vfwadd/vfwsub`（vv/vf/wv/wf）、`vfwmul`、`vfwmacc*` | S/H | 加宽 FP：per-operand 类型 2·SEW；P0 先 H 保证舍入正确 |
| `vfredosum/vfredusum/vfredmax/vfredmin/vfwred*` | H | 有序/无序归约；helper |
| `vfclass` | H | 分类位掩码 |
| `vfmv.f.s` / `vfmv.s.f` / `vfmv.v.f` | H/S | 标量↔元素 0 用 H；`vfmv.v.f` = BROADCAST |

### 4.5 跨 lane / 搬移（28 条）

`vrgather{,_vi,_vx}`、`vrgatherei16`、`vslideup/down{,_vi,_vx}`、`vslide1up/down`、`vfslide1up/down`、`vcompress`：**H**（P1），P3 再考虑 LSX `vshuf/vperm` 或 LASX。

---

## 5. P0 必须钉死的 lowering 契约（PVIR 无语义执行器）

`pvir.c` 只验证结构；语义由 LATR 的 IR2 后端实现。**本文选择的形状已经过 `pvir_instruction_validate()` 实测**：`make test-pvir-rvv-shapes` → 10 checks（masked `vadd.vv`+MERGE、`vadd.vx` 标量操作数、`vadd.vi` 立即数、`vmseq.vv` 的 MASK 结果、`vwadd.vv` 加宽类型、`vnsrl.wv` 收窄形状、`vmacc.vv` 三向量 FMA、FP 形状带 active-lane 异常、以及 `vl=0` 被拒绝）。一个容易踩的细节：**零初始化的 `PvirInstruction` 不是合法指令**——`effects.fault.unknown_from_lane` 必须显式设为 `PVIR_NO_VALUE`（`pvir.c` 的 `valid_fault_effect` 对 `FAULT_NONE` 要求该哨兵）。

1. **SELECT**：`(cond, if_true, if_false)` 还是 `(cond, a, b)`？位级还是按 `type` 解释？RVV `vmerge` 需要 `cond` 为展开后的 v0 位（全 1/全 0），按元素宽度全选。
2. **FMA**：`a*b+c` 与融合（单次舍入）与否；RVV `vmacc` 要求融合。浮点路径必须与软浮点 `muladd` 一致。
3. **混合位宽整数运算**：操作数按各自 `element_bits` 读取后，是否先零/符号扩展到结果宽度？移位是否在操作数宽度内进行再截断？（决定 `vwadd`/`vnsrl` 能否直接映射。）
4. **COMPARE → MASK 的位布局**：mask lane i 在 packed word 的 bit i（PVIR 的 MASK 语义），与 RVV v0 的布局一致 → 可直接写回 `vreg[0]` 的低 `ceil(vl/8)` 字节。
5. **BROADCAST 的截断/符号**：标量 x 广播到 SEW 元素时按低 SEW 位取（RVV `vx` 语义），有符号扩展只发生在运算解释阶段。
6. **LOAD/STORE 的 inactive 与 fault**：`PVIR_INACTIVE_MEMORY_NO_TRANSACTION` + `PVIR_FAULT_NONFAULTING` 的组合必须定义"哪些 lane 的地址被计算、哪些 lane 不产生访问"。
7. **ARCH_STATE 的 opaque state id**：`vstart`、`vxrm/vxsat`、整寄存器复制分别用哪个 `state_id`，版本如何递增。

---

## 6. PVIR 缺口与候选扩展（按优先级）

> 逐条缺口、注入形态与"不影响 SVE"的机械约束见 [`RVV_PVIR_GAPS.md`](RVV_PVIR_GAPS.md)；该文同时给出分批补丁计划
> （A 契约/测试 → B 只加枚举 → C 新 payload → D 放宽校验 → E 可选）与已冻结的枚举编号断言。

### 6.1 `TRUNCATE/EXTEND` 与混合位宽契约（P0，最高优先级）

- 需求：`vnsrl/vnsra/vnclip`、`vzext/vsext`、`vfncvt`。
- 最小方案：增加 `PVIR_OP_TRUNCATE`（结果类型 SEW，输入类型 2·SEW）与 `PVIR_OP_EXTEND`（signedness 决定符号）。
- 替代方案：把 `SHR` 的语义定义为"按 operand0 宽度移位、结果按结果类型截断"，并让 `vzext/vsext` 用 `AND`/`SAR` 合成（需要结果宽度与符号表示都清晰）。**必须在 P0 前二选一并写测试**。

### 6.2 `MIN/MAX`（P0，已注入）

已加 `PVIR_OP_MIN/MAX`（整数限定；FP 的 NaN/sNaN 规则留给专门 op/helper，避免隐式改变 SVE 的 FLOAT 语义）。

### 6.3 `REM`（P1，已注入）

`PVIR_OP_REM` 已加入（整数限定）；除零/溢出仍由 lowering 契约处理（与 DIV 一致）。

### 6.4 fault-only-first（P1，**现有 API 已可表达，无需扩展**）

- 现状（本轮实测修正）：`NONFAULTING` 在顶层校验中**强制** `arch_state` 为 `FIRST_FAULT / UPDATE / CUMULATIVE_SUCCESS`，并要求 `arch_state.output_value != PVIR_NO_VALUE`（`pvir.c:987-997`）；`RESTART` 域会导致校验失败。
- 因此 `vleff` 的形状是：`PVIR_FAULT_NONFAULTING` + `fault_result=UNKNOWN` + `unknown_from_lane=故障 lane 值` + `arch_state` = FIRST_FAULT/UPDATE/CUMULATIVE_SUCCESS；实测通过（`test-pvir-rvv-shapes` 的 `vleff` 用例）。
- 语义对应：`CUMULATIVE_SUCCESS` 的"边界之前是已知成功"正是 RVV 的"故障 lane 之前已写入"；`vstart` 由 lowering 从 `unknown_from_lane` 或累计状态边界写回。**不新增 API，也不改 SVE 语义**；只需在 lowering 契约里定义 `vstart` 的写回规则。

### 6.5 mask 寄存器访存与整寄存器搬移（P2）

`vlm/vsm` 已通过"LOAD/STORE 允许 MASK + 8-bit 内存元素"落地（位序 lane i → bit i%8 of byte i/8）；整寄存器 `vl<n>re/vs<n>r` 仍用 `BITS` 向量 + `ARCH_STATE` 的既有表达（已实测）。

### 6.6 跨 lane / 归约（P3）

不建议进入 PVIR 核心：跨 lane 是体系结构/微架构强相关的 shuffle 网络。用 LSX `vshuf.b/vperm.w`、LASX 或标量 helper；PVIR 只负责"每 lane elementwise + 掩码 + 访存"。

---

## 7. 分阶段计划与验收

### P0：最小可用 RVV（整数 elementwise + unit-stride）

范围：`vset{i}vl{i}`、`vle/vse`（8/16/32/64、unit-stride）、`vadd/vsub/vand/vor/vxor/vsll/vsrl/vsra`（vv/vx/vi）、`vmseq/vmsne/vmslt(u)/vmsle(u)`（vv/vx/vi）、`vmerge`（vvm/vxm）、`vmv.v.*`、`vstart/vl/vtype` 状态、`vl=0` 跳过、`vta/vma` 全部按 undisturbed。
验收：
- 每条指令族 × SEW∈{8,16,32,64} × LMUL∈{1,2,4,8} × vl∈{0,1,vlmax-1,vlmax} × mask∈{off,on,全 0,全 1,交替} 的差分向量；
- 与 QEMU-LVZ 对比：`qemu-system-riscv64 -cpu rv64,v=true,vlen=128,elen=64` 运行同一段 guest 汇编（`vlen`/`elen` 是 QEMU 的 CPU 属性，见 `../QEMU-LVZ/target/riscv/cpu.c:1918,1958`），比较目标寄存器 + `vl/vtype/vstart`；
- 负例：`vill=1`、保留编码、`vl>vlmax` 置 `vill`。

### P1：乘除、定点/饱和、mask 生成、memory 变体

`vmul/vdiv`、`vmin/vmax`（合成）、`vmacc/vnmsac`、`vmadc/vmsbc`、`vcpop/vfirst/vid/viota`、`vslide*`、`vrgather`、`vlm/vsm`、`vlseg/vlse/vlxei`、`vmv<n>r/vl<n>re` —— 新指令优先 helper，正确性通过后再按热点内联。

### P2：FP 向量

`vfadd/vfsub/vfmul/vfdiv/vfma*`（FMA 融合）、`vfsgnj*`、`vfmin/max`（helper）、`vfsqrt/vfrec7`（helper）、`vfcvt` 族（helper，联调 `frm/fflags`）。

### P3：故障与重启路径

`vleff`、跨页向量访问、`vstart` 重启、页故障精确性（与 LATR 的 MMU/页故障路径共用 `latr_raise_mem_fault`）。

### P4：性能

v 寄存器直接映射、mask words 缓存、LSX/LASX 宽度利用（LMUL≤2 时一条 LASX）、跨 lane 指令用 LSX shuffle 内联。

---

## 8. 风险与开放问题

1. **PVIR 无执行器**：所有"能表达"的结论都要落到 IR2 lowering 契约（§5），否则会在实现时返工。
2. **混合位宽语义未定**：widening 直接映射依赖"操作数各自宽度 + lane 数相同"的 lowering 规则；narrowing 需要 `TRUNCATE` 或 SHR 契约（§6.1）。
3. **尾/掩码优化**：P0 全用 undisturbed 是正确的，但会阻止后端把目的寄存器当"纯写"（影响寄存器分配与 store 消除）。`UNKNOWN` 只能在前端数据流证明目的死时使用。
4. **vl 变化后的谓词缓存**：mask words 的 version 必须随 `v0` 写入与 `vl` 改变递增，否则 expanded-mask cache 会命中旧位。
5. **LMUL 组与别名**：RVV 允许 source/dest 重叠（如 `vadd.vv v0,v0,v1`）；PVIR 的 merge_value 与 operand 是同一次读，必须保证 lowering 先读后写。
6. **FP 状态映射**：LA FCSR 与 RVV `frm/fflags` 的互操作（vxrm/vxsat 只在定点指令上），需要和标量 F/D 共用一套 `fcsr` 语义。
7. **VLEN 与宿主 LSX 宽度**：VLEN=128（一条 LSX）是基线；若后续宣称 VLEN=256 必须先把 direct lowering 通过差分，且 PVIR 的 mask words 与寄存器分组都要重算。
8. **QEMU 版本差异**：与 BSIMSOI 的教训相同，RVV 的 golden 只能是 QEMU-LVZ 的 9.0.50 行为；不得用其他 QEMU 版本的向量结果做 golden，也不得混用。

---

## 8.5 实现进度（本轮）

**已完成：P0 第 1–2 步（状态 + `vset*` + 向量 CSR）**

- `CPUArchState` 新增 `vtype/vill/vl/vstart/vxrm/vxsat/vreg[32][16]`（VLEN=128、ELEN=64；追加在结构尾部，既有 offset 不变）。
- 解码：`vsetvli/vsetvl/vsetivli` 走 QEMU decodetree；IR1 恢复 `zimm` 字段承载 vtype 立即数（`csrwi` 的 uimm 仍走 `rs1`）。
- `latr_rvv_vsetvl(env, s1, vtype)` 逐行对齐 QEMU `HELPER(vsetvl)`：`vlmul=4`/保留位/`VEDIV`/`SEW>ELEN`/分数 LMUL 容量不足 → vill（vl=0、vtype=0、vstart=0）；否则 `vlmax = VLEN >> (vsew+3-lmul)`、`vl=min(s1,vlmax)`；设置 `mstatus.VS=Dirty`。
- 发射：`vsetvli/vsetvl/vsetivli` 按 QEMU `do_vsetvl()` 的 AVL 规则（`rd=x0,rs1=x0` → 当前 vl；`rs1=x0` → 最大值；`vsetivli` → uimm），调用 helper、写 `rd`、推进 PC 并**退出 TB**（vtype 改变后下一 TB 重新特化）。`mstatus.VS==0` 时报非法（QEMU `require_rvv`）。
- CSR：`vstart(0x008)/vxsat(0x009)/vxrm(0x00a)/vcsr(0x00f)/vl(0xc20)/vtype(0xc21)/vlenb(0xc22)`；`vl/vtype/vlenb` 只读；`vl/vtype` 写入报非法；`vstart` 只保留 lg2(VLEN)=7 位；`vcsr` 读写拆分 `vxrm/vxsat`；所有向量 CSR 在 `mstatus.VS==0` 时报非法；写操作置 `mstatus.VS=Dirty`；`vtype` 读回在 bit63 反映 `vill`。
- 测试：`test_rvv_state`（helper 的 12 组 vtype/vlmax/vill、guest `vsetvli/vsetivli/vsetvl`、AVL 三种规则、VS 门控、CSR 读写/只读/别名），裸机合计 **605 checks**；`latr-diff` 不变。
- **未做**：`misa.V` 仍未置位（避免向 guest 宣称尚不可用的向量执行）。

## 8.6 实现进度（向量执行轮）

**已完成：P0 的整数执行子集 + `vle/vse` + 掩码词 + `vstart`**

- 解码登记 51 条：`vle/vse` 的 8/16/32/64 位 unit-stride（`nf=1`），`vadd/vsub/vand/vor/vxor/vsll/vsrl/vsra` 的 vv/vx/vi，`vmseq/vmsne/vmslt/vmsltu/vmsle/vmsleu` 的 vv/vx/vi，`vmerge.vvm/vxm/vim`，`vmv.v.v/x/i`。IR1 新增 `vm`/`nf` 字段。
- `env->v_mask_words[2]`：每指令由 `vl` 与 `v0` 重建，`v_mask_version` 递增；helper 用词位判定活动 lane。
- helper：整数 ALU（按 SEW 截断/符号扩展/移位量取 `SEW-1`）、比较（写 mask 寄存器）、`vmerge`（v0 选 vs2/vs1，标量形式等价）、`vmv`、unit-stride 访存（MMIO 走 `latr_mmio_load/store`；`nf>1` 显式报非法）。
- agnostic：`vma/vta=1` 的 lane 写全 1 元素；mask 目标尾部保持 undisturbed（与 QEMU `rvv_ta_all_1s=false` 默认一致）。
- `vstart`：从 `vstart` 开始；`vstart>=vl` 清零返回；成功后清零；访存故障时 `vstart=故障 lane`、`mem_va=元素地址`，由 `latr_raise_mem_fault` 投递。
- 门控：仅向量 opcode 发射 `mstatus.VS`/`vill` 检查；scalar 路径不受影响。
- 测试：`latr-isa` 714 checks、decode 26 checks、`latr-diff` 不变。
- **本轮扩展（已实现并测试）**：
  - `misa.V`（bit 21）已置位；`misa = 0x8000_0000_0020_112d`。
  - 访存：strided `vlse/vsse`、indexed `vluxei/vloxei`/`vsuxei/vsoxei`；indexed 的 EEW 为索引宽度且索引零扩展（对齐 QEMU `GEN_VEXT_GET_INDEX_ADDR`），strided 步长为 GPR 符号扩展值；`nf>1` 仍显式非法。
  - mask：`vmand/vmnand/vmandn/vmxor/vmor/vmnor/vmorn/vmxnor`；`vcpop/vfirst/vmsbf/vmsif/vmsof/viota/vid`。
  - 归约：整数/宽整数/FP 归约（`vred*`、`vwredsum(u)`、`vfred*`、`vfwred*`）。
  - 排列：slides、`vrgather`（vv/vx/vi）、`vrgatherei16`、`vcompress`。
  - 扩缩窄/饱和：`vwadd(u)/vwsub(u)/vwmul(u/su)`、`vwmacc*`、`vnsrl/vnsra`、`vnclip(u)`、`vsadd(u)/vssub(u)`、`vaadd(u)/vasub(u)`、`vssrl/vssra`（`vxrm` 四模式）。
  - 向量浮点：单宽算术/min/max/sqrt/sgnj/merge/mv、比较、class、单宽与宽窄转换（含 rod）、`vfwadd/vfwsub/vfwmul`、FP 归约。
  - 整数标量移动：`vmv.x.s`、`vmv.s.x`。
- **PVIR 全覆盖、TB 缓存与混合 LSX/LASX lowering 已落地**：
  - 解码/执行补齐：`vlm.v/vsm.v`、`vmul`、`vdiv(u)`、`vrem(u)`、`vmin(u)`、`vmax(u)`、`vmsgt(u)`、`vzext.vf2/4/8`、`vsext.vf2/4/8`；修复 `vsll/vsrl/vsra.vi` 无符号移位立即数、`vmv.x.s x0` 保护、`vmv.s.x`/`vfmv.s.f` 的 `vl==0` 与 `vta=1`、`INT_MIN/-1` 溢出与 masked `CONVERT`。
  - PVIR 执行与缓存：`pvir-exec.c` 补齐 `FIXED`/`EXTRACT`/`INSERT`/`LANE_INDEX`/`LOAD`/`STORE`（含 `vlm.v/vsm.v`）与带 mask `CONVERT`（含 `vfncvt.rod`）；`LatrTbKey.vspec` 按 `(vtype, vill, vl)` 特化 TB；64 槽 per-TB PVIR 描述符缓存（`latr_pvir_cache_*`）追踪命中 `env->pvir_cache_hits`。
  - 3 级自适应直接 lowering（`translator/pvir-lower-lsx.c`）：Tier 2 LASX (`la_xv*`, 32B) + Tier 1 LSX (`la_v*`, 16B) 支持同一指令内按 32B+16B 混合降低，Tier 0 回退 C helper；裸机打开 `CSR.EUEN` (`FPE|SXE|ASXE`) 验证通过（1181 checks，含 `-march=rv64gcv` QEMU 差分）。
- **仍未做**：segment/whole-register/`vleff`、FP FMA/`vadc/vmadc/vmacc`、组对齐检查、归约与排列族 PVIR 化。
- **软浮点修复**：`latx/compat/qemu/host-utils.h` 的 `mulu64/muls64` 原先按 `(hi,lo)` 解释出参，而 QEMU 调用方按 `(lo,hi)` 传递；乘法尾数因此丢失。已修正并加入 `mulu64` 与 `float32_mul` 回归。

## 9. 立即可做的 P0 任务清单

1. `CPUArchState` 增加 `vreg/vtype/vl/vstart/vxrm/vcsr`；`latr_tr_data_init` 清零。
2. 解码 `vsetvli/vsetvl/vsetivli`（QEMU 已有 trans，可直接复用 decodetree 表），实现 vtype 解析、`vlmax`、`vill`、`vl` 计算；写 CSR/状态并结束 TB。
3. 新增 IR1 tag：`VSETVLI/VSETVL/VSETIVLI`、`VLE*/VSE*`、`VADD/VSUB/...`、`VMSEQ/...`、`VMERGE/VMV`；先在 `.def` 里按 QEMU 名字登记。
4. mask words 构造：`vl` 与 `v0` 合成 packed words，存到 `env->v_mask_words[VLEN/64]`，version 递增；`vl=0` 分支跳过。
5. PVIR 描述生成：为 `vadd.vv` 等构造 `PvirInstruction`（type=INTEGER/SIGNED 或 UNSIGNED、operand_types、predicate=VECTOR_MASK、inactive=MERGE(旧 vd)），先只做"描述 + 一致性自检"，不接 IR2。
6. 差分测试骨架：同一段 RVV 汇编分别跑 QEMU-LVZ（`-cpu rv64,v=true,vlen=128,elen=64`）与 LATR（先只验证 vsetvl/状态），把结果写进 `tests/` 的向量表。
