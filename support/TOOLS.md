# 构建工具与模拟器

本机 `tools/` 是 Ubuntu 24.04 x86-64 的项目便携包，未向系统安装软件。它包含 Clang/LLD 18.1.3、dosfstools 4.2 和 QEMU 8.2.2（回移 PNX 修复），以及许可证、来源与哈希清单。通过 `tools/bin/` 的包装器调用；包装器按自身位置选择动态库，因此不能只复制 QEMU 裸二进制。

RISC-V GCC/binutils、`qemu-system-riscv64`、make、gcc、Python 3、Perl 和 e2fsprogs 来自宿主已有环境，不在便携包中。Git 仓库不携带大型工具二进制，换机器需配置这些工具或复制本机完整 `tools/`，并执行 `sha256sum -c tools/SHA256SUMS` 时先进入 `tools/` 目录。

## LoongArch

构建默认查找 `tools/llvm18`，同时允许指定系统已安装的编译器：

```bash
make -C xv6-riscv -j4 ARCH=loongarch LA_CC=clang-18 LA_LD=ld.lld-18 all
```

模拟器需要包含 [QEMU 上游 PNX 修复 db2325f](https://github.com/qemu/qemu/commit/db2325f79481fab87211e5a287580d753f582cb8)。本项目将修复回移到 8.2.2，补丁为本目录 `pnx-backport.patch`。该修复解决取指禁止异常处理时再次取相同指令导致的递归异常；不能用禁用 NX 检查替代。

经验证的 QEMU 源码包为 Ubuntu 的 `qemu_8.2.2+ds.orig.tar.xz`，SHA256：

```text
5e692dd20950a819ff6550cd3974e00319a93f47c0a15f808c58df1a9e4d2478
```

来源：[Ubuntu QEMU 源码目录](https://archive.ubuntu.com/ubuntu/pool/main/q/qemu/)。在新的源码目录校验包、按补丁修改并构建 `loongarch64-softmmu`，或使用已包含该修复的模拟器；具体构建依赖以选定版本的官方说明为准。新工具版本仍需通过项目 `archtest` 的只读代码、NX 堆和内核隔离检查，不能把本机历史验收直接当成新环境通过。

本机验证时使用 `tools/bin/qemu-system-loongarch64`，并保留其版本字符串 `AdvancedOS-PNX-db2325f`。原始工具许可证和包来源保存在本机 `tools/`；源码补丁和来源摘要随 Git 仓库发布。
