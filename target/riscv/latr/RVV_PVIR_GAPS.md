# PVIR 承载 RVV 的剩余缺口与无侵入注入方案

范围：只讨论"PVIR API/校验器需要新增什么"，以及"如何在**不改变 SVE 已有语义**的前提下加进去"。
PVIR 能力核对见 [`RVV_PVIR.md`](RVV_PVIR.md)；本文每条结论都用 `pvir_instruction_validate()` 实测或 `pvir.c` 行号支撑，
实测用例固化在 `tests/test_pvir_rvv_shapes.c`（`make test-pvir-rvv-shapes` → 40 checks）。
**执行状态（本轮）**：批次 B/C/D/E 已落地：G1–G6 完成、G7 部分（FP 运算可带舍入；sNaN 比较仍属契约）、G10 的 `vid` 完成；
SVE 侧回归 `test-pvir` 仍为 222 checks 不变，`test-sve-pvir` 347 checks 不变。

---

## 0. 结论速览

| # | 缺口 | 现状（证据） | 注入方式 | 对 SVE 影响 | 优先级 |
| --- | --- | --- | --- | --- | --- |
| G1 | 收窄/扩展（`vnsrl/vnsra/vnclip`、`vzext/vsext`） | **已实现**：`TRUNCATE/EXTEND` 入 PVIR | 复用 `PvirVectorOperation`；校验要求整数、同 lane、位宽方向正确 | 零：新 op，旧路径未改 | ✅ 完成 |
| G2 | 取余（`vrem*`） | **已实现**：`REM` | 复用 `PvirVectorOperation`，仅整数 | 零 | ✅ 完成 |
| G3 | 元素级标量↔向量（`vmv.x.s/vmv.s.x`、`vfmv.f.s/s.f`） | **已实现**：`EXTRACT/INSERT` + `PvirLaneOperation` | 新 payload（lane 索引；INSERT 显式 `merge_value` 保留其余 lane） | 零：SVE 无逐元素搬运指令 | ✅ 完成 |
| G4 | 整数↔FP / FP↔FP 转换（`vfcvt/vfwcvt/vfncvt`） | **已实现**：`CONVERT` + `PvirConvertOperation`（源/目标类型 + 舍入） | 新 payload；要求至少一侧 FLOAT、lane 相同、`ACTIVE_LANES_ONLY` | 零：新 op；旧 FLOAT ADD/MUL 语义不变 | ✅ 完成 |
| G5 | MASK 类型字节访存（`vlm.v/vsm.v`） | **已实现**：LOAD/STORE 允许 MASK + 8-bit 内存元素（字节数 `ceil(lanes/8)`） | 校验器放宽 + `operation->type.kind == MASK` 分支；非 8 位仍拒绝 | 零：只放宽 MASK 组合 | ✅ 完成 |
| G6 | 定点/饱和 + `vxrm/vxsat`（`vaadd/vssrl/vnclip/vsmul`） | **已实现**：`FIXED` + `PvirFixedPointOperation`（kind、rounding、saturating、vxsat 状态） | 新 payload；饱和要求 `ARCH_STATE OTHER/UPDATE` + `sat_state.output_value` | 零 | ✅ 完成 |
| G7 | 动态 FP 舍入（`frm`）与 signaling-NaN 比较 | **部分实现**：`FP_ARITH`（ADD/SUB/MUL/DIV/SQRT + rounding） | 新 payload；旧 FLOAT ADD/MUL 保持默认舍入；sNaN 比较仍属 lowering 契约 | 零 | ✅ 部分 |
| G8 | 多目的（`vmadc/vmsbc`、segment load） | 一条指令只有一个 destination | **不注入**：helper，或未来专用 op | 零（不碰核心） | P1 |
| G9 | 跨 lane/归约/压缩（`vrgather/vslide/vcompress/vred*`） | 无 op | **不注入**：helper（P3 用 LSX/LASX） | 零 | P1→P3 |
| G10 | 索引类（`vid/viota/vcpop/vfirst/vmsbf`） | **`vid` 已实现**：`LANE_INDEX`；其余仍 helper | 新 payload `PvirIndexOperation{start, step}` | 零 | ✅ 部分 |

**已经证明"不是缺口"的**（探针通过，不要过度设计）：

| RVV 需求 | 现有 PVIR 表达 | 证据 |
| --- | --- | --- |
| `vmerge.vvm/vxm/vim` | `OR(vs2, 0)` + 谓词 `vl∧v0` + `MERGE(vs1)` | `test-pvir-rvv-shapes`: `vmerge copy idiom` |
| `vmerge` 的 tail（`vta` 任意） | 第二条 `OR(old_vd, 0)` + 谓词 `lane≥vl` + `MERGE(刚算出的 vd)` | `tail fixup idiom` |
| `vta/vma` 四种组合 | 单一 `MERGE`（undisturbed 是 agnostic 的合法实现） | RVV 规格 `sec-agnostic` NOTE |
| `vleff` | `NONFAULTING` + `FIRST_FAULT/UPDATE/CUMULATIVE_SUCCESS` | `vleff FIRST_FAULT` 用例 |
| 整寄存器 `vl<n>re/vs<n>r/vmv<n>r` | `BITS` 类型的 LOAD/STORE + `ARCH_STATE OTHER` READ/WRITE | `whole-register load` 用例 |
| `vstart`（算术重启） | 折进运行时 mask words（`vstart≤i<vl` 置 1） | 与 `vl` 同一机制 |
| `vmin/vmax/vminu/vmaxu` | `COMPARE` + `SELECT`，或新增 `MIN/MAX`（整数限定，已完成） | 与 `test_pvir` 的 compare/select 用例同形 |
| widening（`vwadd/vwmul`） | 每 operand 独立 `operand_types`、lane 相同 | `vwadd` 用例 |
| `vmseq` 等 mask 目的 | `PVIR_ELEMENT_MASK` + `MERGE(旧 v0)` | `vmseq` 用例 |

> **形状陷阱（实测发现）**：`PVIR_OP_COMPARE` 的**结果类型**是 MASK，但 `operand_types[0..1]` 必须显式写成
> **数据元素类型**（如 `INTEGER/32/256`）。若让 operand 继承 MASK 类型，`EQ/NE` 仍会通过校验（对任意类型都成立），
> 但 `LT/LE/GT/GE` 会被拒绝，且语义是"比较掩码位"而非"比较数据"。RVV 的每次 `vms*` 都必须显式给出数据 operand 类型。

---

## 1. 注入协议（保证 SVE 语义不变）

PVIR 同时被 SVE 前端使用。下列规则是**机械可检查**的：

1. **只追加枚举值**：`PvirOpcode` 新成员一律加在 `PVIR_OP_BROADCAST` 之后（`la-pvir.h:126-144`）。
   在 `tests/test_pvir_rvv_shapes.c` 中用 `_Static_assert(PVIR_OP_BROADCAST == 19, ...)` 冻结既有编号；
   任何人重排枚举都会编译失败。
2. **只追加 union 成员**：`PvirInstruction.operation` 的 union（`la-pvir.h:396-404`）可以加新 payload；
   **绝不改动** `PvirVectorOperation`/`PvirArchStateOperation`/`PvirPredicateGeneratorOperation` 的字段顺序或类型。
   既有 SVE 代码用指定初始化器，新增 payload 不会被误触。
3. **校验只增不改**：新 opcode 的校验写在 `vector_opcode_arity()`、`valid_operation_operands()` 的**新 case** 里；
   现有 case 一行不改。唯一允许的"改"是**放宽**：新组合（如 G5 的 MASK 字节访存）从"拒绝"变为"接受"，
   不会让任何旧描述从"接受"变"拒绝"或改变其含义。
4. **ISA 语义不进核心**：不增加 `is_rvv`/`is_sve` 之类的模式位。所有新 op 的语义由类型/操作数/新 payload 决定；
   RVV 专有的 `vstart/vxrm/vxsat` 通过 `state_id`（前端自管）与 lowering 契约表达。
5. **回归门**：每次注入后必须同时通过
   - `make test-pvir`（既有 222 checks，SVE 形状）；
   - `make test-pvir-rvv-shapes`（RVV 形状 + 新 op 正负例）；
   - 新增 op 的"非法形状必须拒绝"负例（防止校验器漏判）。
6. **文档同步**：`la-pvir.h` 的 opcode 注释写明"通用语义 + 该 op 允许的操作数/类型组合"；RVV 专有部分只写在 `RVV_PVIR*.md`。

---

## 2. 各缺口的建议形态

### G1 收窄/扩展（P0）

> 状态：已实现（`PVIR_OP_TRUNCATE/EXTEND`，整数限定，要求同 lane、位宽方向正确；符号扩展取 operand 的有符号性）。

- 需求：`vnsrl.wv/wx/wi`、`vnsra.*`、`vnclip(u).*`、`vzext.vf*`、`vsext.vf*`、`vfncvt`（FP 部分走 G4）。
- 现状：`vwadd` 这类"宽结果"已验证可写；"窄结果"目前只能写成 `SHR` 的结果类型窄于操作数，
  但校验器不定义"宽操作数 → 窄结果"的截断语义（它只检查结构）。
- 方案 A（推荐）：新增 `PVIR_OP_TRUNCATE` 与 `PVIR_OP_EXTEND`，复用 `PvirVectorOperation`：
  - `TRUNCATE`：`type.element_bits < operand_types[0].element_bits`，lane 数相同；
  - `EXTEND`：`operand_types[0].element_bits < type.element_bits`；符号由 `operand_types[0].signedness` 决定。
  - 校验插入点：`vector_opcode_arity()`（arity=1）+ `valid_operation_operands()` 新 case。
- 方案 B：不加 op，钉死"SHR 按操作数宽度移位、结果按结果类型截断"的 lowering 契约。
  代价：语义藏在后端，PVIR 描述不完全。
- 对 SVE：零影响；SVE 的 `SXTB/SXTH/UXTB/UXTH`/`SUNPK` 若将来走 PVIR 也可用同一 op。

### G2 REM（P1）

> 状态：已实现（`PVIR_OP_REM`，整数限定）。

新增 `PVIR_OP_REM`，复用 `PvirVectorOperation`（arity=2，语义与 DIV 同型，但按类型的有/无符号取余）。
校验与 DIV 同形；RVV 的除零/溢出特例属于 lowering 契约（与 DIV 一致）。

### G3 EXTRACT/INSERT（P1）

> 状态：已实现（`PVIR_OP_EXTRACT/INSERT` + `PvirLaneOperation`；INSERT 必须提供 `merge_value` 保留其余 lane，EXTRACT 必须不带）。

新增新 payload：

```c
typedef struct {
    PvirValueId source;      /* EXTRACT: vector; INSERT: scalar */
    PvirValueId destination; /* EXTRACT: scalar; INSERT: vector */
    PvirVectorType vector_type;
    uint16_t lane;           /* 立即数，< lanes */
} PvirLaneOperation;
```

- `EXTRACT` 对应 `vmv.x.s`/`vfmv.f.s`（lane=0 恒成立，但保留通用性）；
- `INSERT` 对应 `vmv.s.x`/`vfmv.s.f`，语义是"只写 lane，其余按 undisturbed 保持"，
  可用 predicate + `MERGE` 表达，`INSERT` 只负责把标量放入指定 lane。
- 对 SVE：零影响；SVE 的 `LASTB/LASTA` 等若需要也可复用。

### G4 CONVERT（P2）

> 状态：已实现（`PVIR_OP_CONVERT` + `PvirConvertOperation`；要求至少一侧 FLOAT、同 lane、`ACTIVE_LANES_ONLY`；整数位宽变化仍归 TRUNCATE/EXTEND）。

新增新 payload：

```c
typedef enum { PVIR_ROUND_DEFAULT, PVIR_ROUND_NE, PVIR_ROUND_RTZ,
               PVIR_ROUND_RDN, PVIR_ROUND_RUP, PVIR_ROUND_RMM,
               PVIR_ROUND_RNV } PvirRounding;
typedef struct {
    PvirValueId source;
    PvirValueId destination;
    PvirVectorType source_type;
    PvirVectorType destination_type;
    PvirRounding rounding;   /* 默认值表示"实现默认舍入"，旧路径不用此 payload */
} PvirConvertOperation;
```

覆盖 `vfcvt.*`（int↔fp、fp↔fp 宽度）、以及可作为 G1 的补充。校验：两种类型 lane 数相同；
舍入仅对 FP 侧有效；FP 目标要求 `fp_exceptions=ACTIVE_LANES_ONLY`（复用现有规则）。
对 SVE：零影响（新 op；SVE 的 `FCVT` 族可选择性迁移）。

### G5 MASK 字节访存（P1）

> 状态：已实现（LOAD/STORE 允许 `PVIR_ELEMENT_MASK` + `memory_element_bits=8`；非 8 位仍拒绝）。

现状：`LOAD/STORE` 的类型检查让 MASK（`element_bits=1`）不可能有 8-bit 内存元素（`pvir.c:904-916`）。
方案：在 LOAD/STORE 的类型检查里**增加** MASK 分支：

- `type.kind == PVIR_ELEMENT_MASK` 时要求 `memory_element_bits == 8`；
- 访问字节数 = `ceil(lanes/8)`，位序为 lane i → byte[i/8] 的第 `i%8` 位（与 RVV v0 布局一致）；
- `vector_bits` 与 `memory_element_bits` 的整除检查对该分支跳过。
这条是"放宽"，旧形状全部保持；新增负例：MASK + 16-bit 内存元素仍拒绝。

### G6 定点/饱和（P2）

> 状态：已实现（`PVIR_OP_FIXED` + `PvirFixedPointOperation`）：`AVG_ADD/AVG_SUB`、`SAT_ADD/SAT_SUB/SAT_MUL`、`SCALE_SHIFT`、`CLIP`；
> `rounding` 必须为 `RNU/NE/RDN/ROD`（AVG/SCALE/CLIP），SAT_* 必须为 `DEFAULT`；`saturating` 时要求
> `effects.arch_state` 为 `OTHER/UPDATE` 且 `sat_state.output_value` 给出 vxsat 输出。旧整数 ADD/SUB/MUL 语义未动。

新增 payload `PvirFixedPointOperation`：

```c
typedef enum { PVIR_FIXED_AVG_ADD, PVIR_FIXED_AVG_SUB, PVIR_FIXED_SAT_ADD,
               PVIR_FIXED_SAT_SUB, PVIR_FIXED_SAT_MUL, PVIR_FIXED_SHIFT_R,
               PVIR_FIXED_CLIP } PvirFixedPointKind;
```

字段：kind、type、predicate、inactive policy、destination、两个源、`PvirRounding rounding`（vxrm）、
`PvirArchStateEffect sat_state`（写 `vxsat`，domain OTHER）。只用于 `vaadd/vssrl/vnclip/vsmul` 等；
P1 先用 helper，等差分稳定再决定是否内联。

### G7 FP 舍入与 NaN 比较（P2）

> 状态：部分实现。`PVIR_OP_FP_ARITH`（`ADD/SUB/MUL/DIV/SQRT` + `PvirRounding`）已加入，要求 FLOAT 类型与
> `ACTIVE_LANES_ONLY`；**旧 `ADD/MUL`+FLOAT 仍表示默认舍入**，SVE 现有描述不受影响。signaling-NaN 比较的
> invalid 规则仍在 lowering 契约中，未进 PVIR（避免改动既有 COMPARE/effects）。

- 动态 `frm` 不能塞进现有 `PvirVectorOperation`（会改变 SVE 现有描述的含义），因此新增 payload（同 G4 的 `rounding` 字段）
  或独立的 `PVIR_OP_FP_ARITH`。
- 旧 `ADD/MUL` + FLOAT 类型保持"默认舍入"语义，SVE 现有描述不变。
- signaling-NaN 比较：`VMFLT/LE/GT/GE` 与 `VMFNE` 的 invalid 规则写进 lowering 契约；若需要 PVIR 描述，
  给 COMPARE 加 `bool signaling_nan_raises_invalid` 字段是不可接受的（改既有结构），应改用新 payload 或在
  `effects.fp_exceptions` 语义中细化（后者会影响 SVE，暂缓）。

### G8/G9/G10 明确不注入

- **多目的**：`vmadc/vmsbc` 同时产生向量和掩码；segment load 写多个寄存器。PVIR 的一指令一 destination 是其
  value-id 模型的基础；为它们引入多输出会波及 SVE（SVE 也没有这类指令）。用 helper。
- **跨 lane/归约**：属于微架构 shuffle 网络；SVE 与 RVV 的跨 lane 语义不同（ZIP/UZP vs vrgather/slide/compress），
  放进 PVIR 会把 ISA 语义耦合进核心。用 helper，P3 再以 LSX/LASX 内联。
- **索引类**：helper；`vid.v` 若将来频繁使用，可加独立的 `PVIR_OP_LANE_INDEX`（新 op，零影响）。

---

## 3. 分批补丁计划

| 批次 | 内容 | PVIR 改动 | 状态 |
| --- | --- | --- | --- |
| A（零风险） | 固化"不是缺口"的表达方式：vmerge/tail-fix/vleff/整寄存器/BITS 访存 | 无 | ✅ 已固化进测试 |
| B（只加枚举） | `TRUNCATE/EXTEND/REM/MIN/MAX/MOVE` | `PvirOpcode` 追加（20–25）+ arity/operand 新 case | ✅ 已完成 |
| C（新 payload） | `EXTRACT/INSERT`（`PvirLaneOperation`）、`CONVERT`（`PvirConvertOperation`） | union 追加成员 + 新 op 校验（26–28） | ✅ 已完成 |
| D（放宽校验） | MASK 类型 8-bit 访存 | LOAD/STORE 类型检查 + MASK 分支 | ✅ 已完成 |
| E | 定点/饱和 payload、FP 舍入 payload、`LANE_INDEX` | `PVIR_OP_FIXED`（29）、`PVIR_OP_FP_ARITH`（30）、`PVIR_OP_LANE_INDEX`（31） | ✅ 已完成 |

**批次 A 可以立即做**：不碰 `pvir.c`/`la-pvir.h`，只写契约与测试；它决定了"哪些 RVV 指令先能翻译"。

---

## 4. 风险与边界

1. **不要把"能通过校验"当成"语义已定"**：G1/G4/G6/G7 的新 payload 必须同时写 lowering 契约与差分用例。
2. **放宽校验（G5）要保持负例**：MASK + 非 8 位内存元素仍须拒绝；否则会掩盖错误的前端描述。
3. **枚举/结构 ABI**：PVIR 目前是源码内嵌（LATA/LATR 各自编译），没有二进制兼容负担；但仍应冻结编号，
   避免序列化/日志/测试中的数值假设失效。
4. **SVE 回归的唯一判据是测试**：每次注入后跑 `test-pvir` + `test-pvir-rvv-shapes`；新增 op 不得改变任何旧用例的
   期望结果（测试文件里不要为了新 op 修改旧期望）。
5. **多目的/跨 lane 的诱惑**：它们的收益在性能而非语义，等 P1 差分稳定后再评估 LSX/LASX 内联，不要提前扩 PVIR。
