# B-ext2：ext2 文件系统后端

本分支以 C 的 `hal-platform` 为基线，负责 B 的 ext2/FAT32 文件系统工作。当前提交的是 ext2 宿主端解析器和受限写入实验，用来先验证镜像布局、inode、目录项以及文件数据块读写算法。

## 当前进展

| 工作项 | 状态 |
|---|---|
| ext2 超级块、块组和 inode 解析 | 已完成宿主端验证 |
| 根目录、目录项和路径查找 | 已完成宿主端验证 |
| 直接块、单/双/三级间接块读取 | 已完成宿主端验证 |
| 创建、覆盖和截断实验 | 已完成受限验证 |
| xv6 内核 ext2 后端 | 等待 A 的 VFS 接口 |
| FAT32 后端 | 尚未开始 |

宿主端程序不是 xv6 内核文件系统驱动。目前没有自行定义 VFS ABI，也没有把 ext2 接入内核。

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

当前写入范围限定为 1 KiB 块、128 字节 inode、无 ext2 特性标志、根目录普通文件和最多 12 个直接数据块（12 KiB）。不支持目录扩容、间接块写入、删除、事务或崩溃恢复。

## 已验证结果

- 3073 字节跨块文件创建和逐字节读回通过。
- 16 字节覆盖、截断为零、空文件创建通过。
- 每种写入场景后的 `e2fsck -fn` 检查通过。
- 基准镜像 SHA-256：`5389c626f9f1355b8e6ce47fd83b434293caa735d43e612dd44834d3c9fda8dc`。

## 下一步

取得 A 的 VFS 头文件、挂载流程、节点操作表、锁和错误码约定后，依据 D1 接口实现 `kernel/ext2.c`。VFS 接口确定前不自行定义内核后端 ABI。
