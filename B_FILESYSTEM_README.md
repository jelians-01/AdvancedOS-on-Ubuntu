# B-ext2：ext2 与 FAT32 文件系统工作

本分支负责 B 的 ext2/FAT32 文件系统工作，已合入 C 的块设备接口和 A 的 VFS。ext2 已在 xv6 内核中支持受限读取、创建、写入、截断、子目录创建和目录扩容；FAT32 已接入 xv6 VFS，从设备 3 挂载到 `/fat`，支持短名和 VFAT 长名文件的基础操作。

支持范围、模块边界和联合验收复现步骤见 [阶段交付说明](FILESYSTEM_DELIVERY.md)。

2026-11-11：声明范围内的三磁盘联合验收通过，包括同镜像重启读回、跨文件系统复制、空间计数及两种文件系统的宿主一致性检查。

## 当前进展

| 工作项 | 状态 |
|---|---|
| ext2 超级块、块组和 inode 解析 | 已完成宿主端验证 |
| 根目录、目录项和路径查找 | 已完成宿主端验证 |
| 直接块、单/双/三级间接块读取 | 已完成宿主端验证 |
| 创建、覆盖和截断实验 | 已完成受限验证 |
| xv6 内核 ext2 读取 | VFS 挂载、根/嵌套目录枚举、嵌套文件查找和跨块读取测试通过 |
| xv6 内核 ext2 写入 | 普通文件直接块和单级间接块写入/截断，上限 268 KiB；mkdir、子目录内创建、目录扩容、硬链接、文件/空目录删除及延迟回收 |
| xv6 ↔ ext2 双向复制 | 3073 和 30001 字节完整比较、同镜像重启读回通过 |
| 空间不足与一致性 | 块/inode/目录耗尽、索引分配失败回收及目录扩容边界通过；测试镜像 `e2fsck` 和逐字节检查通过 |
| FAT32 宿主端原型 | BPB、短文件名目录、碎片簇链只读解析和损坏输入测试通过 |
| xv6 内核 FAT32 | 设备 3 挂载 `/fat`；文件读写/截断、目录创建、文件/空目录删除；VFAT 长名查找、枚举、创建和删除 |
| FAT32 挂载与损坏处理 | 缺盘、错误 BPB、卷截短或根目录簇链损坏时保留原生根；拒绝修改簇链损坏的文件 |
| FAT32 FSInfo | 维护主记录的空闲簇计数与查找提示；未知/无效提示回退，实际分配以 FAT 为准 |

宿主端程序用于格式回归；内核端 `xv6-riscv/kernel/ext2.c` 与 `fat32.c` 按 A 的 VFS ABI 和 C 的 D1 字节设备 ABI 接入。设备 2 是 ext2 数据卷并挂载到 `/mnt`；设备 3 是 FAT32 卷并挂载到 `/fat`。ext2 支持嵌套目录创建及子目录内创建普通文件；目录最多使用 12 个直接块（本配置下 12 KiB）；普通文件支持 12 个直接块和 256 个单级间接数据块，共 268 KiB。支持普通文件硬链接、文件和空目录删除及最后引用释放后的空间回收；尚不支持双/三级间接块写入或崩溃恢复。FAT32 内核支持原始卷或 MBR 中唯一的 FAT32 主分区（0x0B/0x0C）；支持 VFAT 长名文件和目录的创建、查找、枚举及删除，并自动生成唯一短名别名。格式化和崩溃恢复尚未实现。实现和限制见 [FAT32 说明](fat32-lab/README.md)；完整 ext2 限制、测试和复现命令见 [进展记录](ext2-lab/RESULTS.md)。

## C 提供的块设备依赖

C 的 D1 接口位于 `xv6-riscv/kernel/blkdev.h`，按字节偏移进行同步 I/O：

```c
int blk_read(uint dev, uint64 byte_off, void *dst, uint nbytes);
int blk_write(uint dev, uint64 byte_off, const void *src, uint nbytes);
int blk_size(uint dev, uint64 *out_bytes);
int blk_flush(uint dev);
```

设备 2 用于 ext2 数据卷，设备 3 用于 FAT32 卷。接口约束、设备映射和验证证据见 [D1 交付说明](任务分配/C成员/D1交付说明.md)。

## 目录结构

```text
xv6-riscv/                         xv6 代码和 C 的块设备实现
ext2-lab/ext2_reader.c             B 的 ext2 宿主端解析器
ext2-lab/README.md                 解析器使用说明
ext2-lab/RESULTS.md                验证结果和当前限制
xv6-riscv/kernel/ext2.c            xv6 ext2 后端
xv6-riscv/kernel/ext2.h            ext2 挂载入口
xv6-riscv/kernel/fat32.c           xv6 FAT32 后端
xv6-riscv/kernel/fat32.h           FAT32 挂载入口
xv6-riscv/user/ext2write.c         内核写入、复制及边界测试
xv6-riscv/user/ext2single.c        单级间接块、容量边界及回收测试
xv6-riscv/tests/run_ext2_tests.py  镜像准备、QEMU 测试和宿主校验
xv6-riscv/user/fat32test.c         FAT32 VFS 读取、创建、写入、截断、目录和删除测试
xv6-riscv/user/fat32persist.c      FAT32 重启持久化读回测试
xv6-riscv/user/fat32missing.c      FAT32 可选挂载失败回退测试
xv6-riscv/tests/run_fat32_vfs_tests.py FAT32 镜像准备、QEMU 集成和可写校验
xv6-riscv/tests/run_fs_joint_tests.py 三磁盘联合写入、重启读回及宿主一致性验收
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
- 2026-10-22：268 KiB 全量写入/读回、30001 字节双向复制、索引块回收及只剩 0/1 块时的失败恢复通过；六份镜像的 `e2fsck -fn` 全部通过。

## 下一步

已完成声明范围内的基础文件操作及联合验收，新增 FAT32 MBR 主分区支持已单独验证。后续扩展包括 GPT/逻辑分区、更多格式组合、时间戳及 ext2 更广的写入布局；全量 usertests 和双平台整体验收仍未完成。
