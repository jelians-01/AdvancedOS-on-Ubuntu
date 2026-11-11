# FAT32 解析器与 xv6 VFS 后端

本目录包含 B 的 Linux 宿主端 FAT32 解析器和测试夹具。xv6 内核后端位于 `xv6-riscv/kernel/fat32.c`，通过 VFS 从设备 3 挂载到 `/fat`。内核支持 VFAT 长名与 8.3 别名查找、目录遍历、普通文件创建/写入/截断、长名目录创建及文件/空目录删除；宿主端解析器仍保持只读。

## 能做什么

宿主程序 `fat32_reader` 会以只读方式打开镜像，可以：

- `info`：读取 BIOS 参数块（BPB），显示扇区、簇、FAT 和数据区等几何信息。
- `ls`：枚举卷根目录或指定子目录。
- `stat`：显示目录项里的属性、首簇和文件长度。
- `cat`：把文件原始字节写到标准输出。
- `read`：按偏移和长度读取一段文件，并以十六进制显示。

解析器检查 FAT32 几何参数和卷边界，支持 FAT 镜像关闭镜像时选择 active FAT；镜像开启镜像时会检查各 FAT 副本中正在读取的链表项是否一致。遍历簇链时会拒绝循环、坏簇、保留值、空闲簇和越界簇；文件读取会先验证整条簇链和文件长度需求，再输出数据，因此损坏链路不会泄漏部分文件内容。测试也会对比镜像前后的 SHA-256，确认读取过程不修改输入。

## 目前的限制

- 只接受不带分区表偏移的 FAT32 原始卷镜像（superfloppy）；BPB 中隐藏扇区必须为零。
- 内核可将已有 VFAT 长名从 UTF-16 转为 UTF-8；查找时 ASCII 字母不区分大小写，其他字符按 UTF-8 精确匹配。长名损坏或 UTF-8 超过 VFS 的 255 字节限制时回退到短名。路径仍受 xv6 的 MAXPATH（含结尾 NUL 共 128 字节）限制。宿主解析器仍只解析 8.3 短名。
- 宿主程序不修改 FAT32 镜像，也不负责格式化。内核可创建 UTF-8 长名文件和目录，自动生成带数字尾缀的唯一短名别名，连续目录项空间不足时扩展目录簇链。通过长名或别名删除都会移除整组目录项，打开对象的簇链延迟到最后一个引用释放时回收。长名记录损坏时仍拒绝删除。非空目录删除、符号链接和 FAT 权限语义不允许或未实现。
- 输入卷必须符合 FAT32 的数据簇数量范围。小型 FAT12/FAT16 卷不会因为格式标记字符串而被误认作 FAT32。
- 内核将文件写入和新簇清零分成不超过 4 KiB 的操作，已针对 8 KiB 簇加入集成验证。
- 普通文件按卷内短目录项位置复用后端节点；长名、短名别名和重复打开共享文件大小、簇链和节点锁，各文件描述符保留独立偏移。目录节点仍保留各自的路径祖先信息，这一机制不代表目录并发操作已完整验收。
- 目录项和文件数据只按 FAT32 的首簇、长度及簇链解释；此阶段不实现 FAT32 修复或一致性修复。
- 读写或 flush 返回 I/O 错误后，卷停止后续修改。由于报错前可能已经写入部分数据，保留可能被目录项引用的簇，不能承诺撤销全部修改；重新使用前需检查镜像。未发布目录项且没有 I/O 错误时，内存或空间不足仍可回收新增簇。此策略不提供崩溃恢复。
- 内核维护主 FSInfo 的空闲簇计数和分配查找提示。提示只作参考，实际簇分配仍检查 FAT；未知计数保持未知，越界提示忽略，明显矛盾的计数降为未知。缺失或签名无效的 FSInfo 不阻止文件操作，也不会被自动修复。范围内但已过期的计数不保证被自动纠正，尚未提供完整扫描式空间统计接口。

## 编译与使用

在 WSL2 Ubuntu 中进入仓库的 `fat32-lab` 目录：

```sh
make
./build/fat32_reader /path/to/volume.img info
./build/fat32_reader /path/to/volume.img ls /
./build/fat32_reader /path/to/volume.img ls /DOCS
./build/fat32_reader /path/to/volume.img stat /HELLO.TXT
./build/fat32_reader /path/to/volume.img cat /HELLO.TXT
./build/fat32_reader /path/to/volume.img read /HELLO.TXT 2 8
```

`cat` 输出二进制原文，终端不一定适合直接查看；需要检查具体字节时优先用 `read`。路径使用 `/` 分隔，目录和文件名目前应使用 8.3 格式。

## 测试

运行：

```sh
make clean test
```

测试需要 `gcc`、Python 3、`mkfs.fat` 和 `fsck.fat`（Ubuntu 软件包 `dosfstools`）。脚本会在临时目录中创建 64 MiB FAT32 镜像，不修改项目里的用户镜像；通过 `mkfs.fat` 制作基础文件系统，再构造有已知内容和簇分布的测试文件。基础镜像先由 `fsck.fat -n` 检查，然后验证：

- 卷信息、根目录和子目录枚举、短文件名大小写不敏感查找。
- 普通文件、空文件、嵌套目录，以及 `10 -> 30 -> 17` 的非连续簇链读取。
- 跨簇偏移读取、EOF 边界和无效参数处理。
- active FAT 选择、镜像 FAT 不一致、目录/文件簇链循环、提前结束、坏簇和错误 BPB 拒绝。
- 对损坏输入报错时标准输出为空，并且只读操作前后镜像 SHA-256 不变。

## xv6 VFS 集成测试

`xv6-riscv/tests/run_fat32_vfs_tests.py` 使用一次性 FAT32 测试卷，覆盖文件读写、嵌套目录、删除和重启读回。损坏场景覆盖错误引导签名、非法扇区大小、卷截短、根目录簇链循环或 FAT 副本不一致。对仍可挂载但部分文件簇链损坏的卷，验证读取可返回有效前缀，读到坏链接时报错，覆盖写、截断和删除均拒绝修改文件。检查阶段比对损坏卷的 SHA-256，验证禁用镜像时只写活动 FAT 并保留表项高 4 位，再对正常卷运行 `fsck.fat -n` 和宿主读回。输出路径必须是不存在的新目录，例如：

```sh
cd xv6-riscv
python3 tests/run_fat32_vfs_tests.py prepare /tmp/fat32-vfs-test
python3 tests/run_fat32_vfs_tests.py run /tmp/fat32-vfs-test
python3 tests/run_fat32_vfs_tests.py check /tmp/fat32-vfs-test
```

执行前需要编译 `kernel/kernel` 与 `fs.img`；`prepare` 和 `check` 需要 `mkfs.fat`、`fsck.fat`，QEMU 测试需要支持 `virt` 的 `qemu-system-riscv64`。脚本对每次启动使用独立的原生根盘副本，避免不同场景互相影响。

只验证大簇写入时，给上述三条命令都加上 `--large-cluster` 并使用新的输出目录。此模式只启动一次 QEMU，验证 8 KiB 簇上的目录分配、跨簇写入、非对齐覆盖和逐字节读回；最后用宿主检查目录簇清零和镜像一致性。

只验证 VFAT 长名查找时，给上述三条命令加上 `--vfat` 并使用新输出目录。此模式启动一次 QEMU，验证跨簇长名、中文和补充平面字符、短名别名、坏校验和回退及拒绝删除损坏的长名组；最后确认测试镜像未被修改。

只验证长名创建和删除时，使用 `--vfat-write`。此模式只启动一次 QEMU，覆盖跨簇目录项分配、别名冲突、打开文件删除后同名重建、长名目录及子文件操作、非法名称拒绝。检查阶段用 `fsck.fat -n` 检查一致性，并通过宿主短名读取器和独立目录项编码器核对保留文件及其长名记录。

只验证普通文件共享状态时，使用 `--shared`。此模式启动一次 QEMU，覆盖多个独立句柄、长短名别名、覆盖和扩展写入、第三个句柄截断、父子进程访问，以及删除后同名重建的状态隔离。检查阶段验证宿主读回和 `fsck.fat -n`，不运行其他测试矩阵。

## 写回失败的定向验证

`xv6-riscv/tests/fat32_io_test.c` 包含实际内核 FAT32 实现，使用单线程内存块设备模拟部分成功后报错、flush 错误、FAT 副本写入错误、读错误和内存不足。它检查错误后没有新增设备写入请求、已发布文件的簇仍分配，以及无 I/O 错误时资源回收后能够重试。模拟锁不验证内核调度或并发，也不模拟断电。

在 WSL Ubuntu 中执行一轮：

```sh
cd xv6-riscv
mkdir -p test-output/fat32-io
gcc -O1 -g -Wall -Werror -fno-omit-frame-pointer -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wl,--gc-sections tests/fat32_io_test.c -o test-output/fat32-io/fat32_io_test
test-output/fat32-io/fat32_io_test
```

## FSInfo 定向验证

`tests/fat32_fsinfo_test.c` 直接包含内核 FAT32 后端，在标准 `mkfs.fat` 镜像和宿主内存块设备上验证：已知/未知计数、越界提示、过期零计数、分配/截断/延迟删除、无效签名或指针，以及 FSInfo 写回失败后的停写行为。示例（在 `xv6-riscv` 内执行）：

```sh
mkdir -p test-output/fat32-fsinfo
truncate -s 64M test-output/fat32-fsinfo/input.img
mkfs.fat --invariant -F 32 -S 512 -s 1 test-output/fat32-fsinfo/input.img
gcc -O1 -g -Wall -Werror -fno-omit-frame-pointer -ffunction-sections -fdata-sections -fsanitize=address,undefined -Wl,--gc-sections tests/fat32_fsinfo_test.c -o test-output/fat32-fsinfo/test
test-output/fat32-fsinfo/test test-output/fat32-fsinfo/input.img test-output/fat32-fsinfo/output.img
fsck.fat -n test-output/fat32-fsinfo/output.img
```

写回顺序为 FAT/数据刷盘、更新主 FSInfo 的 8 字节提示字段、再次刷盘；保留其他字节。[Microsoft FAT32 规范](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf) 指定维护 BPB 指向的主 FSInfo，备份记录无需同步更新，因此本实现保留备份记录原样。此宿主场景不替代真实设备、并发或断电验收。
