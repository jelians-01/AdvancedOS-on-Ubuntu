# xv6 虚拟文件系统与硬件抽象层

本项目以 xv6-riscv 为基础，围绕虚拟文件系统（VFS）和硬件抽象层（HAL）开展扩展，目标是统一文件系统访问接口，支持多种文件系统，并逐步推进 LoongArch 平台适配。内核源码位于 `xv6-riscv/`。

## 本次提交：块设备修复与平台验证

本分支维护多磁盘、D1 块设备与 LoongArch 平台探查。本次以原 hal-platform 为基础，只更新本模块及相关测试和文档，不带入其他分支的 VFS 或 ext2 实现。

- 修复 D1 对齐传输直接使用调用方内核虚拟地址的问题，统一通过每设备扇区缓冲区中转。
- 增加内核栈缓冲区的读取、写入和读回测试。
- 自动回归记录源码及 Makefile 校验值，遇到内核 panic 及时失败；集成宿主 20000 次并发写入和 ASan/UBSan 检查。
- 为现有五类 LoongArch 独立探查新增统一构建、运行和日志收集脚本；没有新增完整内核移植能力。

本轮独立验证记录见 [本次提交说明](block-device-and-loongarch/本次提交说明-20261020.md)。独立模块通过不代表其他分支集成后的全部功能通过。

## 当前进展

新增 LoongArch 物理页分配独立实验，按 4 KiB 管理预留的 1 MiB 内存；相关说明见 [平台实验](block-device-and-loongarch/loongarch-probe/README.md)。本轮不修改 RISC-V 内核，也不带入其他分支的文件系统代码。

| 模块 | 状态 |
|---|---|
| RISC-V 原生 xv6 | 可编译、启动，完整原版 `usertests` 通过 |
| 多磁盘与 D1 块设备接口 | 已实现三设备管理、字节读写、容量查询和设备刷新 |
| VFS 与文件系统后端 | 待集成，本分支尚未包含 ext2/FAT32 后端 |
| LoongArch 平台适配 | 已完成交叉编译、QEMU 加载、串口输出、异常现场、`ertn` 返回、定时器和独立上下文切换探查；完整内核适配待实现 |

本阶段重点是多磁盘与块设备层，为文件系统后端提供统一的底层 I/O 接口。设备可读写不等同于文件系统已完成挂载。

## 目录结构

`block-device-and-loongarch/` 保存 D1 块设备说明、交付与测试证据，以及 LoongArch 探查源码和阶段计划。实际 RISC-V 块设备实现仍位于 `xv6-riscv/kernel/`。


```text
xv6-riscv/
  kernel/           内核、块设备接口及 VirtIO 驱动
  user/             用户程序与原版回归测试
  mkfs/             原生文件系统镜像构建工具
  tests/            块设备自动化测试
block-device-and-loongarch/       块设备接口说明、阶段验收与测试日志
block-device-and-loongarch/loongarch-probe/  LoongArch 最小启动与异常入口探查
```

## 编译与启动

在 Ubuntu 中进入源码目录，准备 RISC-V GCC/binutils、make、原生 gcc、perl、bc 和 QEMU 7.2 或更新版本。

```bash
cd xv6-riscv
make qemu
```

QEMU 未加入 PATH 时，可使用 `make qemu QEMU=/实际安装路径/qemu-system-riscv64`。

看到 `$` 后即可执行 xv6 命令。退出 QEMU：先按 Ctrl+A，再按 X。

额外的数据盘通过固定插槽连接：

```bash
make qemu QEMU=/绝对路径/qemu-system-riscv64 \
  DISK2=/绝对路径/dev2.img DISK3=/绝对路径/dev3.img
```

dev=1 是原生根盘，dev=2/3 是数据盘。D1 不负责格式化、挂载或解析分区；只有文件系统后端接入后，数据卷才能通过文件路径访问。

## 块设备接口

接口声明位于 `xv6-riscv/kernel/blkdev.h`，实现位于 `blkdev.c`，底层驱动位于 `virtio_disk.c`。D1 提供 `blk_read`、`blk_write`、`blk_size` 和 `blk_flush`；偏移及长度以字节计，调用同步完成，成功返回 0，失败返回 -1。

详细约束、设备映射、公共文件改动和证据见 [D1 交付说明](block-device-and-loongarch/D1交付说明.md)。

## 测试

在 Ubuntu 的源码目录执行自动化测试。如果 QEMU 已加入 PATH，可省略 `XV6_QEMU` 设置：

```bash
cd xv6-riscv
export XV6_QEMU=/绝对路径/qemu-system-riscv64
python3 tests/run_c_tests.py --quick
# 完整原版回归（包括慢速组）
python3 tests/run_c_tests.py
```

脚本在 `test-output/` 中新建构建副本和一次性数据盘，保存编译日志、串口日志与运行配置。诊断构建会写入测试盘，不要对真实文件系统镜像启用 `BLKDEV_SELFTEST=1`。

宿主并发及错误注入测试运行真实 `kernel/blkdev.c`，使用内存盘和 pthread 锁替代硬件及内核锁：

```bash
mkdir -p test-output/host
gcc -DBLKDEV_HOST_TEST -g -O1 -pthread -fsanitize=address,undefined \
  kernel/blkdev.c tests/blkdev_host_test.c -o test-output/host/blkdev_host_test
test-output/host/blkdev_host_test
```

2026-10-06 阶段验证通过：三设备隔离、非对齐读写、刷新后重启读回、只读盘处理、单根盘兼容、完整原版 `usertests`（含慢速组），以及 20000 次宿主并发部分扇区写入。运行配置和日志见 [阶段验收记录](block-device-and-loongarch/本次接入验收.md)。

宿主测试验证块设备层的读改写逻辑；文件系统后端接入后，还需进行内核并发、挂载与文件操作的联调验收。

## LoongArch 探查

LoongArch 阶段先以独立探查目录固定平台事实，不直接改写 RISC-V 公共内核文件。进入 [LoongArch 探查目录](block-device-and-loongarch/loongarch-probe/)，按其中 README 准备 `loongarch64-unknown-linux-gnu-gcc` 和 QEMU，即可复现串口输出及 `CSR.EENTRY` 异常入口跳转。该探查不表示 xv6 已完成 LoongArch 移植；寄存器保存恢复、定时器触发和独立上下文切换已有探查验证；完整异常与中断处理、页表、用户态、调度器及 VirtIO 的内核接入仍待实现。

## 协作分工

| 成员 | 负责范围 |
|---|---|
| A | VFS、公共接口与系统调用接入 |
| B | ext2/FAT32 后端与文件系统镜像 |
| C | HAL、块设备与多磁盘、LoongArch 平台适配 |

公共文件改动通过个人分支和 Pull Request 审阅后合并。构建产物、QEMU 程序和数据镜像不提交到仓库。

