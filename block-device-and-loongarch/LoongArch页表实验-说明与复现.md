# LoongArch 页表与访问权限实验

本轮在物理页分配实验之上，增加独立的分页验证，作为 hal-platform 分支交付。当前尚未接入 xv6 进程、用户态或文件系统。

## 做了什么

- 从原有 4 KiB 页池分配连续、对齐的内存，为 16 KiB 的映射页面提供物理空间；检查碎片、清零和分配失败。
- 实现三层软件页表，支持映射、查询、修改权限、撤销映射以及页表回收。映射失败时归还本轮新建的页表，避免泄漏。
- 内核代码、栈、串口和页表通过仅允许 PLV0 的 DMW 窗口访问；低地址测试页面经过 TLB 翻译。
- TLB 未命中时，异常入口保存整数寄存器，C 代码查页表，填写偶/奇两个页面的 TLB 项，执行 `tlbfill` 后由 `ertn` 重试原指令。
- 通过实际 load/store/取指验证映射及保护。仅对已登记的测试指令恢复执行；遇到其他异常打印失败并停止。

## 页面大小与页表约定

物理分配器的基本单位仍为 **4 KiB**。本轮分页叶子大小为 **16 KiB**，每个数据页面由四个连续、16 KiB 对齐的基本页组成。

原因是本机 QEMU 7.2.22 的 `target/loongarch/cpu-param.h` 固定 `TARGET_PAGE_BITS=14`，其 `tlb_helper.c` 使用该大小建立软件地址转换缓存。初次采用 4 KiB 映射时，实际转换缓存覆盖了相邻地址，读取结果错误。因此本实验没有把“设置 STLBPS=12”当作 4 KiB 分页已经可用的证据。

三层页表采用实验自定义的 9/9/9 位索引：VA[40:32]、VA[31:23]、VA[22:14]，页内偏移为 VA[13:0]，支持低地址 `[0, 2^41)`。每层表占一个 4 KiB 基本页，包含 512 项。叶子使用 LoongArch 的 TLBELO 格式，包含 V、D、NR、NX 和物理地址，MAT=1、PLV=0。目录项由软件解释，不是 RISC-V 页表，也没有启用 `LDDIR/LDPTE` 或硬件遍历。

TLB 双页结构、权限位及异常返回机制依据 [LoongArch 架构手册第一卷](https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html)，并与本机 QEMU 的 `cpu-csr.h`、`tlb_helper.c` 和 `op_helper.c` 对照。

## 异常验证范围

| 操作 | 检查 |
|---|---|
| 访问偶页、奇页、页面末端及同一物理页的别名 | 读取和写入必须对应正确物理页面，不能覆盖相邻页 |
| 写只读页 | PME，ECODE=4；物理内容保持原值 |
| 读取 NR 页面 | PNR，ECODE=5 |
| 读取 V=0 的叶子 | PIL，ECODE=1 |
| 执行允许执行的页面，再设置 NX | 先返回约定值，再触发 PNX，ECODE=6 |
| 访问撤销映射的页面 | 重填处理器确认页表中没有该叶子，走预期测试恢复路径 |
| 修改权限、改映射到另一物理页 | 清理 TLB 后必须反映新权限和新物理页面 |
| 结束实验 | 页表及数据帧归还，页池恢复 256 个基本页 |

预期异常同时核对来源、ECODE、BADV、故障 PC 和发生次数。未映射分支是本实验重填处理器的“无叶子”处理，不伪称操作系统已实现按需分配或用户进程缺页恢复。

## QEMU 的 PNX 前置修复

未修复的 QEMU 7.2.22 在处理禁止执行异常时，会再次取禁止执行地址的指令来更新 BADI，从而重复 PNX，无法进入来宾异常处理器。本轮保留旧工具和失败记录，另建 QEMU 工具副本，回移官方修复：

[QEMU 官方提交 db2325f：避免读取 BADI 时递归 PNX](https://github.com/qemu/qemu/commit/db2325f79481fab87211e5a287580d753f582cb8)。原作者为 rail5（Andrew S. Rightenburg）。修复把 PNX 归入取指异常，跳过 BADI 读取，不删除 NX 权限检查。

`loongarch-probe/apply_qemu_pnx_fix.py` 只针对新的 QEMU 7.2 源码副本应用同等两行修改，写出补丁和修改前后 SHA256；不自动下载、编译或覆盖现有安装。遇到未知源码或已应用补丁时拒绝继续。

示例构建方式（Ubuntu/WSL，路径按实际替换）：

```sh
cp -a /path/qemu-7.2.22 /path/qemu-7.2.22-pnxfix-source
python3 block-device-and-loongarch/loongarch-probe/apply_qemu_pnx_fix.py \
  /path/qemu-7.2.22-pnxfix-source
mkdir /path/qemu-pnxfix-build
cd /path/qemu-pnxfix-build
/path/qemu-7.2.22-pnxfix-source/configure \
  --target-list=loongarch64-softmmu --disable-docs --disable-werror \
  --disable-gtk --disable-sdl --disable-vnc --disable-tools --disable-user \
  --with-pkgversion=PNX-backport-db2325f
make -j8 qemu-system-loongarch64
```

也可使用已包含该官方修复、并通过本实验验证的 QEMU。不能根据单一主版本号假定已修复。原有 RISC-V QEMU 安装无需替换。

## 复现入口

```sh
cd block-device-and-loongarch/loongarch-probe
export LOONGARCH_CROSS=/path/loongarch64-unknown-linux-gnu-
export LOONGARCH_QEMU=/path/qemu-pnxfix-build/qemu-system-loongarch64
python3 run_probes.py
```

脚本用全新构建副本运行两组宿主 ASan/UBSan 检查和七类 QEMU 实验。旧 QEMU 无法通过新增 PNX 测试时必须报失败，不自动跳过。设置 `LOONGARCH_QEMU_TRACE=1` 可额外记录页表实验的 QEMU 异常日志。每次运行记录源码、头文件、Makefile、测试脚本、修复脚本和 QEMU 二进制的校验值。

单独展示分页实验：

```sh
make vm CROSS="$LOONGARCH_CROSS"
"$LOONGARCH_QEMU" -machine virt -smp 1 -m 2G -nographic -nic none \
  -kernel build/vm-probe.elf
```

看到 `vm: ALL PASS` 后程序保持空闲，按 Ctrl+A 再按 X 退出。正式运行结果以本轮验收记录和日志为准。

## 尚未包含

本轮不包含用户态、PLV3 权限隔离、ASID 进程切换、多 CPU、中断并发、按需分配、整机内存发现、真实板卡适配或磁盘驱动。下一步适合在此基础上验证用户态进入、独立用户栈、最小系统调用及返回，再与主线协商进程接口。
