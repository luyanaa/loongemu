# LoongEMU

一个基于 LoongArch 的二进制翻译与虚拟化实验项目。LoongEMU 让 x86-64、AArch64、
RISC-V（RV64）和 MIPS64 客户机代码运行在 LoongArch CPU 上，并提供在裸机环境中完成
这件事所需的各个组成部分：可移植的 LBT 实现、翻译块缓存、硬件页表、共享向量 IR
（PVIR），以及可在 QEMU 下运行的裸机 / UEFI 测试程序。

[English](README.md) · [GPL-2.0](LICENSE)

## 项目概述

本项目是一个研究 / 验证用的代码树，而不是单一产品的可执行程序。它关注的核心问题是：
*在没有宿主 libc、没有底层 Hypervisor 的情况下，翻译后的客户机代码究竟能在真实的
LoongArch 硬件上跑多远？*

客户机代码由 **LATX**（x86-64 → LoongArch）、**LATA**（AArch64 → LoongArch）、
**LATR**（RISC-V RV64 → LoongArch）或 **LATM**（MIPS64r2 → LoongArch）解码并翻译；
它们都源自 QEMU 相关的上游代码，并被改造为自包含的裸机静态库。x86 算术标志位通过
**LBT**（龙芯二进制翻译扩展）处理，而 AArch64 SVE 与 RISC-V RVV 共用一套 **PVIR**
向量 IR，可降级到 LSX / LASX 并带有 helper fallback。由于并非所有 LoongArch 处理器
都实现了 LBT，LoongEMU 在同一接口下提供了两套后端：

| 后端 | 目标平台 | 说明 |
| --- | --- | --- |
| 硬件 | 3A6000 系列等 LoongArch 处理器 | 使用真正的 `x86add.w`、`x86mtflag`、`x86settm` 等指令，编译时启用 `+lbt`。 |
| 可移植 fallback | 2K1000LA / 2K2000 以及 QEMU | 用整数 C 实现相同语义。QEMU 不执行 LBT，因此裸机程序通过 `#INE` 异常向量来模拟。 |

### 硬件能力分级

整个代码树的设计目标是**在真实的 LoongArch 硬件上优雅降级**，任何测试都不要求顶配核心。
有两套相互独立的能力分级：

- **LBT（x86 标志位），两档。** 上表就是这一分级：硬件 LBT（`+lbt`，真实的
  `x86add.w` / `x86mtflag` / `x86settm`）用于 3A6000 系列核心；整数 C fallback 用于
  2K1000LA / 2K2000 / QEMU（通过 `#INE` 模拟）。由 `LOONGEMU_CPU_LBT_X86` /
  `LOONGEMU_CPU_LBT_ARM` 能力位选择，并由 `make test-loongarch` 与
  `make test-loongarch-fallback` 验证。
- **向量（AArch64 SVE / RISC-V RVV，经 PVIR），三档，运行时选择**，依据宿主实际向量
  能力 `latx_cpu_has_lasx()` / `latx_cpu_has_lsx()`（`LOONGEMU_CPU_LASX` /
  `LOONGEMU_CPU_LSX`）：

  | 档位 | ISA | 分块 | 用途 |
  | --- | --- | --- | --- |
  | 2 | LASX（256 位，`la_xv*`） | 32 字节 | 最宽 LMUL / SVE 向量路径 |
  | 1 | LSX（128 位，`la_v*`） | 16 字节 | LMUL ≤ 1、谓词分块、更宽分组的奇数尾块 |
  | 0 | C helper | — | 无法静态下沉的形态 |

  档位 2 与 1 甚至可以在同一条指令内混合（例如一个 32 字节 LASX 分块加一个 16 字节 LSX
  尾块）。关键在于：**guest 的架构合法性判定与宿主档位无关** —— SVE 访问检查与 RVV
  `vext_check_*` 合法性都在翻译期根据 guest ISA 决定，因此在纯标量宿主上运行只改变代码
  *如何*执行，不改变 guest *看到*的语义。

### 不依赖 LVZ

**任何核心功能都不以 LVZ 为前提。** 翻译器、PLV3 隔离探针、LATR / LATM / LATA 裸机主板
以及所有宿主测试都可在普通 `qemu-system-loongarch64`（或仓库自带二进制）上以
`-cpu la464` / `-cpu max` 运行，无需 LoongArch 虚拟化扩展。Makefile 中的 `QLVZ_REPO` 与
`QEMU_LA64` 只是 *QEMU 二进制的路径变量* —— 可覆盖，并不表示需要 LVZ。`efi-lvz-probe`
只是可选的 *探测* LVZ / 特权 CSR 的诊断程序，在普通 QEMU 上也能启动；少数 legacy recipe
（`tb-sys*`、`tb-loop`、`lbt-smoke`）只是默认使用 `QLVZ` 路径变量，同样可以覆盖。

在此核心之上，代码树探索了一条轻量化 VMM 路径。对 x86-64 而言，它是 PLV0 下的
LoongArch 4 KiB 硬件分页、在影子页表下以 PLV3 执行的翻译块（PLV0 / PLV3 使用独立的
页表根）、经由客户机 IDT 投递的客户机缺页异常，以及一个以 `(pc, cs_base, cr3, cpl)`
为键的固定大小翻译块缓存。对 RISC-V 则有一个更完整的 `LatrMachine` / `LatrSystem`
主板模型（Sv39/48/57、CLINT、PLIC、UART、fw_cfg、pflash、ACPI、SBI、virtio-mmio），
可从 M 模式启动真实 OpenSBI 并进入带 SMP hart 的 S 模式。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `include/loongemu/`、`src/` | 可移植、可在宿主上测试的核心：`loongbt`（LBT 运算 + EFLAGS + IR2 映射）、`tb_cache`、`loongarch_paging`、`x86_64_paging`、`soft_ras`，以及 RISC-V 主板模型（`riscv_sbi`、`riscv_acpi`、`riscv_plic`、`riscv_clint`、`riscv_fw`、`riscv_virtio_blk`、`phys_mem`、`mmio_bus`）。 |
| `latx/` | LATX x86-64 翻译器：基于 Capstone 的 IR1 解码器、IR2、翻译器、优化、运行时，以及精简版 libc/libm。 |
| `lata/`、`target/arm/lata/` | LATA AArch64 → LoongArch 翻译核心，包括 SVE 降级与共享 PVIR（`pvir.c`、`sve-pvir.c`）向量后端。 |
| `latr/`、`target/riscv/latr/` | LATR RISC-V RV64 → LoongArch 翻译核心、RVV 合法性与 PVIR offload、机器模型与文档（`DESIGN.md`、`MACHINE.md`、`RVV_PVIR.md`、`GAPS.md`、`RVV_P1_LEGALITY.md`）。 |
| `latm/`、`target/mips/latm/` | LATM MIPS64r2 → LoongArch 翻译核心（整数、LBT 非对齐访存、FPU helper）。 |
| `target/i386/`、`target/arm/`、`target/riscv/` | 供翻译器使用的目标相关头文件与源码。 |
| `baremetal/bare/` | 以 `-kernel` 方式启动的测试程序：单翻译块、多翻译块循环、分页冒烟测试、PLV3 隔离探针、LATA/LBT/LATM/LATR 冒烟测试、合成的 LATM MIPS64 主板（`latm_board.c`）、OpenSBI 到 S 模式（`latr_opensbi_smoke.c`），以及长模式 `tb_sys` 启动测试。 |
| `baremetal/efi/` | UEFI 应用程序：LBT fallback 自检、IR1 解码冒烟测试、LVZ 探测、VMM 入口。 |
| `baremetal/gen/` | 生成的 LATX 头文件（`ir2-opcode.h`、`ir2-name.h`、`format-table.h` 等）。 |
| `firmware/` | 固定版本的 EDK2 `QEMU_EFI.fd` / `QEMU_VARS.fd` 和 x86-64 内核代码片段二进制，由 `SHA256SUMS.txt` 校验。 |
| `tools/` | 符号白名单检查脚本和 EFI 镜像构建辅助脚本。 |
| `tests/` | 各可移植模块的宿主单元测试。 |
| `Makefile` | 顶层入口，提供宿主测试和 QEMU/EFI 目标。 |

## 环境要求

- 宿主 C 编译器（`cc`）；宿主测试使用严格编译选项
  （`-std=c11 -O2 -Wall -Wextra -Werror -pedantic`）。
- 交叉编译需要：带 LoongArch 后端的 LLVM/Clang 以及 `ld.lld`。请将 Makefile 变量
  `CLANG`、`OBJDUMP`（EFI 还需 `GENFW`）指向你自己的安装位置；仓库内附带的默认值与特定
  环境相关，本项目不假定任何固定的工具链路径。
- 生成 LATX 头文件需要 `python3`。
- 运行裸机和 UEFI 测试需要 `qemu-system-loongarch64`，任意较新版本均可；不需要 LVZ
  （见上文「不依赖 LVZ」）。若使用替代二进制，`QLVZ_REPO`、`QEMU_LA64` 和 `QLVZ` 都是
  可覆盖的路径变量。
- 运行 LATR 的 OpenSBI 到 S 模式测试需要 RISC-V OpenSBI `fw_dynamic` 镜像；请通过
  `OPEN_SBI_BIN` 指向发行版或本地构建安装的位置。
- 生成 PE32+ EFI 应用程序需要 EDK2 的 `GenFw`；请通过 `GENFW` 指向本地路径。

宿主测试不需要上述交叉工具链，只需要 `cc` 和 `make`。

## 快速开始

在宿主上运行可移植核心测试：

```sh
cd loongemu
make            # 等价于：make test
```

该命令会构建并运行可移植的宿主测试套件，现已覆盖四个翻译器及共享核心：LBT / 分页 /
翻译块缓存（`test_loongbt`、`test_loongbt_cpu`、`test_loongarch_paging`、
`test_x86_64_paging`、`test_tb_cache`、`test_soft_ras`）、x86 VMM 辅助模块
（`test_pci_host`、`test_hyperv`）、PVIR/SVE 与 ARM 状态（`test_pvir`、
`test_sve_decode`、`test_sve_pvir`、`test_vcache`、`test_arm_state`、
`test_lata_context`）、RISC-V 主板模型（`test_riscv_sbi`、`test_riscv_acpi`、
`test_riscv_plic`、`test_riscv_uart`、`test_riscv_fw`）以及 LATM 解码。也可以单独运行：

```sh
make test-cpu        # LBT 后端选择与 EFLAGS 打包/解包
make test-lapaging   # LoongArch 4 KiB 页表池 / PTE / 映射 / 遍历
make test-tbc        # 翻译块缓存
make test-soft-ras   # 软件返回地址栈
make test-pci-host   # PCI 主桥
make test-hyperv     # Hyper-V ABI
make test-pvir       # 共享 PVIR 向量 IR
make test-sve-pvir   # AArch64 SVE 降级到 PVIR
make test-riscv-sbi  # RISC-V SBI 调用层
make test-riscv-acpi # RISC-V ACPI 表生成
make test-paging     # x86-64 页表遍历
make test-libc       # 内置 libc 与宿主 libc 的行为一致性
make test-libm       # 内置 libm 与宿主 libm 的精度一致性
```

撰写本文时，所有宿主测试均通过。代表性计数：`loongbt` 181、`loongbt_cpu` 272、
`loongarch_paging` 386、`tb_cache` 510、`soft_ras` 61、`pvir` 222、`sve_decode` 578、
`sve_pvir` 435、`arm_state` 9296，失败数均为 0。RISC-V 主板检查同样通过（`sbi` 60、
`acpi` 160、`plic` 30、`uart` 23、`fw` 44）。

## 构建翻译核心

每个翻译器都会构建为裸机静态库：

```sh
make -C baremetal all   # baremetal/build/liblatx_core.a  （x86-64 翻译器）
make lata-all           # lata/build/liblata_core.a       （AArch64 翻译器）
make latr-core          # latr/build/liblatr_core.a       （RISC-V 翻译器）
make lata-probe         # 逐文件语法检查
make lata-check         # 构建 LATA 并校验其外部符号白名单
make latr-check         # 构建 LATR 并校验其外部符号白名单
make test-latm          # LATM 解码器 + 龙芯 3A4000 profile/system/uncore/PCI mock 测试
make latm-board-run     # 合成 MIPS64 主板：RAM、16550 + IRQ、FDT、精确异常
```

`check_bare_symbols.sh`、`check_lata_symbols.sh` 及 LATR 白名单工具会对比每个静态库
真正依赖的外部符号与白名单，防止裸机镜像中悄悄引入对 libc/glib/libm 的依赖。目前 LATX
静态库是自包含的；LATA 和 LATR 静态库需要一个很小的平台层（`malloc`、`printf`、`qsort`
等）。生成的 `latr/build/` 产物已被 git 忽略，由 `ir1/qemu/*.decode` 重新生成。

## 裸机与 UEFI 测试程序

以下目标会构建 LoongArch 镜像并在 QEMU 下运行，结果通过模拟串口输出：

```sh
# P0：4 KiB 分页、PLV3 缺页、TLBFILL、恢复执行（QEMU -kernel）
make test-p0

# 单翻译块，通过 #INE 模拟 LBT
make bare-image && make bare-run

# 多翻译块动态执行循环
make tb-loop-image && make tb-loop-run

# PLV3 隔离探针：独立的 PLV0/PLV3 页表根、仅可执行客户机代码页、
# VMM 状态不映射到 PLV3（QEMU -cpu la464 与 max）
make test-plv3-isolation

# 长模式客户机启动：EFER、GDT、CR3、CR0、中断、影子分页、
# 以 PLV3 执行翻译块（2 MiB 大页 TLB、8 组 ASID、SoftRAS），
# 并运行真实的 x86-64 内核片段（使用可覆盖的 QLVZ QEMU 路径变量）
make tb-sys-image && make tb-sys-run
make tb-sys-smp-run                # 多 vCPU 变体

# LATA、LATM、LATR 与 LBT 冒烟测试
make lata-smoke-run
make latm-isa-run
make lbt-smoke-run

# LATR RISC-V：与 QEMU-RISC-V 的 ISA 差分、主板差分、启动冒烟
make test-latr                     # latr-check + ISA + qemu ref + boot，签名差分
make latr-sys-diff                 # 主板与 QEMU-RISC-V 逐字节相同
make latr-opensbi-test             # 真实 OpenSBI fw_dynamic -> S 模式 + SRST（约 18 秒）

# UEFI 应用程序（EDK2 GenFw + QEMU_EFI.fd）
make efi-selftest          # LBT fallback 自检镜像，不含任何 LBT 指令
make efi-boot              # 在非 LBT 处理器上启动：17/17 项检查通过
make efi-ir1-smoke         # 在 LoongArch 上进行 IR1 解码
make efi-lvz-probe         # 可选的 LVZ / 特权 CSR 探测（普通 QEMU 即可）
make vmm-efi && make vmm-boot

# 集成宿主 UEFI -> System VMM，需要 x86-64 OVMF CODE 和 VARS
make vmm-integrated OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
make vmm-integrated-boot OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS=/path/to/OVMF_VARS.fd
```

QEMU 运行都有时间限制（recipe 中使用 `timeout` 或固定等待后结束客户机），日志写入
`build/` 目录。
集成 QEMU 冒烟测试检查交接、`tb_sys` 自检及 x86 OVMF 的持续执行；它不证明 OVMF
已完成启动，也不证明客户机操作系统已启动。


## 两套后端的验证

```sh
make test-loongarch           # objdump：确认所有预期的 LBT 指令组都进入了目标文件
make test-loongarch-fallback  # objdump：确认 fallback 中完全没有 LBT 指令
```

这两项检查保证 fallback 可以在没有 LBT 的处理器上运行。LBT 后端的真实硬件执行**并未**
在此验证，那需要一台 3A6000 系列的机器。

## 当前状态

有三条主线在并行推进：

- **x86-64（`tb-sys`）** —— 让内核片段经由 LATX 和 LoongArch 影子页表完整运行。近期工作
  增加了严格的 PLV3 隔离（独立 `PGDH`/`PGDL` 页表根、W^X 代码缓存、VMM 状态不映射到
  PLV3）、默认启用的软件返回地址栈（`option_soft_ras = 1`）、2 MiB（`PS=20`）影子 TLB
  批处理、每个 vCPU 8 组 ASID 快跳根、按 owner 路由 IPI，以及 A/D compare-exchange 重启处理。
- **AArch64（LATA）** —— SVE 支持已具规模：统一客户机访存契约（精确 Data Abort 与 FFR
  抑制）、带检查的 SVE 访存降级、基于 LSX 16 字节 lane 并在 LASX 上 fallback 的谓词掩码
  分块、携带每 TB PVIR 状态的 helper 调用层，以及可绑定 / 线程本地的 vcache。
- **RISC-V（LATR）** —— 完整客户机主板已跑通：Sv39/48/57、CLINT/PLIC/UART、ACPI、
  SBI 2.0 与 virtio-mmio 块设备；RVV 子集经 PVIR 降级到 LSX/LASX 并带合法性检查；SMP
  使用每 hart 的 TLS 上下文和带锁的组相联 TB 缓存。真实 OpenSBI 从 M 模式启动到 S 模式，
  并通过 SBI SRST 关机。`make test-latr` 与 `make latr-opensbi-test` 均通过。

代码树中的工作可能仍在进行中；宿主测试套件是稳定的入口。LATR 细节见
`target/riscv/latr/DESIGN.md` 和 `MACHINE.md`。

## 已知限制

- 这是一个启动 / 验证用的代码树，不是生产级 Hypervisor；除少量桩实现（串口、PIT、
  PCI、APIC）外没有设备模型。
- x86 `tb_sys` 的 PLV3 影子映射**不是不可信客户机的隔离边界**。隔离已得到强化 ——
  独立 `PGDH`/`PGDL` 页表根、仅可执行 / W^X 的代码缓存、VMM 密钥、栈及 prologue/epilogue
  缓冲区不映射到 PLV3，并由 `make test-plv3-isolation` 检查 —— 但客户机仍与一小部分运行时
  表面（`g_vcpu_env`、跳转缓存、临时栈、`pf_table`）共用 PLV3 根。仅运行可信测试客户机；
  软件检查路径尚未覆盖全部内存操作。
- 没有可用宿主 GOP 显存时，Bochs fallback 仅提供客户机可见 RAM，不会输出到宿主显示器。
- 未显式提供宿主读写回调时，物理 7A PCI 配置直通处于关闭状态；集成启动保持回调为
  `NULL`，回归测试断言后端关闭。开放前需要独立的 BAR/DMA/IRQ 所有权设计以及
  IOMMU/DMA 隔离，黑名单和 errata 处理本身不足以保证安全。
- Makefile 附带与环境相关的工具链默认值；请按自己的机器设置 `CLANG`、`OBJDUMP`、
  `LD_LLD`、`GENFW`、`QEMU_LA64` / `QLVZ`、`OPEN_SBI_BIN` 等。项目不假定任何固定的
  安装位置。
- QEMU 不执行 LBT 指令，因此裸机程序对其进行模拟；硬件路径只能在真实的
  3A6000 系列处理器上验证。
- LATR 与 LATA SVE 是研究原型。LATR 实现了 RV64IMAFDC 及部分 RVV（VLEN=128 / ELEN=64），
  但尚未覆盖全部 RVV 1.0；没有真实的 HPM 事件后端，也缺少 AIA/IMSIC/APLIC。SVE 降级对
  无法静态下沉的形态依赖 helper 调用层。详见 `target/riscv/latr/RVV_P1_LEGALITY.md`
  与 `GAPS.md`。
- `build/` 下的产物（包括生成的 `latr/build/`）是生成文件，已被 git 忽略；`firmware/` 中的
  固件是特意跟踪的，并由 `firmware/SHA256SUMS.txt` 固定版本。

## 许可证

GPL-2.0 —— 详见 [LICENSE](LICENSE)。内置的 `latx/capstone_git` 和 `latx/upstream`
代码树保留各自的上游许可证和声明。
