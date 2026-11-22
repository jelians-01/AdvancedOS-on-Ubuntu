> 整合说明（2026-11-22）：下文保留 B 原始阶段交付的范围与历史结果，其历史 test-output 路径不随源码导入。当前共享项目已经接入正式双平台内核并完成新的完整验收；新增来源、集成修复、准确证据与边界见 [本次整合说明](任务分配/2026-11-22-B新版整合说明.md)。下文“LoongArch/全量原生测试不包含在本轮”指 B 的历史交付，不是本次整合验收。

# ext2 / FAT32 阶段交付说明

日期：2026-11-11。分支：`B-ext2`。本文件说明当前可交付的支持范围；本轮验收结果见 `ext2-lab/RESULTS.md` 最新记录。

结论：基础文件操作已通过三磁盘联合验收，随后新增 FAT32 MBR 主分区支持并通过一轮定向验证。联合验收输出为 `xv6-riscv/test-output/fs-joint-20261111/`；新增分区功能输出为 `xv6-riscv/test-output/fat32-mbr-20261111/`。后者没有重跑整套联合验收。

## 基础操作

| 操作 | ext2 | FAT32 |
|---|---|---|
| 自动挂载 | 设备 2，`/mnt` | 设备 3，`/fat` |
| 路径查找、目录枚举、stat | 支持 | 支持短名和 VFAT 长名 |
| 普通文件创建、读取、覆盖、扩展、清零截断 | 支持 | 支持 |
| 嵌套目录创建和目录扩容 | 支持，直接块目录 | 支持，簇链目录 |
| 文件删除、空目录删除 | 支持，拒绝非空目录 | 支持，长名和别名删除整组记录 |
| 打开对象删除及同名重建 | 保留旧对象至最后引用关闭 | 保留旧对象至最后引用关闭 |
| 硬链接 | 同卷普通文件；多个名称共享 inode | 不提供硬链接 |
| 空间管理 | 数据块、inode 位图及空闲/目录计数 | FAT 簇链及主 FSInfo 提示 |
| 失败行为 | 脏卷标记和停写，非事务回滚 | I/O 错误后停写并保留可能被引用的分配 |

已删除但仍打开的对象可读；本阶段拒绝继续写入或截断。目录作为当前目录被删除后，拒绝在其中创建新对象。此行为范围与完整 POSIX 语义存在差异。

## 格式与容量边界

- ext2 写入限定：初始干净、无特性位、1 KiB 块、128 字节 inode。普通文件最多 268 KiB（12 个直接块和单级间接块），目录最多 12 KiB。读路径与宿主读取器有更广的解析范围，不代表同样可以写入。
- FAT32 内核接受偏移为 0 的原始卷，或 MBR 中唯一的 0x0B/0x0C FAT32 主分区。原始卷 BPB 优先识别；分区卷的隐藏扇区字段允许为 0 或匹配实际起点。拒绝越界、重叠、多 FAT32 候选及保护性 GPT 分区表，不遍历扩展/逻辑分区。宿主读取器仍只接受原始卷。簇大小与几何必须通过挂载检查；历史定向验证覆盖 512 字节和 8 KiB 簇，本次 MBR 场景使用 512 字节簇。
- 两者均受 xv6 的 MAXPATH 限制：整个路径连同 NUL 最多 128 字节。VFAT 按 UTF-8 输入、UTF-16 存储，ASCII 字母大小写不敏感，其他字符精确匹配。
- ext2 普通文件链接数上限为 32767。FAT32 的空闲计数为提示；未知值保持未知，分配始终检查 FAT，范围内但过期的计数不保证自动纠正。
- 未实现格式化、完整时间戳/权限维护、符号链接、日志和断电恢复、用户态手动挂载/卸载。ext2 不支持双/三级间接块写入。全量原生 usertests 和 LoongArch 整体验收不包含在本轮结论中。

## 与其他模块的边界

- A 的 VFS：使用既有 vnode 回调和路径桥接，提供文件操作、mkdir、unlink 及 ext2 link。共享结构和系统调用编号未改变。
- C 的 D1 块设备：保持 `blk_read/write/size/flush` 接口，设备 1/2/3 分别用于原生根、ext2、FAT32。本阶段没有改动块设备驱动。
- `FAT32_HOST_TEST` 仅供宿主模拟设备验证；普通内核不启用该宏。镜像制作及宿主检查依赖 mke2fs/debugfs/e2fsck 和 mkfs.fat/fsck.fat。

## 最终联合验收的复现

先构建：

```sh
cd xv6-riscv
make -j4 kernel/kernel fs.img
```

在不存在的新输出目录执行一轮：

```sh
python3 tests/run_fs_joint_tests.py prepare test-output/fs-joint
python3 tests/run_fs_joint_tests.py run test-output/fs-joint
python3 tests/run_fs_joint_tests.py check test-output/fs-joint
```

允许分开环境运行：本机使用 Ubuntu 22.04 编译及宿主检查，Ubuntu 24.04 的 QEMU 运行。输出目录需在两个环境中可见。

本轮先同时接入三个磁盘完成写入场景，再用同一组三个镜像重启读回。覆盖跨文件系统复制、ext2 硬链接和删除回收、FAT32 长名及多句柄、原生 VFS 目录行为。重启读回前后核对两个数据盘的 SHA-256，宿主核对数据、分配计数、FAT 镜像和 FSInfo，并运行两个文件系统的一致性检查。

`joint.json` 记录测试时的基线提交、关键源码/内核哈希及结果；原始串口日志和一次性磁盘保存在输出目录。生成产物由 Git 忽略，交付源码和复现步骤保存在仓库。

## FAT32 MBR 功能验证的复现

构建后，在新的输出目录运行一轮：

```sh
python3 tests/run_fat32_mbr_tests.py prepare test-output/fat32-mbr
python3 tests/run_fat32_mbr_tests.py run test-output/fat32-mbr
python3 tests/run_fat32_mbr_tests.py check test-output/fat32-mbr
```

`prepare` 使用 GCC ASan/UBSan 执行实际后端的 17 个挂载检查场景；`run` 一次启动完成长名和多句柄写入/删除，再用同一 FAT32 磁盘重启读回；`check` 比对卷外全部字节的哈希，检查持久化内容、FAT 副本、主/备份 FSInfo 并运行 `fsck.fat -n`。2026-11-11 实测输出为 `FAT32 MBR FEATURE ROUND PASSED`，结果及源码哈希见输出目录的 `mbr.json`。不修改 A 的 VFS ABI 或 C 的块设备接口。
