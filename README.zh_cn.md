# LoongEMU

一个基于 LoongArch 的二进制翻译与虚拟化实验项目。LoongEMU 让 x86-64（以及实验性的
AArch64）客户机代码运行在 LoongArch CPU 上，并提供在裸机环境中完成这件事所需的各个
组成部分：可移植的 LBT 实现、翻译块缓存、硬件页表，以及可在 QEMU 下运行的裸机 /
UEFI 测试程序。

[English](README.md) · [GPL-2.0](LICENSE)

## 项目概述

本项目是一个研究 / 验证用的代码树，而不是单一产品的可执行程序。它关注的核心问题是：
*在没有宿主 libc、没有底层 Hypervisor 的情况下，翻译后的客户机代码究竟能在真实的
LoongArch 硬件上跑多远？*

客户机代码由 **LATX**（x86-64 → LoongArch）或 **LATA**（AArch64 → LoongArch）解码并翻译；
两者都源自 QEMU 相关的上游代码，并被改造为自包含的裸机静态库。x86 算术标志位通过
**LBT**（龙芯二进制翻译扩展）处理。由于并非所有 LoongArch 处理器都实现了 LBT，
LoongEMU 在同一接口下提供了两套后端：

| 后端 | 目标平台 | 说明 |
| --- | --- | --- |
| 硬件 | 3A6000 系列等 LoongArch 处理器 | 使用真正的 `x86add.w`、`x86mtflag`、`x86settm` 等指令，编译时启用 `+lbt`。 |
| 可移植 fallback | 2K1000LA / 2K2000 以及 QEMU | 用整数 C 实现相同语义。QEMU 不执行 LBT，因此裸机程序通过 `#INE` 异常向量来模拟。 |

在此核心之上，代码树探索了一条轻量化 VMM 路径：PLV0 下的 LoongArch 4 KiB 硬件分页、
在影子页表下以 PLV3 执行的翻译块、经由客户机 IDT 投递的客户机缺页异常，以及一个以
`(pc, cs_base, cr3, cpl)` 为键的固定大小翻译块缓存。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `include/loongemu/`、`src/` | 可移植、可在宿主上测试的核心：`loongbt`（LBT 运算 + EFLAGS + IR2 映射）、`tb_cache`、`loongarch_paging`、`x86_64_paging`。 |
| `latx/` | LATX x86-64 翻译器：基于 Capstone 的 IR1 解码器、IR2、翻译器、优化、运行时，以及精简版 libc/libm。 |
| `lata/`、`target/arm/lata/` | LATA AArch64 → LoongArch 翻译核心。 |
| `target/i386/`、`target/arm/` | 供翻译器使用的目标相关头文件与源码。 |
| `baremetal/bare/` | 以 `-kernel` 方式启动的测试程序：单翻译块、多翻译块循环、分页冒烟测试、LATA/LBT 冒烟测试，以及长模式 `tb_sys` 启动测试。 |
| `baremetal/efi/` | UEFI 应用程序：LBT fallback 自检、IR1 解码冒烟测试、LVZ 探测、VMM 入口。 |
| `baremetal/gen/` | 生成的 LATX 头文件（`ir2-opcode.h`、`ir2-name.h`、`format-table.h` 等）。 |
| `firmware/` | 固定版本的 EDK2 `QEMU_EFI.fd` / `QEMU_VARS.fd` 和 x86-64 内核代码片段二进制，由 `SHA256SUMS.txt` 校验。 |
| `tools/` | 符号白名单检查脚本和 EFI 镜像构建辅助脚本。 |
| `tests/` | 各可移植模块的宿主单元测试。 |
| `Makefile` | 顶层入口，提供宿主测试和 QEMU/EFI 目标。 |

## 环境要求

- 宿主 C 编译器（`cc`）；宿主测试使用严格编译选项
  （`-std=c11 -O2 -Wall -Wextra -Werror -pedantic`）。
- 交叉编译需要：带 LoongArch 后端的 LLVM/Clang 以及 `ld.lld`。
  Makefile 默认使用 Homebrew 路径，例如
  `/opt/homebrew/Cellar/llvm/23.1.5_1/bin/clang`；如果本机版本不同，请覆盖
  `CLANG`、`OBJDUMP`（EFI 还需覆盖 `GENFW`）。
- 生成 LATX 头文件需要 `python3`。
- 运行裸机和 UEFI 测试需要 `qemu-system-loongarch64`。
- 运行 LBT / 硬件分页相关测试需要 QEMU-LVZ；请通过 `QLVZ` 指定其可执行文件
  （Makefile 默认值为 `~/Documents/QEMU-LVZ/build/qemu-system-loongarch64`）。
- 生成 PE32+ EFI 应用程序需要 EDK2 的 `GenFw`（`GENFW ?= /tmp/edk2/BaseTools/Source/C/bin/GenFw`）。

宿主测试不需要上述交叉工具链，只需要 `cc` 和 `make`。

## 快速开始

在宿主上运行可移植核心测试：

```sh
cd loongemu
make            # 等价于：make test
```

该命令会构建并运行四套测试：`test_loongbt`、`test_loongbt_cpu`、
`test_loongarch_paging` 和 `test_tb_cache`。也可以单独运行：

```sh
make test-cpu        # LBT 后端选择与 EFLAGS 打包/解包
make test-lapaging   # LoongArch 4 KiB 页表池 / PTE / 映射 / 遍历
make test-tbc        # 翻译块缓存
make test-paging     # x86-64 页表遍历
make test-libc       # 内置 libc 与宿主 libc 的行为一致性
make test-libm       # 内置 libm 与宿主 libm 的精度一致性
```

撰写本文时，所有宿主测试均通过（`loongbt` 181 项、`loongbt_cpu` 272 项、
`loongarch_paging` 367 项、`tb_cache` 510 项，失败数均为 0）。

## 构建翻译核心

两个翻译器都会构建为裸机静态库：

```sh
make -C baremetal all   # baremetal/build/liblatx_core.a  （x86-64 翻译器）
make lata-all           # lata/build/liblata_core.a       （AArch64 翻译器）
make lata-probe         # 逐文件语法检查
make lata-check         # 构建 LATA 并校验其外部符号白名单
```

`check_bare_symbols.sh` 和 `check_lata_symbols.sh` 会对比每个静态库真正依赖的外部符号
与白名单，防止裸机镜像中悄悄引入对 libc/glib/libm 的依赖。目前 LATX 静态库是自包含的；
LATA 静态库需要一个很小的平台层（`malloc`、`printf`、`qsort` 等）。

## 裸机与 UEFI 测试程序

以下目标会构建 LoongArch 镜像并在 QEMU 下运行，结果通过模拟串口输出：

```sh
# P0：4 KiB 分页、PLV3 缺页、TLBFILL、恢复执行（QEMU -kernel）
make test-p0

# 单翻译块，通过 #INE 模拟 LBT
make bare-image && make bare-run

# 多翻译块动态执行循环
make tb-loop-image && make tb-loop-run

# 长模式客户机启动：EFER、GDT、CR3、CR0、中断、影子分页、
# 以 PLV3 执行翻译块，并运行真实的 x86-64 内核片段（需要 QLVZ）
make tb-sys-image && make tb-sys-run

# LATA 与 LBT 冒烟测试
make lata-smoke-run
make lbt-smoke-run

# UEFI 应用程序（EDK2 GenFw + QEMU_EFI.fd）
make efi-selftest          # LBT fallback 自检镜像，不含任何 LBT 指令
make efi-boot              # 在非 LBT 处理器上启动：17/17 项检查通过
make efi-ir1-smoke         # 在 LoongArch 上进行 IR1 解码
make efi-lvz-probe         # 特权级 / LVZ CSR 探测
make vmm-efi && make vmm-boot
```

QEMU 运行都有时间限制（recipe 中使用 `timeout` 或固定等待后结束客户机），日志写入
`build/` 目录。

## 两套后端的验证

```sh
make test-loongarch           # objdump：确认所有预期的 LBT 指令组都进入了目标文件
make test-loongarch-fallback  # objdump：确认 fallback 中完全没有 LBT 指令
```

这两项检查保证 fallback 可以在没有 LBT 的处理器上运行。LBT 后端的真实硬件执行**并未**
在此验证，那需要一台 3A6000 系列的机器。

## 当前状态

目前的开发重点是 `tb-sys`：让 x86-64 内核片段经由 LATX 翻译和 LoongArch 影子页表完整
运行。近期提交涉及客户机页表遍历、通过客户机 IDT 投递 `#PF`、PLV3 翻译块、中断 /
定时器注入、`IRETQ`/`SYSRET` 与 MSR 处理，以及加载内核的 `PT_LOAD` 段。代码树中的工作
可能仍在进行中；宿主测试套件是稳定的入口。

## 已知限制

- 这是一个启动 / 验证用的代码树，不是生产级 Hypervisor；除少量桩实现（串口、PIT、
  PCI、APIC）外没有设备模型。
- Makefile 中的工具链路径固定为 Homebrew LLVM 布局，在其他系统上需要覆盖。
- QEMU 不执行 LBT 指令，因此裸机程序对其进行模拟；硬件路径只能在真实的
  3A6000 系列处理器上验证。
- `build/` 下的产物是生成文件，已被 git 忽略；`firmware/` 中的固件是特意跟踪的，
  并由 `firmware/SHA256SUMS.txt` 固定版本。

## 许可证

GPL-2.0 —— 详见 [LICENSE](LICENSE)。内置的 `latx/capstone_git` 和 `latx/upstream`
代码树保留各自的上游许可证和声明。
