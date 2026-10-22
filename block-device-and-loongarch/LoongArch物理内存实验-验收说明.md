# LoongArch 物理内存实验验收说明

状态：实现与独立验证完成；2026-10-21 获得提交授权，纳入 `hal-platform` 分支。此说明不代表主线已合并或完整 LoongArch 内核已完成。

## 本轮增加的能力

在原有五类 LoongArch 实验之上，增加独立的物理页分配实验。程序在链接阶段预留 1 MiB 内存，分成 256 个 4 KiB 页面；支持分配、清零、归还以及剩余页数查询。

它相当于先实现一个“小块内存仓库”：取出一页可以使用，归还后可以再次取出；仓库用完会明确返回失败，错误归还也会被拒绝。这为后续页表、内核栈等模块提供可验证的内存分配基础。

## 实现范围

| 文件 | 用途 |
|---|---|
| `loongarch-probe/page-allocator.c`、`.h` | 单 CPU 的页分配状态及接口 |
| `loongarch-probe/memory-entry.S` | 设置独立栈、屏蔽中断源并清零 BSS |
| `loongarch-probe/memory-linker.ld` | 分离程序、16 KiB 栈及 1 MiB 预留页池，检查地址边界 |
| `loongarch-probe/memory-start.c` | 裸机运行入口及初始化错误测试 |
| `loongarch-probe/memory-test.c` | 目标与宿主共用的行为测试 |
| `loongarch-probe/memory-host-test.c` | 宿主 sanitizer 测试入口 |
| `loongarch-probe/Makefile`、`run_probes.py` | 构建、六类 QEMU 实验及宿主检查 |

没有修改 `xv6-riscv/`。没有集成或替换其他分支的 VFS、ext2 或 FAT32 代码。

## 实际验证结果

最终运行目录：`loongarch-probe/test-output/run-20261020-204116/`。可随代码提交的日志保存在 `证据/20261020/LoongArch-memory/`；manifest 记录了被测源码及脚本校验值和运行命令。

| 检查 | 结果 |
|---|---|
| 原有启动/串口、异常入口、定时器、现场恢复、上下文切换 | 五项均通过 |
| 新增物理页实验 | LoongArch QEMU 7.2.22 下通过 |
| 全部 256 页对齐、地址唯一、清零、数据隔离 | 通过 |
| 耗尽后的空返回、归还与再分配、重复释放拒绝 | 通过 |
| 初始化边界、越界/非对齐/空地址释放拒绝 | 通过 |
| 八轮完整分配及回收 | 通过 |
| 同一分配器及测试的宿主 ASan/UBSan 运行 | 通过，无 sanitizer 报错 |
| 新增目标源码 `-Wall -Wextra -Werror` 编译 | 通过 |
| ELF 段与符号地址检查 | 页池位于栈之后，范围为 `0x9000000000206000` 至 `0x9000000000306000`（末端不含） |

首次交叉编译时，编译器拒绝了链接脚本结束符号的 C 指针减法。现已改为整数地址运算后转换为指针，保留 `-Werror`；首次失败日志单独保留。最终完整运行通过。没有因这一处错误降低编译检查。

本轮没有重跑 RISC-V `usertests`，因为该目录与构建入口均未变动。以前的回归记录保留，不能作为其他分支后续集成结果的保证。

## 复现

在 Ubuntu/WSL 中进入仓库下的 `block-device-and-loongarch/loongarch-probe/`：

```sh
export LOONGARCH_CROSS=/实际工具链路径/loongarch64-unknown-linux-gnu-
export LOONGARCH_QEMU=/实际安装路径/qemu-system-loongarch64
python3 run_probes.py
```

需安装 Python 3、make、宿主 C 编译器及 ASan/UBSan。脚本新建构建副本，不使用历史 ELF；预期显示 `memory-host (ASan/UBSan): PASS`、六个目标的 `PASS`，以及 `ALL LOONGARCH PROBES PASSED`。裸机程序结束后保持空闲循环，脚本在四秒后终止 QEMU 并核对输出。

## 当前限制与下一步

- 只管理链接器预留的 1 MiB 内存，不探测整机内存，不读取固件内存表。
- 单 CPU、无中断并发；未提供锁或多核安全保证。
- 实验使用现有直接访问地址，没有建立页表、用户态隔离或缺页处理。
- 尚未接入 xv6 的 `kalloc`、进程调度及用户程序；不能称为完整 LoongArch xv6 移植。

本轮改动提交到 `hal-platform`，由团队审阅后决定是否合入主线。后续先明确 LoongArch 页表和平台接口，完成地址映射与缺页实验，再与主线维护者商定内核接入。
