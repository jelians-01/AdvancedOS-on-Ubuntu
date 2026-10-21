# LoongArch 最小启动探查

这里保存 LoongArch 平台适配的独立实验，包括启动、异常、中断、上下文切换、物理页分配和分页。它们尚未接入完整 xv6 内核。源码与 `xv6-riscv/` 分开维护。

## 已确认的环境

- QEMU：`qemu-system-loongarch64` 7.2.22，机器类型使用 `virt`。
- 交叉编译器：`loongarch64-unknown-linux-gnu-gcc` 13.0.0。
- 工具链存放在项目外的 `tools/loongarch-cross/`，不提交到 GitHub。

## 构建

在 Ubuntu/WSL 中：

```sh
export PATH=/mnt/d/gpt_work/高级操作系统/tools/loongarch-cross/root/cross-tools/bin:$PATH
cd block-device-and-loongarch/loongarch-probe
make clean
make
loongarch64-unknown-linux-gnu-readelf -h build/loongarch-probe.elf
```

构建出口是：

- `build/loongarch-probe.elf`：带符号的 ELF，便于检查入口和段地址；
- `build/loongarch-probe.bin`：去掉 ELF 元数据后的裸二进制；
- `build/loongarch-probe.dis`：反汇编记录。

## 已完成的启动探查

在本机 QEMU 7.2.22 上，以下命令可以加载 ELF 并输出一行串口文字：

```sh
qemu-system-loongarch64 \
  -machine virt -m 2G -nographic \
  -kernel build/loongarch-probe.elf
```

预期输出包含：

```text
loongarch probe: hello
```

可复核日志位于 `evidence/build.log` 和 `evidence/qemu.log`。命令用 `timeout 5` 包装时，超时退出是正常的，因为探查程序会在打印后永久执行 `idle 0`。

这只证明了交叉编译、ELF 链接、QEMU 加载和串口输出链路，不等同于 LoongArch xv6 已移植完成。串口地址目前仍属于平台探查假设；在接入异常、中断、页表和 VirtIO 前，需要结合 QEMU 启动日志与平台源码继续确认。

## 异常入口探查

`make exception` 会构建第二个独立 ELF。它把向量地址写入 CSR.EENTRY，然后执行 `syscall 0`；向量代码不使用栈，向串口写入 `E` 后停住：

```sh
make exception
qemu-system-loongarch64 -machine virt -m 2G -nographic \
  -kernel build/exception-probe.elf
```

输出应包含：

```text
exception probe: before syscall
E
```

日志在 `evidence/exception-build.log` 和 `evidence/exception-qemu.log`。这一步只验证异常入口跳转，不包含寄存器保存、`ertn` 返回或异常码分派。

## 定时器中断探查

`make timer` 会配置 `CSR.ECFG` 的定时器位、`CSR.TCFG` 周期计时器和 `CRMD.IE`，然后进入 `idle`。定时器向同一个向量入口发出中断，向量代码清除 `TINTCLR` 并输出 `T`：

```sh
make timer
qemu-system-loongarch64 -machine virt -m 2G -nographic \
  -kernel build/timer-probe.elf
```

日志在 `evidence/timer-build.log` 和 `evidence/timer-qemu.log`，输出应包含 `T`。这仍是硬件事实探查，不是完整的中断处理器：向量没有保存通用寄存器，也没有执行 `ertn` 返回，因此不能直接接入 xv6 调度器。

## 寄存器保存与 `ertn` 返回探查

`make return` 会建立 4 KiB 内核栈。异常向量先保存 32 个通用寄存器，读取并推进 `CSR.ERA`，输出 `R`，恢复寄存器后执行 `ertn`。返回地址落在 `syscall 0` 后，入口程序继续输出第二行：

```sh
make return
qemu-system-loongarch64 -machine virt -m 2G -nographic \
  -kernel build/return-probe.elf
```

输出应包含：

```text
trap return: before syscall
R
trap return: after ertn
```

日志在 `evidence/return-build.log` 和 `evidence/return-qemu.log`。这是可运行的异常现场保存/恢复样例，仍未接入 xv6 的调度器、页表或系统调用分派。

## 上下文切换探查

`make context` 构建两个独立栈和两个执行上下文，汇编例程保存并恢复 `ra`、`sp`、`fp` 和 `s0-s8`，在 `main` 与 `worker` 之间切换：

```sh
make context
qemu-system-loongarch64 -machine virt -m 2G -nographic \
  -kernel build/context-probe.elf
```

输出应按顺序包含：

```text
context: main
context: worker
context: main resumed
context: worker resumed
context: complete
```

日志在 `evidence/context-build.log` 和 `evidence/context-qemu.log`。这是调度上下文的独立验证，不等于已经接入 xv6 的 `proc`、调度器或页表。



## 物理页分配实验

`make memory` 构建 `build/memory-probe.elf`。链接脚本在程序数据和独立的 16 KiB 栈之后预留 1 MiB 内存，按 4 KiB 分成 256 页。预留区属于该 ELF 的内存段，并由链接断言检查不会超出 QEMU virt 的低地址 RAM，也不会覆盖程序栈。

`page-allocator.c` 维护分配状态，提供初始化、分配、释放和剩余页数查询；分配时清零整页。初始化只接受完整、对齐、非空且不超过 256 页的区域，拒绝重复初始化；释放时拒绝空指针、越界地址、非页首地址及重复释放。

```sh
make memory
qemu-system-loongarch64 -machine virt -smp 1 -m 2G -nographic \
  -kernel build/memory-probe.elf
make host-memory
```

两种运行都应输出 `memory: ALL PASS`。`host-memory` 用宿主编译器和 ASan/UBSan 运行同一份分配器及行为测试，要求宿主已安装支持这两个 sanitizer 的 C 编译器。

测试覆盖：错误初始化、全部页的对齐/唯一性/清零、耗尽后返回空指针、各页数据互不覆盖、错误释放、重复释放、回收再分配时清零，以及八轮完整分配和归还。目标程序先关闭中断源并清零自身 BSS，在单 CPU 下执行；没有验证多 CPU 或中断并发安全。

内存布局依据本次使用的 QEMU 7.2.22 源码：`hw/loongarch/virt.c` 将物理地址 0 起的 256 MiB 映射为 lowram，`include/hw/loongarch/virt.h` 定义 `VIRT_LOWMEM_SIZE=0x10000000`。实验沿用已有程序的 `0x9000000000200000` 入口地址，使用同一直接访问区域。当前未解析固件内存表，也未建立页表或接入 xv6 的 `kalloc`；这不是对所有 LoongArch 机器通用的内存布局约定。

## 分页与访问权限实验

新增 `make vm` 及 `make host-vm`。页表实验管理 16 KiB 映射页面，并复用原有 4 KiB 物理分配器的连续对齐分配功能。软件重填处理器遍历三层页表，将偶/奇叶子装入 TLB；实际验证映射、别名、读写保护、禁止读取、禁止执行、无效页面、撤销及重映射。内核和测试栈使用 DMW，尚未进入用户态。

**完整分页实验要求 QEMU 包含官方 PNX 递归异常修复。** 原版 7.2.22 的禁止执行测试会失败，脚本不会跳过这一项。页大小选择、修复来源、独立工具构建和实验限制见 [页表实验说明](../LoongArch页表实验-说明与复现.md)。原有六类实验仍可分别用各自的 make 目标构建运行。

## 七类探查与两组宿主检查统一复现

在 Ubuntu/WSL 运行，工具不在 PATH 时设置实际安装位置：

```bash
export LOONGARCH_CROSS=/your/toolchain/bin/loongarch64-unknown-linux-gnu-
export LOONGARCH_QEMU=/your/qemu/bin/qemu-system-loongarch64
python3 run_probes.py
```

脚本在 test-output 中新建构建副本，运行物理分配器和页表管理的两组宿主 sanitizer 检查，再验证七类目标实验。目标程序验证后进入空闲循环，脚本按超时终止，并检查串口标记及显式失败信息。原始输出和 manifest.json（含源码、头文件、脚本及 QEMU 二进制校验值）保留在该次目录。
