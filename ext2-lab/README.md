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
