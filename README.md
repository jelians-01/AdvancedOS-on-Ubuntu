# B-ext2：ext2 文件系统后端

本分支以 C 的 `hal-platform` 为基线，负责 B 的 ext2/FAT32 文件系统工作。当前提交的是 ext2 宿主端解析器和受限写入实验，用来先验证镜像布局、inode、目录项以及文件数据块读写算法。

## 当前进展

| 工作项 | 状态 |
|---|---|
| ext2 超级块、块组和 inode 解析 | 已完成宿主端验证 |
| 根目录、目录项和路径查找 | 已完成宿主端验证 |
| 直接块、单/双/三级间接块读取 | 已完成宿主端验证 |
| 创建、覆盖和截断实验 | 已完成受限验证 |
| xv6 内核 ext2 后端 | 已接入 VFS，使用 RISC-V GCC 成功编译；QEMU 运行验证待升级环境 |
| FAT32 后端 | 尚未开始 |

宿主端程序用于格式回归；内核端 `xv6-riscv/kernel/ext2.c` 已按 A 的 VFS ABI 和 C 的 D1 字节设备 ABI 接入。当前后端只读，尚未实现创建、写入、截断、删除和目录修改。

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
xv6-riscv/kernel/ext2.c            xv6 只读 ext2 后端
xv6-riscv/kernel/ext2.h            ext2 挂载入口
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

内核后端和 `ext2test` 已通过交叉编译。当前 WSL 的 QEMU 6.2 启动该内核时没有串口输出，项目所需 QEMU 版本为 7.2 以上，因此尚无内核运行验证结果。升级后需按 `ext2-lab/RESULTS.md` 的步骤将含有 `hello.txt` 和 `multi.bin` 的镜像接到设备 2 并运行测试；随后再实现 ext2 写入、截断和块/inode 回收。
