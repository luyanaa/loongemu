# 四、P1：RVV 1.0 正确性与 PVIR 能力缺口

> 本文对应评审条目「四、P1：RVV 1.0 正确性与 PVIR 能力缺口」，只做**检查与定型**，不含代码改动。
> 证据基准：LATR 源码（本轮逐条核对，给出 `file:line`）与 QEMU-LVZ 9.0.50
> （`target/riscv/insn_trans/trans_rvv.c.inc`、`target/riscv/vector_helper.c`）。
> 相关文档：[`RVV_PVIR.md`](RVV_PVIR.md)（PVIR 映射研究）、[`RVV_PVIR_GAPS.md`](RVV_PVIR_GAPS.md)（PVIR 注入方案）、
> [`DESIGN.md`](DESIGN.md) §5、[`GAPS.md`](GAPS.md) P2「RVV 合规检查」。

---

## 0. 结论速览

| # | 结论 | 依据 |
| --- | --- | --- |
| 1 | **完整性缺口是 94 条编码**（不是"若干族"）：整数 36（carry/borrow 10、MAC 8、`vmulh*` 6、`vsmul` 2、`vrsub` 2、`vw*.w` 8）、whole-register 24、fault-only-first 4、FP 30（FMA 24、其他 6）。已登记 267 条执行指令 + 3 条 `vset*`。 | §2.1，`rv64gc-opcodes.def` 与 `insn32.decode:340-760` 逐名比对 |
| 2 | 原清单有两处需要修正：`vandn` 属 **Zvbb**，不属 RVV 1.0；漏列了 `vrsub.vx/vi`、`vmulh*`、`vsmul`、`vwadd.wv/wx` 一族（共 18 条）。 | §2.2 |
| 3 | **segment 不需要新解码入口**：QEMU 的 `vle8_v` 带 `nf` 字段，`vlseg<nf>e8.v` 就是同一 trans 的 `nf>1`。LATR 现在把 `nf>1` 显式报非法（`translate-riscv-rvv.c:506-510`），语义正确、只是没实现。 | §2.2 |
| 4 | **最严重的现存缺陷不是缺指令，而是缺合法性检查**：`rvv.c` 的 lane 寻址用 `(reg + idx) & 31`（`:120,:125,:150,:161`），非法寄存器组会**回绕到别的架构寄存器**，其中包含 `v0`（掩码）。 | §3.1 |
| 5 | **组合合法性检查必须在翻译期做，而且成本极低**：TB key 已含 `vtype/vill/vl`（`tb-cache.c:56`），翻译期 `env->vtype` 就是该 TB 的常量，可直接算出 EMUL/组大小。 | §3.4 |
| 6 | 另发现 **2 个真缺陷 + 4 条契约/边界问题**：向量 FP 全路径缺非法 `frm` 的 illegal（`rvv.c:1546` 等处丢弃 `latr_fp_set_rounding` 的返回值）、归约族缺 `vstart≠0` 门控；另有 `vlm.v` 的 `INACTIVE_ZERO` 契约漂移、masked load 策略表达不精确、`vmv.x.s` 的 `lanes` 兜底。 | §4 |
| 7 | **`misa.V` 的处置有一条几乎零成本的路**：把 profile=rv64gc 实现为「`misa` 去掉 V」+「`mstatus.VS` 读为 0、写忽略」，则现有全部 VS 门控（`emit_vs_gate`、向量 CSR）自动把 V 变成非法，无需逐指令加开关。 | §6.3 |

---

## 1. 现状盘点（代码事实，不含计划）

### 1.1 登记面

| 项 | 数值 | 证据 |
| --- | --- | --- |
| RVV 主解码区块条目 | 383 | `ir1/qemu/insn32.decode:340-760` |
| LATR 已登记向量 tag | 270（其中 3 条为 `vset*`，执行类 267） | `ir1/rv64gc-opcodes.def` |
| 未登记（RVV 1.0） | **94** | 逐名 `comm` 比对，见 §2.1 |
| 未登记（非 RVV，同区块混排） | 9 | `hfence_*`/`hlv_*`/`hsv_*`（H 扩展）、`wrs_*`（Zawrs） |

未登记的 trans 一律 `return false`（`gen_decode_stubs.py` 生成的 reject stub）→ `latr_decode()` 归为
`LATR_RV_ILLEGAL`（`ir1/decode-riscv.c:97-101`）。**这一点是好的**：缺指令不会误执行，只会 SIGILL。

### 1.2 三条执行路径

| 路径 | 位置 | 生效条件 |
| --- | --- | --- |
| 直接 lowering（Tier 1/2） | `translator/pvir-lower-lsx.c` | 有 LSX；`vill==0 && vstart==0 && 0<vl<=VLEN`；`vd+nregs<=32` 等（`:699-712,:745-753`） |
| PVIR 解释执行 | `translator/pvir-exec.c` | helper 内部构造 `PvirInstruction` 并解释 |
| C helper | `translator/rvv.c` | 其余全部 |

直接 lowering 的准入条件里**已经**包含了 `vd + nregs > 32` 的拒绝（`:745-753`），所以回绕问题只存在于 helper 路径。
换句话说：同一个非法编码，在有 LSX 的宿主上走 helper、在别的宿主上可能走 direct —— 两条路径的合法性判定并不统一。

### 1.3 测试现状

- 裸机 `latr-isa` 1337 checks、decode 26、`latr-diff` PASS（`GAPS.md:58-72`）。
- RVV 差分序列只有 **13 条编码**，且固定 `vsetivli 4 / e32,m1`、无掩码、无 `vstart`、无 `vl=0`
  （`tests/latr_isa_diff_vectors.h:44-56`）。
- 合法性负例：目前只有"`nf>1` 报非法"和"`VS=0` 报非法"两类。

**结论**：数值正确性有覆盖，**合规边界几乎零覆盖**。这正是 §3、§5 要补的。

---

## 2. 剩余指令族：94 条编码

### 2.1 全量缺口表

| 族 | 条数 | 代表编码 | 建议 | 关键语义陷阱 |
| --- | --- | --- | --- | --- |
| Carry/Borrow | 10 | `vadc.vvm/vxm/vim`、`vmadc.*`、`vsbc.vvm/vxm`、`vmsbc.*` | **helper** | `v0` 同时是掩码与进位输入；`vmadc` 是"比较"语义，`vadc` 是"写元素"语义，两者不能共用一个 helper |
| 整数 MAC | 8 | `vmacc/vnmsac/vmadd/vnmsub`（vv/vx） | helper → 后续融合 | `vmacc` = `vd += vs1*vs2`（**累加进 vd**），`vmadd` = `vd = vs1*vd+vs2`，两者 operand 角色不同，容易写反 |
| 高位乘 | 6 | `vmulh/vmulhu/vmulhsu`（vv/vx） | **可直接补** | 64×64→128 高位；SEW<64 时先按 SEW 符号扩展到 64 再乘，不能用宿主 128 位乘法直接截高位 |
| 定点乘 | 2 | `vsmul.vv/vx` | helper | 走 `vxrm` + `vxsat`，与 `vaadd` 同一条 `FIXED` 路径（`rvv.c:1236`） |
| 反向减 | 2 | `vrsub.vx/vi` | **可直接补** | 标量在前；`vi` 的立即数符号扩展与 `vadd.vi` 同规则（`translate-riscv-rvv.c:117` 已有 `imm_signed=0` 的先例，这里要 1） |
| wide `.w` 加减 | 8 | `vwadd.wv/wx`、`vwaddu.*`、`vwsub.*`、`vwsubu.*` | **可直接补** | `vs1` 是宽（2·SEW），`vs2` 是窄（SEW）；现有 `RVV_X_WV` 宏（`:213`）只用于 `vnsrl` 类收窄，语义方向相反，不能直接复用 |
| Whole-register | 24 | `vl1re8/16/32/64`、`vl2re*`、`vl4re*`、`vl8re*`（16）、`vs1r/2r/4r/8r`（4）、`vmv1r/2r/4r/8r`（4） | **优先补** | 只要求 `rd % LEN == 0`、`rs2 % LEN == 0`；长度恒为 `LEN*VLENB` 字节，**不看 `vl`**；`vstart` 按**元素**解释，字节偏移 `vstart*SEW/8`（QEMU `vector_helper.c:5140-5166`）；`vmv<n>r.v` 还要 `rd != rs2` |
| Fault-only-first | 4 | `vle8ff/16ff/32ff/64ff.v` | 优先补 | 成功时 `vl` 不变，失败时 `vl = 已成功元素数`；`vstart` 语义与普通 load 不同（故障元素之后不写、不报） |
| FP FMA | 24 | `vfmacc/vfmsac/vfnmacc/vfnmsac/vfmadd/vfmsub/vfnmadd/vfnmsub`（vv/vf，16）+ `vfw{m,n}macc/msac`（vv/vf，8） | SoftFloat `muladd` | 必须**单次舍入**；`vn*` 的取负作用在乘积上而非结果上；`.vf` 需广播 |
| FP 其他 | 6 | `vfrsub.vf`、`vfrdiv.vf`、`vfrec7.v`、`vfrsqrt7.v`、`vfslide1up/down.vf` | 前 4 条 helper；slide 归入排列族 | `vfrec7/vfrsqrt7` 是**近似**指令（7 bit 精度、无 fflags），不能用 `1/x` 精确除法实现，否则差分对不上 |

合计 **94**。

### 2.2 对原清单的三处修正

1. **`vandn` 不属于 RVV 1.0**：它和 `vrol/vror/vbrev/vbrev8/vclz/vctz/vclmul` 一起属 **Zvbb/Zvkb**，
   在本仓库的 `insn32.decode:960-970` 单独一段。把它们算进"RVV 1.0 完整性"会让 100% 的目标不可达。
   建议单列「Zvbb 可选」一节，不与 P1 混算。
2. **漏列 18 条**：`vrsub`(2)、`vmulh*`(6)、`vsmul`(2)、`vw*.w`(8)。其中 `vwadd.wv` 一族是"宽累加"的
   主力形态（`vwmacc` 的实现也要先有 `vwadd.w` 的语义），不该被跳过。`vsmul` 在 `RVV_PVIR.md` §6
   G6 里被列为"已注入 `FIXED`"，但编码侧没有登记 —— 文档与实现对不上，本轮一并记录。
3. **Segment 不是独立解码入口**：`vlseg<nf>e8.v` 由 `vle8_v` 的 `nf` 字段承载。所以 segment 的工作量
   不是"加 33 条解码"，而是"让 `latr_rvv_mem` 接受 `nf>1` + 处理 `EMUL*NF<=8` + 多寄存器目的"。
   同理 `vsseg`。当前 `translate-riscv-rvv.c:506-510` 的显式拒绝应保留到实现完成的那一刻。

### 2.3 优先级：同意结论，调整顺序

原建议「whole-register → segment → fault-only-first」。按**依赖关系**排，更省事的顺序是：

1. **whole-register（24 条）**：不依赖 `vl`/掩码/`nf`，只需对齐检查 + 字节块拷贝，且能立刻解决
   "上下文保存/恢复"的真实需求（Linux 的 `vstate` 切换就用 `vl1re8.v` + `vs1r.v`）。
   顺带把 §3 的对齐检查机制建起来（它的规则最简单，正好做第一个消费者）。
2. **fault-only-first（4 条）**：它改的是 `vl` 这个**状态**。`vl` 在 TB key 里（`vspec` bit16-23），
   所以 `vleff` 必须结束 TB，否则后续指令仍按旧 `vl` 翻译 —— 这是设计约束，先做掉比后做便宜。
3. **segment（`nf>1`）**：它同时引入"多寄存器目的"和"`EMUL*NF<=8`"两条新规则，是三者中最重的。
   放在已有一套合法性检查之后再动。
4. Carry/Borrow 与 FP FMA 可以与上面并行：它们只改 `rvv.c` 的 helper，不碰解码/状态机。

### 2.4 实现形态（与现有约定对齐）

现有约定是「`env->rvv_args[]` 传参 + `emit_rvv_call` 单参数调用」（`translate-riscv-rvv.c:50-74`）。
新族沿用，不引入第二种调用约定：

```c
/* whole-register: rd/rs2 已在翻译期做过对齐检查，这里只信检查结果 */
void latr_rvv_whole_reg(CPUArchState *env);   /* args: {op, reg, is_load, len} */
/* fault-only-first: 返回 0 或 cause；成功时按 CUMULATIVE_SUCCESS 写 vl */
uint64_t latr_rvv_mem_ff(CPUArchState *env);  /* args: {base, vd, eew, mode} */
/* segment: nf 由 args 传入，目的组 = vd + i*EMUL*VLENB */
uint64_t latr_rvv_seg_mem(CPUArchState *env); /* args: {base, vd, eew, is_load, vm, nf} */
/* carry/borrow: v0 既是掩码又是进位输入，必须单独 helper */
void latr_rvv_adc(CPUArchState *env);         /* args: {op, vd, vs2, vs1, kind, vm} */
/* FP FMA: 复用软浮点 muladd，rn* 通过 FMA 的取负合成 */
void latr_rvv_fp_fma(CPUArchState *env);      /* args: {op, vd, vs2, vs1, kind, vm, wide} */
```

**本批不走 PVIR 的理由**（与 `RVV_PVIR_GAPS.md` G8/G9 一致）：carry 需要"`v0` 同时作谓词与数据"、
segment 需要多目的、FMA 需要"单次舍入"的显式保证 —— 三者都会把 ISA 语义推进 PVIR 核心。
whole-register 是例外：`BITS` 类型 + `ARCH_STATE` 的形态已在 `test-pvir-rvv-shapes` 的
`whole-register load` 用例里验证过，可以走 PVIR。

---

## 3. 组合合法性检查（比继续加指令更重要）

### 3.1 现状：`& 31` 回绕 = 静默错执行

`latr_rvv_read_elem` / `latr_rvv_write_elem` 的寻址是：

```c
per = RVV_VLEN / sew;          /* 每寄存器元素数 */
idx = lane / per;              /* 组内第几个寄存器 */
r   = (reg + idx) & 31u;       /* rvv.c:125, :161 */
```

`& 31` 的用意是"LMUL 组循环使用寄存器"，但它把**非法编码**变成了**合法但错误**的执行。
具体例子：`vadd.vv v31, v0, v1`，`SEW=64, LMUL=8`（`vlmax=16`，每寄存器 2 lane）：

| lane | 期望（规范：非法指令） | 实际（LATR） |
| --- | --- | --- |
| 0–1 | — | 写入 `v31` |
| 2–3 | — | 写入 `v0`（**掩码寄存器**） |
| … | — | … |
| 14–15 | — | 写入 `v6` |

`v0` 被覆盖后，**后续所有掩码向量指令的活动 lane 全错**。这不是"结果不对"，是"故障扩散"。

需要区分两种 `& 31`，不要一起改：

- **真回绕**：只有 `read_elem`/`write_elem` 里的 `(reg + idx) & 31`（`:120,125,150,161`）。
  它们是**所有按 lane 寻址路径的共同入口**（ALU/FP/访存/扩缩窄都经过），所以修这一处就覆盖了绝大部分。
- **无效掩码**：其它 `env->vreg[vd & 31u]`（如 `:421,714,761,812,1090,1638,1850`）里 `vd` 来自 5 bit
  解码字段，本来就在 0..31，`& 31` 是恒等操作、不产生回绕。但它们同样**默认"vd 已合法"**，
  合法性检查缺失时它们只是"没错得很明显"，不是"正确"。

QEMU 的做法是在翻译期就直接拒绝：`require_align()`（`trans_rvv.c.inc:119`）+
`require_nf()`（`:109`）在 `vext_check_store/load`（`:251-273`）里被调用，非法编码走
`return false` → `RISCV_EXCP_ILLEGAL_INST`。

### 3.2 需要检查的规则全集

以下逐条转录自 QEMU（行号为 `trans_rvv.c.inc`），并标注规范条款。这是"应当实现什么"的权威清单。

| 规则 | 语义 | 适用 | QEMU |
| --- | --- | --- | --- |
| `require_align(r, emul)` | `emul<=0`（分数 LMUL）时任意；否则 `r % 2^emul == 0` | 所有向量源/目的 | `:119-122` |
| `require_nf(vd, nf, emul)` | `nf << max(emul,0) <= 8` 且 `vd + 该值 <= 32` | 访存（含 segment） | `:109-113` |
| `require_vm(vm, vd)` | `vm==0` 时 `vd != 0` | 除 mask 目的指令外全部 | `:104-107` |
| `require_noover(dst,dl,src,sl)` | §5.2 三条：同 EEW 可重叠；目标 EEW 更小且重叠在源组低位；目标 EEW 更大、源 EMUL≥1 且重叠在目标组高位 | widening/narrowing 与 indexed | `:143-171` |
| `vext_check_store(s,vd,nf,eew)` | `-3 <= EMUL <= 3`（EMUL = EEW-SEW+LMUL），且 align/nf | `vse*`、`vsse*`、`vsxei*`、`vsseg*` | `:251-258` |
| `vext_check_load(s,vd,nf,vm,eew)` | store 的规则 + `require_vm` | `vle*`、`vlse*`、`vlxei*`、`vlseg*` | `:270-273` |
| `vext_check_st_index(s,vd,vs2,nf,eew)` | EMUL 范围 + `vs2` 按 **EMUL** 对齐 + `vd` 按 **LMUL** 对齐 + `nf` 按 LMUL | indexed store | `:289-306` |
| `vext_check_ld_index(...)` | 上者 + `require_vm` + 每个 segment 组与 `vs2` 的重叠规则（`nf>1` 时禁止任何重叠） | indexed load | `:326-352` |
| `vext_check_ss/sss` | `vd`、`vs2`、`vs1` 均按 LMUL 对齐 + `require_vm` | 普通 elementwise | `:359-382` |
| `vext_check_ms/mss` | 源按 LMUL 对齐；`vd != vs` 时 `require_noover(vd, lmul=0, ...)`；**不检查 `require_vm`** | 比较、mask 目的 | `:385-416` |
| `vext_check_reduction` | `vs2` 对齐 **且 `vstart == 0`** | 所有 `vred*/vfred*/vfwred*` | `:563-566` |
| whole-register | `rd % LEN == 0`、`rs2 % LEN == 0`、`rd != rs2`（`vmv<n>r.v`） | `vl<n>re`、`vs<n>r`、`vmv<n>r` | `:3565-3580` |
| mask 指令附加 | `vmsbf/vmsif/vmsof`：`rd != rs2`；`viota`：`rd` 组与 `rs2` 单寄存器不重叠；`vcompress`：`rd != rs2` 且 `rd` 组与掩码 `rs1` 不重叠 | | `:3055-3105,3538-3543` |

**两个容易做错的地方**：

- `require_vm` 对**比较/mask 目的**不适用（规范 §5.3 的例外："目标被写入掩码值时允许与 `v0` 重叠"）。
  `vms*`、`vmfeq*`、`vmadc` 都是 `vext_check_mss`（无 `require_vm`）。
- indexed 的 `vs2` 是**索引**，其对齐按 **EMUL（EEE-relative）** 而不是 LMUL。`vluxei64.v` 在
  `SEW=8, LMUL=1` 时 EMUL=8，索引组必须 8 对齐；按 LMUL 检查会漏判。

### 3.3 API 设计

现有代码里没有 `LatrVectorState` 类型，向量状态是 `CPUArchState` 的散字段（`riscv-env.h:113-129`）。
按你给的形状落地，建议补一个只读视图，保持签名不变：

```c
/* include/riscv-env.h */
typedef struct LatrVectorState {
    uint32_t vtype;    /* 不含 vill 位 */
    uint32_t vill;
    uint64_t vl;
    uint64_t vstart;
} LatrVectorState;

static inline LatrVectorState latr_vector_state(const CPUArchState *env)
{
    LatrVectorState st = { (uint32_t)env->vtype, (uint32_t)env->vill,
                           env->vl, env->vstart };
    return st;
}
```

```c
/* include/latr.h 或 translate.h —— 纯函数，可在裸机单测里直接调用 */
typedef enum {
    LATR_RVV_LEGAL = 0,
    LATR_RVV_ILLEGAL_EMUL,      /* EMUL 越界 / EMUL*NF > 8 */
    LATR_RVV_ILLEGAL_ALIGN,     /* 组起点未按 EMUL 对齐 */
    LATR_RVV_ILLEGAL_RANGE,     /* 组越过 v31 */
    LATR_RVV_ILLEGAL_OVERLAP,   /* 违反 §5.2 重叠规则 */
    LATR_RVV_ILLEGAL_MASK,      /* vm=0 且 vd 组覆盖 v0 */
    LATR_RVV_ILLEGAL_VSTART,    /* 该指令要求 vstart==0 */
} LatrRvvLegality;

LatrRvvLegality latr_rvv_check_operands(const RiscvIR1 *insn,
                                        const LatrVectorState *state);
```

实现要点：

1. **输入**：`insn->opcode` 决定族（复用 `translate-riscv-rvv.c` 里已有的
   `rvv_alu_op/rvv_cmp_op/rvv_mem_op/...` 判定，注意这些函数是 `static`，需要抽到共享位置或加一个
   `enum LatrRvvClass`）；`insn->arg.rd/rs1/rs2/nf/vm` 是寄存器号。**不要先 `& 31`** —— 正是这一步
   要在检查里区分"编号 32 以上"与"编号 31 但组越界"。
2. **EMUL 计算**：`emul = eew_log2 - vsew + lmul`，用 int8 保持一致（QEMU 就是 `int8_t`）。
3. **`vill` 的处理**：`vill=1` 时不在这里判（它是运行时状态，编译期不可知）；返回 `LEGAL`，
   由 `emit_vs_gate`（`translate-riscv-rvv.c:17-33`）在运行期 trap。若把 `vill` 也塞进编译期判定，
   会与 TB 特化冲突（`vspec` 里已经有 vill，但 `vsetvl` 之后才变）。
4. **`vstart` 的处理**：`state->vstart` 在翻译期不可知（运行时 CSR），所以 `LATR_RVV_ILLEGAL_VSTART`
   **不能**由这个函数返回，只能由发射端生成运行期比较（沿用 `emit_vstart_zero_gate`
   `:35-48` 的形态）。签名保持你的形状，但要在注释里写清"`vstart` 规则不在本函数内"，
   否则后续维护者会误以为已经覆盖。

### 3.4 调用点与 TB 特化的关系

TB key 已经包含 `(vtype & 0x3ff) | vill<<10 | (vl & 0xff)<<16`（`tb-cache.c:56`），
所以**翻译期 `env->vtype` 就是该 TB 的常量**（`pvir-lower-lsx.c:705` 已经在这么用）。
因此在 `translate_vector_op()` 的入口处调用一次即可，无需每条指令重复：

```c
/* translate-riscv-rvv.c, translate_vector_op() 内，emit_vs_gate() 之后 */
{
    LatrVectorState st = latr_vector_state(env);
    if (latr_rvv_check_operands(ir1, &st) != LATR_RVV_LEGAL) {
        latr_raise_illegal_instruction(ir1);
        return true;
    }
}
```

**代价**：TBs 增加（同一编码在不同 vtype 下可能一个合法一个非法）。可接受——`vtype` 本来就在 key 里，
命中率不变；只是"非法编码"会按 vtype 各生成一个 trap TB，可用一个统一的
`illegal-vector` TB 收敛（`pc` 相同时 key 不同，通常只有几种 vtype，绝对数量很小）。

**必须同时做的一件事**：把这条检查**放在 direct lowering 之前**。现在 direct 路径自己有一套
`vd+nregs<=32` 的拒绝逻辑（`pvir-lower-lsx.c:745-753`），与 helper 路径的 `& 31` 回绕**判定不一致**：
同一个非法编码，能走 direct 的宿主报错/回退，不能走的宿主静默算错。统一到 `latr_rvv_check_operands`
之后，两条路径的合法性才有唯一答案。

### 3.5 执行器侧的第二道防线

即使翻译期检查到位，`latr_rvv_read_elem/write_elem` 也不该再有 `& 31`：

```c
/* rvv.c:105 起，改成 */
r = reg + idx;                        /* 不取模 */
if (r >= 32u) {                       /* 只可能来自未检查的路径 */
    env->mem_fault_cause = 0;
    env->pvir_status = PVIR_STATUS_INVALID;   /* 或专门的 rvv_illegal 标志 */
    return 0;
}
```

理由：**回绕是"静默错"，越界是"显式错"**。前者会在 guest 里表现为随机的掩码错乱，
后者至少在 `latr-isa` 里能被断言抓住。裸机测试可以直接构造"`vd=31, LMUL=8`"的 helper 调用来锁住这条。

### 3.6 负例矩阵（必须报 illegal 的编码清单）

| 类别 | 具体编码 | 期望 |
| --- | --- | --- |
| 组起点未对齐 | `vadd.vv v1, v2, v3`（LMUL=2）；`vadd.vv v4,v8,v2`（LMUL=8） | illegal |
| 组越界（不换行） | LMUL=8 且 `vd=28`；LMUL=4 且 `vd=30` | illegal（不是回绕） |
| mask 冲突 | `vadd.vv v0, v1, v2, v0.t`（vm=0 且 vd=v0） | illegal |
| mask 例外（应合法） | `vmseq.vv v0, v1, v2, v0.t` | **合法**（`vext_check_mss`） |
| 分数 LMUL 免对齐 | LMUL=1/2 时 `vadd.vv v1, v0, v3` | 合法 |
| indexed 索引组 | `vluxei64.v`（SEW=8,LMUL=1）`vs2=v1` | illegal（EMUL=8 → 需 8 对齐） |
| `nf` 越界 | `vlseg8e8.v` 且 LMUL=2（`8<<1=16`，`vd+16>32`） | illegal |
| `vstart≠0` + 归约 | `vstart=1; vredsum.vs` | illegal（本轮缺失，见 C2） |
| `vstart≠0` + `vcpop` | 已实现（`translate-riscv-rvv.c:511-519`） | illegal ✓ |
| whole-register 未对齐 | `vl2re32.v v1` | illegal（实现后） |

---

## 4. 已实现指令的正确性缺陷（本轮新发现）

以下每一条都是"已登记、能跑、结果可能错"的问题，优先级高于补新指令。

### C1 向量 FP 全路径缺少非法 `frm` 的 illegal —— **高**

先纠正一个容易搞错的前提：**RVV 的 `vfcvt/vfwcvt/vfncvt` 没有 `rm` 编码字段**
（`insn32.decode:649-654` 是 `@r2_vm`，`rtz` 变体是独立的 funct6：`00110/00111`），
舍入一律来自 `frm` CSR。所以这里不是"丢弃 `rm`"，而是"**没有校验 `frm`**"。

- 证据：`latr_fp_status_begin(env, &status, 0)`（`rvv.c:944,1546,1707`、
  `pvir-exec.c:732,761,890,965`）内部调 `latr_fp_set_rounding(status, env, 7)`，
  后者对 `frm∈{5,6,7}` **返回 false**（`translate-riscv-fp-helper.c:64-66`），
  而**所有向量调用点都丢弃返回值**。
- golden 行为：QEMU 的 `helper_set_rounding_mode` 在 `default:` 直接
  `riscv_raise_exception(ILLEGAL_INST)`（`fpu_helper.c:54-81`）；转换族用的
  `helper_set_rounding_mode_chkfrm` 还会额外"`frm >= 5` 就 illegal"（`:88-91`）。
  即 **QEMU 对 `vfadd` 与 `vfcvt` 都会 trap**。
- 对比：标量路径已实现这个检查 —— `translate-riscv-fp.c:1188-1204` 生成
  "`frm < 5` 否则 illegal" 的序列。向量侧完全没有。
- 影响：guest 写入 `frm=5` 后执行 `vfadd.vv`/`vfcvt.x.f.v`，规范与 golden 都要求
  illegal instruction；LATR 不报错，且 `float_status` 的舍入模式**根本没被赋值**
  （`latr_rvv_fp`/`latr_rvv_reduce`/宽 FP 路径里的 `float_status status;` 都是未初始化的栈变量，`rvv.c:937,1517,1698`）——
  于是按栈上残值算，结果**不可复现**。既不确定、又不报错，是最坏的一类。
- 修法（两选一，建议后者）：
  1. `latr_fp_status_begin` 改成返回 `bool`，各向量 helper 入口判 `false` 则置 illegal；
  2. 翻译期生成一次 `frm < 5` 的门控（复用 `emit_vs_gate` 的形状），运行期只比较一个 CSR 字段。
  方案 2 更省，且与"翻译期做合法性"的总体方向一致；注意这个门控必须覆盖**所有**用到 `frm`
  的向量指令（算术/转换/归约/`vfmv.f.s` 等），遗漏一条就退化成本条缺陷。
- 测试：`M10`（`frm=5/6 × vfadd/vfcvt` 期望 illegal）+ `frm=4`(RMM) 的数值差分。

### C2 归约族缺 `vstart≠0` 门控 —— **中**

- 证据：`translate-riscv-rvv.c:511-519` 的 `emit_vstart_zero_gate` 只覆盖
  `vcpop/vfirst/vmsbf/vmsif/vmsof/viota/vcompress`。**归约不在其中**，而
  `latr_rvv_reduce`（`rvv.c:916-1044`）从 `vstart` 开始累加。
- 规范/QEMU：`vext_check_reduction`（`trans_rvv.c.inc:563-566`）要求 `vstart_eq_zero`，
  否则 `return false` → illegal。
- 影响：`vstart=1` 时 LATR 给出"从第 1 个元素开始的和"，QEMU 报 illegal。差分测试若覆盖
  `vstart` 就会对不上；更实际的是：**只有真的从 trap 里恢复时才会遇到非零 `vstart`**，
  所以这条是"trap 恢复路径"的隐患，不是日常路径。
- 修法：把 `vred*/vfred*/vfwred*` 加进同一个门控列表。1 行。

### C3 `vlm.v` 使用 `PVIR_INACTIVE_ZERO` —— **低（契约漂移）**

- 证据：`rvv.c:555-556`，`is_mask && is_load` 时 `inactive_policy = PVIR_INACTIVE_ZERO`。
- 与文档冲突：`DESIGN.md` §5.4 明确"RVV 不能使用 `ZERO`，agnostic 只能保持或写全 1"，
  并要求"在 bridge 的断言里锁死"。
- 当前**无可见影响**：`vlm.v` 的 predicate 是 `all_true(lanes=vl)`，不存在 inactive lane，
  策略字段不会被消费。但它是"文档说不许、代码在做"的例子，将来若给 `vlm.v` 加 `vma` 尾
  处理就会变成真 bug。
- 修法：改成 `PVIR_INACTIVE_UNKNOWN`（或实现 `vta` 时改 `MERGE`），并加一条 assert。

### C4 masked load 的 inactive 策略表达不精确 —— **低（结果正确）**

- 证据：`rvv.c:555-556` 对非 mask 的 load 一律 `PVIR_INACTIVE_UNKNOWN`，然后在
  `:576-590` 手工把 `vma=1` 的 masked-off lane 写全 1。
- 语义上：`vma=0` 时应为 `MERGE(旧 vd)`，`vma=1` 时为 agnostic（`UNKNOWN` 合法）。
  现在一律 `UNKNOWN`，正确性依赖"executor 对 UNKNOWN 的实现恰好是不写"。
- 这是**隐式契约**：一旦 `pvir-exec` 的 UNKNOWN 改成"写零"（也符合 PVIR 的 agnostic 定义），
  这里立刻错。建议按 `vma` 分开写，或至少在注释里钉死"UNKNOWN ⇒ 不写"。

### C5 mask 目的指令的 `vta` 尾 —— **低**

- `latr_rvv_cmp`（`rvv.c:417-423`）只处理 `vma` 的 masked-off 位，尾部（`[vl, vlmax)`）
  保持原值。这是**合法**的（`vta=1` 时保留也允许），但与 QEMU 默认一致这一点没有测试锁定。
- 建议：在组合矩阵里显式加一条"`vta=1` 的 `vmseq` 尾部"用例，把"保持"变成受测行为。

### C6 `vmv.x.s` 的 `lanes` 兜底 —— **低**

- 证据：`rvv.c:2029`，`lanes = env->vl ? env->vl : 1u`。
- 语义上 `vmv.x.s` 忽略 `vl`，读 element 0 恒成立，所以结果是对的；但构造了一个
  `lanes=1` 的 PVIR 描述去读一个"当前 vl=0"的向量，绕开了 PVIR 的 0-lane 拒绝。
- 建议：改成直接 `env->gpr[rd] = sign_extend(read_elem(vs2, 0, SEW))`，不走 PVIR；
  既去掉特例，也省一次解释执行。同理 `vfmv.f.s`。

---

## 5. 组合状态矩阵测试

现在的差分是"13 条编码 × 固定 `vl=4` × 无掩码"。合规边界要按**组合**测，而不是按数值测。

### 5.1 维度

| 维度 | 取值 | 说明 |
| --- | --- | --- |
| `SEW` | 8 / 16 / 32 / 64 | 与 ELEN=64 交叉 |
| `LMUL` | 1/8, 1/4, 1/2, 1, 2, 4, 8 | 分数 LMUL 只允许 `SEW*LMUL <= VLEN` |
| `vl` | 0, 1, vlmax-1, vlmax | `vl=0` 是"整条指令不写"的分支 |
| `vstart` | 0, 1, vl-1, vl, >vl | `>vl` 与 `==vl` 都要求"清零并返回" |
| `vta`/`vma` | 4 种组合 | 只能用"保持"作为 golden（QEMU 默认 `false`） |
| 掩码 | 无 / 全 0 / 全 1 / 交替 / 仅边界位 | |
| `vxrm` | 0..3 | 只对定点族 |
| `frm` | 0..4, 7(DYN), **5/6（保留）** | 5/6 期望 illegal |
| `vill` | 0 / 1 | 1 时所有向量指令 illegal |

### 5.2 观测点（缺一不可）

`vd` 全部 lane（含 tail 与 masked-off）、`vl`、`vtype`、`vstart`、`vxsat`、`fcsr.fflags`、
`mcause/mtval/mepc`、以及**内存是否被改写**（masked-off load 不得触碰内存 —— 
`PVIR_INACTIVE_MEMORY_NO_TRANSACTION` 的验证点）。

### 5.3 必测组合（覆盖"状态 × 边界"，不追求笛卡尔积）

| # | 用例 | 断言 |
| --- | --- | --- |
| M1 | `vl=0`，各 `vta/vma` | 目标寄存器逐字节不变；`vstart=0`；无异常 |
| M2 | `vstart=vl`，执行 `vadd`/`vle` | `vstart` 清 0；无写回；无访存 |
| M3 | `vstart=1`，`vle32.v` 在 lane 2 故障 | `vstart=2`、`mtval=元素地址`、`vl` 不变、`vd[0..1]` 已写 |
| M4 | `vstart=1`，`vredsum`/`vcpop`/`vcompress` | illegal instruction |
| M5 | `vill=1`，`vadd`/`vle`/`vsetvli` | illegal；`vl`/`vtype` 不被改写 |
| M6 | `vta=1,vma=1` 与 `vta=0,vma=0` 同输入 | **结果完全一致**（证明"保持"实现） |
| M7 | masked load，mask 全 0 | 内存无读；`vd` 按 `vma` 不变 |
| M8 | 分数 LMUL（1/8..1/2）× SEW ≤ VLEN | 只有前 `VLEN*LMUL` 位被碰；相邻寄存器不变 |
| M9 | `vxrm` 0..3 × `vaadd/vssra/vnclip` | `vxsat` 与舍入逐个对齐 QEMU |
| M10 | `frm=5/6` × `vfadd/vfcvt` | illegal（C1 的验收点） |
| M11 | `frm=4`(RMM) × `vfcvt.x.f.v` | 与 QEMU 一致（RMM 在向量里也必须支持） |
| M12 | `nf>1` 的 `vlseg2e32.v` | 当前 illegal；实现后按 `EMUL*NF<=8` 判定 |

**golden 只能是 QEMU-LVZ 9.0.50**（`-cpu rv64,v=true,vlen=128,elen=64`，
`rvv_ta_all_1s=false`/`rvv_ma_all_1s=false` 默认），不得混用其他版本 —— 与 `DESIGN.md` §8.3 一致。

### 5.4 落地形态

现有 `tests/latr_isa_qemu_ref.S` + `latr_isa_diff_vectors.h` 的机制够用，但需要两处增强：

1. **负例需要"期望异常"的编码槽**：现在 `.word XXX` 只能表达"执行并比较数值"。建议加一个
   `.word LATR_DIFF_EXPECT_ILLEGAL` 的宏约定（在 `latr_isa_test.c` 侧捕获 `env.exception`）。
2. **`vstart` 需要可编程**：`vsetvl` 之后写 `csr vstart` 已在 CSR 路径支持（`translate-riscv-csr.c`
   的 `LATR_CSR_VSTART`），测试里直接 `csrw vstart, t0` 即可，不需要新机制。

---

## 6. CPU profile：`rv64gc` 与 `rv64gcv-experimental`

### 6.1 现状

| 事实 | 证据 |
| --- | --- |
| `misa` 读值是编译期常量，含 V(bit21)：`0x8000_0000_0020_112d` | `translate-riscv-csr.c:99,344-345` |
| `misa` 只读，写入被丢弃 | `:222`（`csr_is_read_only`）、`:524-528` |
| 没有任何 profile / 扩展可用性概念 | 全仓库无 `profile`/`has_v`/`ext_v` 字段 |
| 向量指令唯一的门控是 `mstatus.VS != 0` 与 `vill` | `translate-riscv-rvv.c:17-33` |
| `mstatus.VS` 可写（`sstatus` 亦然） | `:86-91` 的 `SSTATUS_WRITE_MASK` 含 `MSTATUS_VS` |

### 6.2 为什么必须分开

`misa.V=1` 等于向 guest 宣告"完整 RVV 1.0"。后果不是"有些指令不可用"，而是：

1. Linux 侧：见到 V → 初始化向量上下文（`vlenb` 读得到 16 → `vsize=16`）→ 允许内核/用户态
   使用向量 → 命中 94 条未实现编码时 **SIGILL**。这类失败出现在启动期（内核 `kernel-mode
   vector`、`memcpy` 的向量实现），表现为"随机位置崩"，非常难归因。
2. 更糟的是 §3.1 的回绕：`vl1re8.v`/`vs1r.v` 正是上下文保存用的指令，**恰好是缺的那 24 条**；
   而 guest 若用 LMUL=8 的 `vadd` 且编码非法（编译器不会生成，但手写/恶意代码会），
   回绕会静默污染 `v0`。
3. 结论：**"宣称 V 但不完整"比"不宣称 V"危险得多**。缺指令应该是 SIGILL，不该是静默错。

### 6.3 设计

```c
/* include/riscv-env.h */
typedef enum {
    LATR_CPU_RV64GC = 0,             /* 稳定：不宣称 V */
    LATR_CPU_RV64GCV_EXPERIMENTAL,   /* 开发验证：宣称 V，明确不保证完整 */
} LatrCpuProfile;
```

放在 `CPUArchState`（而不是 `LatrSystem`），因为翻译器需要按 TB 读它；`LatrSystem` 里
`CPUArchState env` 是内嵌的（`latr-system.h:27`），所以板级初始化时设一次即可
（`latr_system_init`）。

**关键洞察：门控几乎全部可以复用现有机制。**

| 门控点 | 实现 | 是否需要新代码 |
| --- | --- | --- |
| `misa` 读 | `profile==RV64GC` 时用 `MISA_RV64GC_NO_V`（去掉 bit21） | 改 `:99` 一处常量选择 |
| `mstatus.VS` 读 | `RV64GC` 时读为 0 | 读路径加一次掩码 |
| `mstatus.VS` 写 | `RV64GC` 时写被忽略（RAZ/WI） | 写掩码按 profile 去掉 `MSTATUS_VS` |
| 向量指令 | 已有 `emit_vs_gate`：`VS==0` → illegal | **零改动** |
| 向量 CSR（`vstart/vxrm/vxsat/vcsr/vl/vtype/vlenb`） | 已按 `VS==0` → illegal 实现 | **零改动** |
| `vsetvli/vsetvl/vsetivli` | 已走 `emit_vs_gate` | **零改动** |

即：**把 `VS` 变成"永远为 0 且不可写"，整条 V 扩展就自动非法了**，不需要在 267 条指令上各加一个开关。
这是本轮最省事、也最难写错的方案。

**仍然需要单独处理的 3 处**：

1. `misa` 常量本身（否则 guest 仍看到 V=1 却执行不了 → 自相矛盾的镜像）。
2. `vlenb`：V 不存在时规范未定义该 CSR，应 illegal。上面靠 VS=0 已自动满足，但如果将来允许
   `VS` 可写而 V 不可用，就会漏；建议在 CSR 路径显式再判一次 profile。
3. 启动期断言：`profile==RV64GC` 时 `env->vtype/vl/vstart` 必须为 0，否则说明有路径绕过了门控。

**默认值**：`LATR_CPU_RV64GC`。实验 profile 必须显式打开（板级参数 / 测试宏），
并且**差分测试固定使用 experimental**（`-march=rv64gcv` 的 golden 要求 V 可用）。

### 6.4 与 TB 缓存的关系

`vspec` 不含 profile。若 profile 在启动后不变（现状如此），无需进 key；但这意味着
**运行期切换 profile 必须 flush TB 缓存**。建议：profile 设为 `latr_system_init` 的入参，
并在 `latr_tb_cache_key()` 里加一条 debug 断言（profile 与 `misa` 常量的一致性），
避免"改了 profile 忘了 flush"的隐式 bug。

---

## 7. 建议的 P1 执行顺序与验收

| 步 | 内容 | 依赖 | 验收 |
| --- | --- | --- | --- |
| **S1** | `latr_rvv_check_operands` + 去回绕（§3.3–3.5）；负例矩阵 §3.6 全部落地 | 无 | `latr-isa` 新增 ≥20 checks（全为 illegal 期望）；现有 1337 不回归 |
| **S2** | C1/C2 两个真缺陷：非法 `frm` 门控（覆盖全部用 `frm` 的向量指令）、归约 `vstart` 门控 | S1 的门控形态可复用 | 差分新增 `frm=5/6` 负例与 `frm=4`(RMM) 数值用例；`latr-diff` 仍 PASS |
| **S3** | whole-register 24 条 + `vl<n>re` 的 `vstart` 字节偏移 | S1 的对齐检查 | 差分：`vl1re8.v/vs1r.v` 往返 + `vstart=1` |
| **S4** | `vleff` 4 条（含 `vl` 截断与 TB 退出） | S3 | 差分：跨页故障 → `vl=已成功数`、`vstart` 语义 |
| **S5** | profile 拆分（§6.3） | 无（可与 S1 并行） | 新增 `test-latr-profile`：`misa.V=0`、`VS` RAZ/WI、`vsetvli` illegal、`vlenb` illegal |
| **S6** | 剩余整数族 18 条（`vrsub/vmulh/vsmul/vw*.w`）+ carry 10 + MAC 8 | S1 | 差分 |
| **S7** | FP FMA 24 + `vfrsub/vfrdiv/vfrec7/vfrsqrt7` 6 | S2（舍入基础设施） | 差分（含 `fflags`） |
| **S8** | segment `nf>1` | S1/S3 | 差分 + `EMUL*NF` 负例 |

每个 S 结束后固定回归（沿用 `GAPS.md` 的验证基线）：
`make -C latr test-decode && make test-latr && make test-pvir && make test-pvir-rvv-shapes && make test-sve-pvir && make -C latr check`。

**注意本机限制**：当前环境没有 `riscv64-*-gcc`（`make test-latr` 需要 `-march=rv64gcv` 交叉工具链），
所以 S1–S4 的裸机验收要么在 CI/有工具链的机器上跑，要么把纯函数部分（`latr_rvv_check_operands`）
抽出来做一个宿主侧单测（类似 `test_latr_decode`：只依赖 `liblatr_core` + 一个假的 `CPUArchState`）。
后者建议提前做，因为它让 S1 的 20 个负例可以在没有交叉工具链时也跑。

---

## 8. 风险与未决问题

1. **合法性检查的"编译期 vs 运行期"边界**：`vtype` 编译期可知（TB key 已含），`vill`/`vstart` 不可知。
   本文的方案是"编译期查寄存器组合，运行期查 vill/vstart"。如果将来 TB 不再特化 `vtype`
   （例如为了减少 TB 数量），这条检查必须改成运行期，成本会上升一个量级 —— 这是个需要先定的方向。
2. **`vl` 进 TB key 的副作用**：`vspec` 只存 8 bit（`vl & 0xff`），`LMUL=8/SEW=8` 时 `vlmax=128`，
   仍然装得下；但如果将来 `VLEN=256`，`vlmax=256` 就会溢出 8 bit。现在不是问题，改 VLEN 前必须先改 key。
3. **缺指令的"临时对策"**：在 S3/S4 落地前，`vl1re8.v` 这类上下文保存指令仍是 illegal。
   如果短期要跑 Linux，Linux 的向量上下文切换会直接崩 —— 这也是为什么 S5（profile）应当与 S3 同时或更早。
4. **PVIR 的 `UNKNOWN` 语义依赖**：C4 指出的隐式契约（`UNKNOWN ⇒ 不写`）目前只存在于实现里，
   `la-pvir.h` 的注释没有钉死。要么写进契约，要么把策略改精确。这是 `RVV_PVIR_GAPS.md` 之外
   需要补的一条。
5. **`vsmul` 的文档/实现对不上**（§2.2 第 2 条）：`RVV_PVIR.md` §6 G6 说 PVIR 侧已支持 `SAT_MUL`，
   编码侧却没登记。建议在 S6 一并补齐，并顺手校订该文档。
