# LATR RISC-V 机器模型与 Linux 启动路径

本文记录「把 LATR 从裸机执行器变成一台完整 RISC-V 机器」的设计、验收阶梯和当前状态。
与 `DESIGN.md`（翻译器内部）和 `GAPS.md`（缺口清单）互补。

## 1. 分层

```
VMM / 宿主线程           每 vCPU 一个 LatrTranslatorContext（TLS 绑定）
   │
LatrMachine              harts[]：CPUArchState + TB cache + context + arena
   │                     run()：合作式轮转，每 hart 一次 dispatch
LatrSystem               RAM / CLINT / PLIC / UART / fw_cfg / pflash / vclock
   │
LoongArch 宿主           生成的代码 + latr_enter_tb trampoline
```

关键边界：**guest 的异常、页表、设备权限、定时器全部由 `LatrMachine`/`LatrSystem` 维护**，
不交给宿主信号处理。宿主只提供一个入口（`enter_tb`）和一个出口（`jirl ra`），
以及可选的宿主 fault 归因（`latr_machine_host_fault`）。

## 2. 每 hart 的独立性

| 状态 | 归属 | 说明 |
|---|---|---|
| `CPUArchState` | `LatrMachineHart.env` | CSR/PC/privilege/向量状态全在 hart 内 |
| `TRANSLATION_DATA` + IR2 缓冲 | `LatrTranslatorContext.tr_data` | 两条 hart 绝不共享标签、IR 链表、寄存器分配 |
| TB 索引 + 代码 arena | `LatrTbCache`（per hart） | 代码不跨 hart 复用；arena 只由 owner 回收 |
| 宿主 PC→guest PC 映射 | `LatrTranslatorContext.tr_data.insn_map_list` | 每 TB 一张堆上 map；回收 arena 时同步 `forget_range` 释放，避免陈旧归因 |
| PVIR 描述符缓存 | `LatrPvirCache`（per hart） | 自带自旋锁，覆盖 vCPU 迁移与共享 fallback |
| CLINT MTIMECMP / MSIP | `LoongemuClint`（按 hart 索引） | 每 hart 独立比较器；tick 时写入**本 hart** 的 MIP |

`latr_context_bind()` 返回切换前的 context，`latr_context_restore()` 放回——
宿主 fault dispatcher 借用别的 hart 之后必须还原，否则多线程下会把 hart 0 的状态指向 hart 1。

## 3. 时间与中断

- 虚拟时钟（`LoongemuVClock`）是**全局**的，每个机器步进只推进一次；
  `latr_system_update_hart()` 只负责把该 hart 的 CLINT 状态（MSIP/MTIP）与设备状态
  反映到它的 `mip`。早期实现把 tick 放在每 hart 循环里，N 核会快 N 倍。
- `mtimecmp` 复位为 `UINT64_MAX`（与 QEMU 一致）。复位成 0 会让第一条 tick 就置 MTIP，
  固件还没来得及编程定时器。
- 中断选择顺序 MEI→MSI→MTI→SEI→SSI→STI，`mideleg` 决定 S 模式接管；PLIC context `2*hart` 为 M、
  `2*hart+1` 为 S，SEIP 已接通。

## 4. 启动契约

| 场景 | 入口 | 寄存器 / CSR |
|---|---|---|
| OpenSBI `fw_dynamic` | `firmware_entry`（RAM 起始） | `a0=hartid`、`a1=DTB`、`a2=0x100`（`fw_dynamic_info`，magic `OSBI`） |
| 裸 Linux `Image`（无固件） | `+2 MiB` | `a0=hartid`、`a1=DTB`、`a2=0`；`mstatus.MPP=S`、`mideleg=mie=0x222`、`mcounteren=7` |
| M 模式 stand-in（默认测试） | RAM 起始 | 同 OpenSBI，用来在没有外部镜像时验证契约 |

无固件直启 S 模式的 CSR 初值由 `latr_machine_boot()` 安装，等价于 OpenSBI 会留下的状态；
不安装的话第一条 `rdtime`/CSR 访问就会 trap。

## 5. 设备与 DTB

`latr_machine_build_dtb()` 生成 QEMU virt 风格的树：`/cpus`（每 hart 一个 `cpu@N` + `cpu-intc`）、
`/memory@80000000`、`/soc/{clint@2000000,plic@c000000,uart@10000000,virtio_mmio@10001000,test@100000}`、
`/poweroff`、`/reboot`、`/chosen`（`bootargs`、`stdout-path`、`linux,initrd-*`）。

`/poweroff`、`/reboot` 与 `/soc/test@100000` 三者成套，对应 QEMU virt 的 SiFive test 设备：
OpenSBI 的 `fdt_reset_sifive_test` 绑到 `sifive,test1`，把 SRST 的 shutdown 实现成向偏移 0
写 `0x5555`（reboot 写 `0x7777`），而 `latr_system` 的 exit 设备正好解码这个写操作。
缺这三个节点时，S 模式 `system_reset` 会返回 `SBI_ERR_NOT_SUPPORTED`（`a0 = -2`），
每次干净关机都得绕过固件直接写 test 设备；补上之后 `a0 = 0`，关机由 OpenSBI 自己完成，
Linux 的 `poweroff`/`reboot` 也走同一条路径。exit 设备窗口按 DTB 声明扩到 4 KiB。

生成的 DTB 已用两条独立途径验证：

```sh
# dtc 结构与语义
dtc -I dtb -O dts our.dtb        # 无 warning，可回读
# QEMU 用同一份 DTB 跑真实 OpenSBI
qemu-system-riscv64 -M virt -cpu rv64 -m 16M \
  -bios opensbi-riscv64-generic-fw_dynamic.bin -dtb our.dtb -serial file:log
# -> OpenSBI v1.8.1 banner，Platform HART Count 1，uart8250，aclint-mtimer @ 10 MHz
```

结论：DTB 生成路径可以支撑真实固件；LATR 侧的问题不在 DTB。

## 6. 验收阶梯与状态

1. **OpenSBI + UART → S 模式** — **通过**。真实 `fw_dynamic` 从 M 模式固件入口启动，
   打出完整 banner（版本、平台信息、domain 信息），随后在 S 模式执行
   `baremetal/bare/latr_smode_payload.S` 的真实 payload，payload 通过 **SBI ecall**
   打印 `[latr] smode up hart=00` / `[latr] smode ok`，最后用 SBI SRST 关机。
   验收判据 `latr-opensbi: PASS opensbi=1 smode=1`。
   根因与修法见 §7。
2. **Linux earlycon** — 未开始（第 1 级已解锁）。
3. **Initramfs shell** — 未开始。
4. **Virtio block rootfs** — 设备模型（MMIO/描述符/DMA/IRQ）与 DTB 节点已就绪并有裸机测试，
   缺 rootfs 镜像与 1–3。
5. **SMP Linux** — 每 hart 的 context/cache/定时器/IPI 已实现并有测试；真并行调度需要 VMM 线程。

第 1 级通过时的关键机器状态（`make latr-opensbi-run`）：

```
uartregs lcr=3 fcr=1 ier=0 dll=2 dlm=0 fifo=1
rc=1 lastpc=0x802006c exit=0x5555
tb == entered == ret          # 每个翻译块都返回了 dispatch，没有漏返回
trap=3 trappc=0x80018bc4      # OpenSBI 自己的 ebreak 模式探测
priv=1 mcause=0x9 mepc=0x802006c   # S 模式 ecall，即 payload 的 SRST
```

`mcause=0x9`（ecall from S-mode）+ `exit=0x5555` 同时成立，说明
**S 模式运行 → ecall 陷入 M 模式 → OpenSBI 处理 → 写 test 设备 → 机器退出**
这条完整往返是通的，而不是"打印了几个字节就停了"。

## 7. 第 1 级的三处根因（已修复）

排查结论按发现顺序记录，因为其中两个现象非常容易互相伪装。

### 7.1 生成代码写入 arena 后没有同步指令缓存

**现象**：真实 OpenSBI 在 `0x80002162`（`fmv.w.x ft0, a0; jr t0`）执行后 guest PC 变 0，
宿主侧表现为伪死循环。

**根因**：`latr_machine` 的翻译 miss 路径在 `latr_tb_insn_map_rebase` 之后就直接进入生成代码，
没有 `dbar 0; ibar 0`。LoongArch 的取指与数据通路之间没有一致性保证，刚写入 arena 的指令
可能取到旧内容或半写状态。

**修法**：`latr_icache_sync()` 在翻译完成后、`latr_enter_tb` 之前调用。

### 7.2 IR2 链表被 append 成环，`label_dispose()` 永不结束

**现象**：越过后仍会在 17 万 TB 左右停住，宿主 `-d exec` 尾部 99% 落在
`label_dispose` ↔ `ir2_opcode_is_branch_with_*`。

**根因**：`ir2_append()` 对"已经是表尾"的节点再 append 一次时，会把自己的 `_next`
指向自己的 id，链表成自环，`label_dispose()` / assemble 的遍历永远走不完。
`target/arm/lata/ir2/ir2.c` 已加防御：`former_last == ir2` 直接返回。

### 7.3 宿主没开浮点单元，生成代码一碰 FPR 就 FPD

**现象**：`0x2162` 之后依然停住，且宿主完全没有报告——因为**宿主根本没有异常处理器**，
`EENTRY = 0`，FPD 异常把控制流送到垃圾地址，看起来只是"慢"。

**根因**：翻译器把 RISC-V 的 FPR 直接映射到 LoongArch 的 `$f` 寄存器（例如
`fmv.w.x ft0, a0` → `fst.d $fs0, $s8, 256`），但 baremetal harness 从未置
`EUEN.FPE`，宿主 CPU 抛 FPD。

这正是"不能因为 baremetal guest 已经能执行，就把所有 VM-exit 情况交给宿主信号处理"的
具体实例：宿主侧需要**显式的异常入口 + 归因**，而不是让异常落到未定义地址。

**修法**（`baremetal/bare/latr_opensbi_smoke.c`）：

- `enable_host_units()`：置 `EUEN.FPE`（以及向量单元），与 `baremetal/bare/main.c` 一致。
- `install_host_fault_handler()`：写 `EENTRY`（**注意 `SAVE0..3` 在这份 QEMU 里是
  `0x30+N`，`0x1a/0x1b` 是 PGDH/PGD，写错会自己触发 INE**），处理器保存
  `ERA/ESTAT/BADV` 后打印 `HOSTFAULT ecode=... era=...` 并停机。
  这次正是它把 `ecode=0x0f`（FPD）、`era` 落在 code arena、出错指令
  `fst.d $fs0,$s8,256` 直接指了出来。

### 7.4 两个"假现象"，浪费了大量时间，务必记住

- **`uart_log` 窗口太小**：先是 256 字节、后来 1024 字节，都在 banner 中途截断。
  一个**完全成功**的运行会因此看起来"打印到 logo 一半就停了"。
  现已 8 KiB，并且报告时按 `uart_log_len` 全量输出。
- **`PASS kernel=1` 的判据是 `contains("K")`**，会被 `OpenSBI` banner 里的字母 K 命中，
  于是"下一级从没跑过"的机器也报 `kernel=1`。现在判据是 payload 自己用 SBI ecall
  打出的两个标记 `smode up hart=` / `smode ok`。

## 8. 复现命令

```sh
make -C latr test-elf          # ELF 装载器负例（30 checks）
make -C latr test-tb-smp       # TB cache / SMP 不变量（63 checks）
make test-riscv-clint          # CLINT 复位值 / per-hart MTIP（17 checks）
make latr-boot-run             # 5 段机器验收（含 2 hart、per-hart IRQ、无固件 S 模式）
make test-latr                 # 全量回归（含 latr-isa 1542 checks、latr-diff）

# 第 1 级验收：真实 OpenSBI + S 模式 payload
make latr-opensbi-run OPEN_SBI_TBS=1500000 OPEN_SBI_STALL=0 OPEN_SBI_CODEWORDS=0
# -> latr-opensbi: PASS opensbi=1 smode=1
```

`latr-opensbi-run` 的可调参数：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `OPEN_SBI_TBS` | 4000 | TB 预算；真实 OpenSBI 到 S 模式约需 80 万 |
| `OPEN_SBI_STALL` | 4 | 连续 N 轮（每轮 1000 TB）无新字节即判停；0 关闭 |
| `OPEN_SBI_STOP` | 1 | 看到 payload 的 `smode ok` 即停；关掉可继续跑 |
| `OPEN_SBI_PROBES` | 0 | 每 TB 两个 `rdtime` + 4K 直方图，会主导运行时间 |
| `OPEN_SBI_PCLOG` | 0 | 从该 TB 号起逐块打印 guest PC（`T`=开始翻译，`D`=翻译完成，`>`=进入） |
| `OPEN_SBI_CODEWORDS` | 200 | 命中 `0x80002162` 时转储的 arena 字数 |

调试三探针的语义：`T` 有而 `D` 无 = 卡在翻译器；`D` 有而 `>` 无返回 = 生成代码没回来；
`tb == entered == ret` 三条计数一致 = 每个块都正常 `ret` 回 dispatch。


## 9. 第 2 级（Linux earlycon）：已开机，未过 BSS/早期初始化

**已具备**：`make latr-linux-run LATR_LINUX_IMAGE=<Image>` 把发行版内核整体装到 `+2 MiB`，
机器在无固件 S 模式下启动它，并**自己服务 S 模式的 `ecall`**（BASE/TIME/IPI/RFENCE/HSM/SRST/DBCN，
`latr_machine.c` 的 `machine_sbi_*`；有固件时该拦截关闭，ecall 归 OpenSBI）。
`make latr-boot-run` 的第 6 段（`sbi=1`）钉住这条链路：BASE 返回 2.0、DBCN 字节真的落到 UART、
返回状态与 `mret` 一致（MPP 清零、MPIE 置 1、PC 越过 ecall）。

**已通过（2026-11-27）**：AOSC `linux-kernel-7.1.13`（PE 包装的 flat Image，QEMU `-kernel`
也是整文件载入）在 LATR 上跑到了 **earlycon**：

```
[    0.000000] Booting Linux on hartid 0
[    0.000000] Linux version 7.1.13-aosc-main ...
[    0.000000] Machine model: latr-riscv64-virt
[    0.000000] SBI specification v2.0 detected     (TIME/IPI/RFENCE/SRST/DBCN 全部识别)
[    0.000000] earlycon: sbi0 at I/O port 0x0 (options '')
[    0.000000] earlycon: sbi0 at I/O port 0x0 (options '')  -> printk 走 SBI DBCN
[    0.000000] riscv: base ISA extensions acdfim
[    0.000000] Initmem setup node 0 [mem 0x80000000-0xafffffff]
[    0.000000] mem auto-init: stack:off, heap alloc:on, heap free:off
```

即：S 模式直启 → DTB 解析 → SBI 2.0 协商 → **DBCN 控制台** → 内存/NUMA/zone 建立。
之后的内核逐页初始化（196608 页）在 1000 万翻译块内没有跑完，所以还没到 initramfs。

**内核镜像的获取不必手动。** `make latr-linux-extract` 现在自己从 AOSC 的
`linux-kernel-7.3.y` topic 下载 riscv64 `.deb`（`LATR_LINUX_DEB_URL` 可覆盖，或用
`LATR_LINUX_DEB` 指向本地文件），解包出的 `vmlinux` 存在
`~/.cache/loongemu/linux/Image`。以前放在 `build/linux/`，而 `make clean` 就是
`rm -rf build`——一次 clean 会把 60 MiB 的发行版内核连同下载一起删掉，再想跑就得重新找 URL。

**`GUEST_RAM_MIB=256` 可缩小 guest RAM。** 用于把"慢"和"卡死"分开：`free_area_init`
的逐页循环是线性的，页数减三倍，越过该阶段的块数也应减三倍。`ram` 由链接脚本钉在
`0x80000000`，所以只有 `_guest_ram_end` 会动（256 MiB → `0x90000000`），校验方法就是看它。

**这段距离里修掉的两个真实缺陷**（都在 `tb-insn-map.c` / `tb-cache.c`，详见 §9.2）：
`LatrTbInsnMap` 每块一张 ~22 KiB 且宿主 `free()` 是 no-op，以及 `rebase` 之后
`begin()` 再也找不到同一张 map，导致 ~1450 块就耗尽 32 MiB 裸机 arena。
在此之前的表现是 `HOSTFAULT ecode=13 era=0` + `$s8` 被写坏（§9.2 第 1 条：
harness 栈指针与数组大小不一致），非常像翻译器写坏了 env base。

### 9.1 复现（本机）

```sh
# 取得镜像（不 vendored）：任一 riscv64 内核 .deb，或已经解开的 vmlinux/Image
make latr-linux-extract LATR_LINUX_DEB=build/linux/linux-kernel.deb
make latr-linux-run LATR_LINUX_IMAGE=build/linux/Image LATR_LINUX_TIMEOUT=600
# 期望：latr-linux: begin image=... / entry=0000000080200000 / tbs=... 
# 失败时读 build/latr_linux.log 的 HOSTFAULT 一行与 last pcs 环。
```

`-m 1G` 是必须的：内核 ~61 MiB 代码+数据，128 MiB 机器会在 `mem_init` 里失败，
日志看起来像翻译器 bug。harness 的 backing store 放在 LoongArch `virt` 的 highmem 窗口
（`0x80000000` 起，1 GiB 机器下 768 MiB），低端 256 MiB 留给 VMM 映像本身。

### 9.2 长启动才暴露的两个固定上限

**1. harness 栈指针必须和数组大小一致。** `_start` 用 `lu12i.w $t0, N` +
`add.d $sp, $sp, $t0` 把 `$sp` 挪到栈顶，`lu12i.w` 的立即数是 4096 的倍数，
所以 `N` 必须等于 `sizeof(stack) >> 12`。Linux harness 最初写的是 512（2 MiB）
而数组只有 1 MiB，`$sp` 从数组外 1 MiB 开始跑，第一次深调用就写进 `.bss`，
表现是 `HOSTFAULT ecode=13 era=0` + `$s8` 被覆盖成小整数——非常像翻译器把
`$s8`（env base）写坏了。`latr_boot_smoke`（128/512 KiB）与 `latr_opensbi_smoke`
（256/1 MiB）都是对得上的，只有新 harness 错，所以这类错误只能靠对照发现。

**2. `LatrTbInsnMap` 的生命周期从此跟 TB 条目绑定。** 一张 map 是
512 spans + 512 bindings ≈ 22 KiB，每个翻译块一张。宿主 `free()` 在
freestanding 构建里是 no-op，而 map 原来只在 arena 回收时整批丢弃；arena 要
50 万块左右才回收一次，于是 32 MiB 的 `LATX_BARE_ARENA_SIZE` 在约 1450 张 map
时就耗尽，`malloc` 里 `g_assert_not_reached`（现在会打印 want/used/cap）。

修法有两层，都在 `tb-insn-map.c` / `tb-cache.c`：
- `latr_tb_cache_insert` 淘汰某一条时把它的 code 地址记进
  `LatrTbCache.evicted_code`，调度器随即 `latr_tb_insn_map_forget()` 精确回收那
  一张——被淘汰的条目永远不可能再被查到，而它描述的代码字节仍在 arena 里
  （可能还有 hart 在执行），所以故障归因不受影响。
- `drop_map` 不再把内存还给 allocator，而是压入
  `TRANSLATION_DATA.insn_map_free_list`；`latr_tb_insn_map_begin` 优先复用。
  这样常驻 map 数上界是"活跃 TB 数 + 复用池"，而不是"曾经翻译过的块数"。

修完的效果：AOSC 内核从 18 万块（`sbi=0`，死在 `0x81ae8672`）推进到 162 万块、
`sbi=4`、PC 已进入内核 KSEG 高地址 `0xffffffff81513afc`。
