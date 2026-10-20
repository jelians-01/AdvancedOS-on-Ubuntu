# C 成员下一阶段：LoongArch 最小启动探查

状态：RISC-V 多磁盘与 D1 阶段已完成；LoongArch 第一阶段启动探查已完成，后续仍需继续实现异常、中断、内存和上下文。

## 当前环境结论

- QEMU 7.2.22 已安装，可执行 `qemu-system-loongarch64`。
- QEMU 提供 `virt` 机器，型号为 Loongson-3A5000/LS7A1000。
- 工具链已固定为 `loongarch64-unknown-linux-gnu-gcc` 13.0.0（20220906 experimental），放在项目外的 `tools/loongarch-cross/`。
- QEMU 7.2.22 的 `virt` 机器已经成功加载最小 ELF，并输出 `loongarch probe: hello`。
- `syscall 0` 已经能够跳转到 CSR.EENTRY 指定的向量并输出 `E`；可复核源码和日志位于 `block-device-and-loongarch/loongarch-probe/`。
- 定时器 CSR、定时器中断使能和向量触发已验证；软件异常的 32 个通用寄存器保存、`CSR.ERA` 推进、恢复和 `ertn` 返回也已在独立探查中验证。工具链本身不提交 GitHub。
- 两个独立栈之间的 `ra/sp/fp/s0-s8` 上下文切换也已在 QEMU 中验证；尚未接入 xv6 的 `proc` 和调度器。

## 任务顺序

1. 固定可用的 LoongArch 交叉编译器及版本，记录 gcc、ld、objcopy 输出。
2. 确认 QEMU `virt` 的加载方式、串口设备、内存布局和启动介质。
3. 编写只打印一行串口文字的最小裸机程序，确认编译、链接、加载链路。已完成。
4. 在最小程序上加入异常入口和定时器/外部中断探查。异常入口、定时器触发和软件异常保存/恢复已完成。
5. 再实现页表、内核栈和上下文切换，保持 RISC-V 构建不受影响；独立探查已建立内核栈并验证调度上下文，页表和 xv6 调度器接入仍待完成。
6. 最后才接入用户态、系统调用、VirtIO 磁盘和 xv6 shell。

## 阶段出口

第一阶段只需提交以下证据：

- 交叉工具链版本；
- QEMU 完整启动命令；
- 最小串口程序的源码、链接脚本和构建日志；
- QEMU 串口输出；
- 当前阻塞点和下一步方案。

裸机串口程序只能证明启动链路，不等同于 xv6 已移植完成。LoongArch 不应直接复制 RISC-V 的 MMIO 地址、中断号、异常帧或寄存器约定。

## 与 A、B 的接口边界

本阶段不依赖 VFS、ext2 或 FAT32。开始修改公共的 `proc.c`、`exec.c`、`syscall.c`、`defs.h` 或 `Makefile` 前，先向 A 说明平台字段和构建入口，避免整体覆盖公共文件。
