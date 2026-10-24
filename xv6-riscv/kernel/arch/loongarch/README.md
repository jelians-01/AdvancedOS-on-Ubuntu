# LoongArch 正式 xv6 平台

这组文件将 LoongArch 接入同一套 xv6 进程、文件系统和用户程序，不是独立实验运行时。共同源文件仍在 `kernel/` 与 `user/`；`kernel/arch/loongarch/` 只保留平台相关部分。

## 构建与运行

从 `xv6-riscv/` 执行：

```sh
make ARCH=loongarch all
make ARCH=loongarch qemu DISK2=/path/to/ext2.img DISK3=/path/to/fat32.img \
  LA_QEMU=/path/to/pnx-fixed/qemu-system-loongarch64
```

产物为 `build/loongarch/kernel.elf` 与 `build/loongarch/fs.img`。后者包含 LoongArch 用户程序，不能拿 RISC-V 的 `fs.img` 替代。默认 RISC-V 构建仍用原路径，两个架构的目标文件、内核、用户程序和根镜像互相隔离。

Clang/LLD 18 使用官方 Ubuntu 包的项目内解压副本，默认目录 `/home/jelians/AdvancedOS-tools/llvm18`。换机器时可传 `LA_TOOLS=/your/path/llvm18`，或分别传 `LA_CC`、`LA_LD`。目标是 `loongarch64-unknown-none`、LP64S；代码当前不使用浮点寄存器。

运行配置固定单 CPU、QEMU virt、1 GiB 客体 RAM。内核仅使用低端 128 MiB。PCI 槽位 1/2/3 固定映射逻辑设备 1/2/3：原生根、ext2、FAT32。缺某个数据盘不重编号其它盘。磁盘必须为裸文件系统卷，不能用分区镜像冒充。

## 平台与公共层边界

- `cpu.h` 提供中断状态、页大小、软件页表约定；`vm.c` 的公用页面管理继续服务两平台。
- `trap.c` 将共同软件 PTE 转换为 LoongArch TLBELO，执行 TLB refill、权限异常、用户系统调用和抢占。
- `vector.S` 保存全部整数寄存器；用户异常先切可信进程内核栈。refill 使用专用内核栈，进程内核栈保留未映射 guard。
- `arch_abi.h` 提供参数、调用号、返回值和 exec 入口/栈设置；共享 `proc/exec/syscall` 不使用另一平台的固定寄存器偏移。
- `pci.c` 来自 C 的 PCI 实验机制并作正式驱动适配，向上提供共同 `virtio_disk_*`。睡眠锁保护同一请求与 bounce buffer；共享 D1 承担字节范围读改写和原生根盘绕缓存写入拒绝。
- 当前 PCI 为串行轮询，UART 通过时钟轮询接入共享控制台。没有声称已实现 LoongArch 多核、PCI 中断队列或浮点上下文。

## 模拟器 PNX 修复是验收前提

系统 QEMU 8.2.2 在正式 `archtest` 的 NX heap 用例上已实测停滞，原因是处理取指禁止异常时再次取相同指令。[QEMU 上游修复 db2325f](https://github.com/qemu/qemu/commit/db2325f79481fab87211e5a287580d753f582cb8) 将 PNX 纳入无需再次取指的异常分类。

应使用按该修复构建的项目专用 QEMU，并在验收记录保存路径、版本与哈希。不能因为 `init/sh` 和原生 usertests 能运行，就推断未覆盖的 NX 异常路径通过；也不能关闭 NX 或去掉测试。

## 验证入口

在 shell 内可执行：

```text
testrun /archtest
testrun /vfspath
testrun /vfsexec
testrun /vfsdir
testrun /usertests
```

三文件系统整体验收、重启持久性、缺盘、坏镜像与宿主文件系统检查使用结题目录的统一双平台 runner。每次用可重建的新镜像，完整原生回归与写入演示使用不同根盘副本，避免数据占满根盘后误判。

`archtest` 不只搜索 PASS：它检查只读代码写入、NX heap 执行和内核地址读取三个子进程均被内核终止，宿主还应检查 `TESTRUN /archtest status=0`。最终是否全部达标，以冻结源码对应的完整验收清单为准。
