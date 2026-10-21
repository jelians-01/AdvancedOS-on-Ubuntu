# B-ext2：ext2 文件系统后端

本分支负责 B 的 ext2/FAT32 文件系统工作，已合入 C 的块设备接口和 A 的 VFS。当前已完成 ext2 宿主端解析器，以及 xv6 内核中的只读访问和受限创建、写入、截断功能。

## 当前进展

| 工作项 | 状态 |
|---|---|
| ext2 超级块、块组和 inode 解析 | 已完成宿主端验证 |
| 根目录、目录项和路径查找 | 已完成宿主端验证 |
| 直接块、单/双/三级间接块读取 | 已完成宿主端验证 |
| 创建、覆盖和截断实验 | 已完成受限验证 |
| xv6 内核 ext2 读取 | VFS 挂载、目录枚举和跨块读取测试通过 |
| xv6 内核 ext2 写入 | 根目录文件创建、直接块读写、截断和数据块回收测试通过 |
| xv6 ↔ ext2 双向复制 | 3073 字节完整比较、同镜像重启读回通过 |
| 空间不足与一致性 | 块/inode/目录耗尽、计数核对和四份镜像的 e2fsck 通过 |
| FAT32 后端 | 尚未开始 |

宿主端程序用于格式回归；内核端 `xv6-riscv/kernel/ext2.c` 已按 A 的 VFS ABI 和 C 的 D1 字节设备 ABI 接入。内核创建限定于根目录，写入和截断限定于最多 12 个直接块（12 KiB）的普通文件；尚不支持间接块写入、目录扩容、mkdir、link、unlink 或崩溃恢复。完整限制、已执行测试和复现命令见 [进展记录](ext2-lab/RESULTS.md)。

## C 提供的块设备依赖

C 的 D1 接口位于 `xv6-riscv/kernel/blkdev.h`，按字节偏移进行同步 I/O：

```c
int blk_read(uint dev, uint64 byte_off, void *dst, uint nbytes);
int blk_write(uint dev, uint64 byte_off, const void *src, uint nbytes);
int blk_size(uint dev, uint64 *out_bytes);
int blk_flush(uint dev);
```

设备 2 预留给 ext2 数据卷。接口约束、设备映射和验证证据见 [D1 交付说明](任务分配/C成员/D1交付说明.md)。

## 目录结构

```text
xv6-riscv/                         xv6 代码和 C 的块设备实现
ext2-lab/ext2_reader.c             B 的 ext2 宿主端解析器
ext2-lab/README.md                 解析器使用说明
ext2-lab/RESULTS.md                验证结果和当前限制
xv6-riscv/kernel/ext2.c            xv6 ext2 后端
xv6-riscv/kernel/ext2.h            ext2 挂载入口
xv6-riscv/user/ext2write.c         内核写入、复制及边界测试
xv6-riscv/tests/run_ext2_tests.py  镜像准备、QEMU 测试和宿主校验
```

## 编译和使用

在 WSL2 Ubuntu 中执行：

```bash
cd ext2-lab
gcc -std=c11 -Wall -Wextra -Werror -O2 -o ext2_reader ext2_reader.c
```

只读检查：

```bash
./ext2_reader base.ext2 info
./ext2_reader base.ext2 ls /
./ext2_reader base.ext2 stat /hello.txt
./ext2_reader base.ext2 cat /multi.bin
./ext2_reader base.ext2 read /multi.bin 1020 10
```

写入实验必须使用镜像副本：

```bash
cp base.ext2 work.ext2
./ext2_reader work.ext2 put host-file /new-file
./ext2_reader work.ext2 truncate /new-file
e2fsck -fn work.ext2
```

宿主工具和内核写入均限定为 1 KiB 块、128 字节 inode、无 ext2 特性标志，使用测试镜像副本。宿主 `put` 会替换整个文件；内核普通 `write` 保留未覆盖的尾部，需要 `O_TRUNC` 才会先清空旧内容。

## 已验证结果

- 3073 字节跨块文件创建和逐字节读回通过。
- 16 字节覆盖、截断为零、空文件创建通过。
- 每种写入场景后的 `e2fsck -fn` 检查通过。
- 基准镜像 SHA-256：`5389c626f9f1355b8e6ce47fd83b434293caa735d43e612dd44834d3c9fda8dc`。
- QEMU 8.2.2、RISC-V 三核下，`ext2test`、`ext2write`、同镜像重启读回及只读设备测试通过。
- 数据块、inode、目录空间耗尽测试通过；宿主逐字节比较、精确空闲计数和四份镜像的 `e2fsck -fn` 通过。

## 下一步

下一步扩展间接块写入与回收、目录扩容和删除，再继续 FAT32。当前结果属于受限 ext2 可写里程碑，尚未完成指定 VFS 测例、全量 usertests 或双平台整体验收。
