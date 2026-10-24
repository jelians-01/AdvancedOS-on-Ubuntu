# FAT32 解析器与 xv6 VFS 后端

本目录包含 B 的 Linux 宿主端 FAT32 解析器和测试夹具。xv6 内核后端位于 `xv6-riscv/kernel/fat32.c`，通过 VFS 从设备 3 挂载到 `/fat`。内核支持目录遍历、8.3 短文件名查找、嵌套目录、按偏移读取、普通文件创建/写入/截断及目录创建；宿主端解析器仍保持只读。

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
- 文件路径只支持 FAT 8.3 短文件名，按大小写不敏感方式查找；不解码 VFAT 长文件名和 OEM 字符集。
- 宿主程序不修改 FAT32 镜像，也不负责格式化。xv6 内核端支持 8.3 普通文件的创建、跨簇写入、覆盖、截断、嵌套目录创建和重启持久化；删除、符号链接和 FAT 权限语义仍未实现。
- 输入卷必须符合 FAT32 的数据簇数量范围。小型 FAT12/FAT16 卷不会因为格式标记字符串而被误认作 FAT32。
- 目录项和文件数据只按 FAT32 的首簇、长度及簇链解释；此阶段不实现 FAT32 修复或一致性修复。

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

`xv6-riscv/tests/run_fat32_vfs_tests.py` 会准备一次性 FAT32 测试卷，将其作为设备 3 以可写方式连接到 QEMU，并运行 `fat32test`、VFS 目录测试、`usertests copyin` 和重启持久化测试。测试包含目录嵌套创建、`..` 父目录路径、三级路径下写入；另测试 FAT32 引导签名损坏和设备 3 缺失两种情况，确认 xv6 保留原生根文件系统且不挂载 `/fat`。检查阶段运行 `fsck.fat -n`，并由宿主端逐字节读回根目录文件及新建嵌套目录中的文件。输出路径必须是不存在的新目录，例如：

```sh
cd xv6-riscv
python3 tests/run_fat32_vfs_tests.py prepare /tmp/fat32-vfs-test
python3 tests/run_fat32_vfs_tests.py run /tmp/fat32-vfs-test
python3 tests/run_fat32_vfs_tests.py check /tmp/fat32-vfs-test
```

执行前需要编译 `kernel/kernel` 与 `fs.img`；`prepare` 和 `check` 需要 `mkfs.fat`、`fsck.fat`，QEMU 测试需要支持 `virt` 的 `qemu-system-riscv64`。脚本对每次启动使用独立的原生根盘副本，避免不同场景互相影响。当前已覆盖引导签名损坏和设备缺失；后续可补充其它错误 BPB/FAT 表项及不同扇区/簇大小的内核边界测试，再决定是否实现 VFAT 长文件名。
