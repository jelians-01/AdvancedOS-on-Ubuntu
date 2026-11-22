# ext2 宿主端实验

`ext2_reader.c` 是一个运行在 Linux/WSL 上的 ext2 镜像解析器和受限写入实验程序，用于验证 B 的 ext2 格式处理代码。它不是 xv6 内核文件系统后端，也不依赖 VFS 或块设备接口。

## 编译

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -o ext2_reader ext2_reader.c
```

## 只读命令

```bash
./ext2_reader base.ext2 info
./ext2_reader base.ext2 ls /
./ext2_reader base.ext2 stat /hello.txt
./ext2_reader base.ext2 cat /multi.bin
./ext2_reader base.ext2 read /multi.bin 1020 10
```

程序支持超级块检查、块组描述符和 inode 定位、目录项遍历、路径查找、文件属性读取，以及直接块和单/双/三级间接块读取。

## 受限写入命令

只在镜像副本上操作：

```bash
cp base.ext2 work.ext2
./ext2_reader work.ext2 put host-file /new-file
./ext2_reader work.ext2 truncate /new-file
e2fsck -fn work.ext2
```

写入范围限定为：1 KiB 块、128 字节 inode、无 ext2 特性标志、根目录普通文件，且最多 12 个直接数据块（12 KiB）。当前不支持目录扩容、间接块写入或回收、删除、事务和崩溃恢复。写入失败时不保证回滚，因此必须使用测试副本。

## xv6 内核后端

上述命令限制属于宿主工具。内核 `xv6-riscv/kernel/ext2.c` 支持直接块及单级间接块文件写入、覆盖、截断（上限 268 KiB），并通过 VFS 支持嵌套 `mkdir`、子目录内创建文件、目录扩容、文件/空目录删除及普通文件硬链接。每个可写目录目前最多使用 12 个直接块。目录操作维护 `.`、`..`、父目录链接计数及块组目录计数。双/三级间接块写入和崩溃恢复仍未实现。

硬链接复用原 inode 和数据块，只增加目录项与 inode 链接计数；同一卷内可跨目录链接。源文件必须满足当前可写普通文件布局，拒绝目录、无名对象、重复目的名称及跨文件系统链接。为适配共享 stat ABI 的有符号 short 字段，本实现将链接数上限设为 32767；普通资源不足发生在链接发布前，不消耗链接计数，I/O 错误则保留脏卷状态用于离线检查。

删除先移除名称并减少链接计数；最后一个名称消失后，打开的文件仍可读，继续写入或截断会被拒绝。inode 及直接/单级间接数据块在最后一个后端引用释放时回收。已删除目录仍可由旧 fd 枚举，但拒绝在其中创建新对象。未回收的打开对象存在时，卷保持未清洁标记；如果此时异常终止，需要离线检查，未提供孤儿链自动恢复。

只验证新目录能力时，先编译 `kernel/kernel` 和 `fs.img`，再执行：

```sh
cd xv6-riscv
python3 tests/run_ext2_tests.py prepare /tmp/ext2-mkdir --mkdir
python3 tests/run_ext2_tests.py run /tmp/ext2-mkdir --mkdir
python3 tests/run_ext2_tests.py check /tmp/ext2-mkdir --mkdir
```

此模式只启动一次 QEMU，验证嵌套目录、子目录中跨块/单级间接文件读写、截断、已有目录写入、目录扩容及链接计数；宿主阶段执行 `e2fsck -fn`、文件读回与分配计数检查。

只验证删除和空间回收时，把三条命令的 `--mkdir` 换成 `--unlink`，并使用新的输出目录。此模式只启动一次 QEMU，验证已打开文件/目录的删除和同名重建、已删除当前目录的行为、非空目录拒绝及宿主预置硬链接的链接计数递减。宿主检查要求所有临时文件/目录的数据块和 inode 完整回收、原有文件内容不变，并通过 `e2fsck -fn`。

只验证硬链接时，使用 `--link` 和新的输出目录。此模式只启动一次 QEMU，覆盖跨目录硬链接、链接引发的目录扩容、共享读写与截断、拒绝不允许的链接、逐个删除名称及打开文件的最终回收。宿主阶段检查保留名称的 inode 相同、链接计数为 2、数据一致，以及精确分配计数和 `e2fsck -fn`。
