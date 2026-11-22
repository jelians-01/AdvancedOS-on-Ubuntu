# 原赛题演示

本目录的模板使用当前最终内核和当前编译的用户程序制作。`tests/` 保留原题的 `test1.c`、`test2.c`、73 字节 `cmds.sh` 和 96 字节 `hello.c`；没有改写原题测试。模板只用于复制，QEMU 不直接写模板。

## 启动

在项目根目录运行：

```bash
python3 demo/run.py --arch riscv --new
```

LoongArch 使用项目附带、含 PNX 修复的 QEMU：

```bash
python3 demo/run.py --arch loongarch --new
```

进入 xv6 后依次输入：

```sh
cd /tests
cat cmds.sh | sh
```

应出现四条 `test_copy_file(...): succeed` 和一条 `test_cross_fs_copy(): succeed`。两个测试程序不可靠地传播内部失败，不能只凭退出码判断成功。脚本中的 `mkdir /mnt` 会报告目录已存在：内核已经自动挂载 ext2，这不表示实现了用户态手动 mount。

查看三份文件并同步：

```sh
/cat /hello.c
/cat /tests/hello.c
/cat /mnt/hello.c
/sync
```

退出 QEMU：先按 `Ctrl+A`，松开后按 `X`。保留上次操作的数据时将 `--new` 改为 `--resume`；新一轮演示用 `--new`。会话全部位于项目 `.run/`，不会散落到 Ubuntu 主目录。`--resume` 复用该架构最后一个会话，不能同时在两个 QEMU 中打开同一会话。

## 重新制作模板

源码变化后先按根目录 README 编译两种架构，再运行：

```bash
python3 demo/prepare.py --out demo/templates-new
python3 demo/check.py --arch all --templates demo/templates-new
```

`prepare.py` 默认输出 `demo/templates`，该目录必须不存在；已有模板时输出到新的目录，核验后再安排替换。`check.py` 不指定 `--templates` 时检查当前 `demo/templates`。脚本使用 Python 标准库、已有 `mke2fs/debugfs/e2fsck`、`mkfs.fat/fsck.fat`、对应 QEMU，不自行安装依赖。

制作过程用原项目 mkfs 新建根盘，包含当前架构全部已编译用户程序；由当前内核创建 `/tests`，给 test1/test2 和脚本所需命令建立硬链接。ext2 为 32 MiB、1 KiB 块、128 字节 inode、无特性位；FAT32 为 64 MiB 原始卷。两者均为新建空卷，不依赖 Windows 旧演示模板。`manifest.json` 记录内核、镜像、测试输入、用户程序和源码哈希。

`check.py` 在新的会话副本上运行原脚本，要求五条具体成功信息、正常同步、三份完整 96 字节数据及宿主文件系统检查均通过，并核对模板未改变。它不代表原题测试覆盖了 FAT32 写入或任意断电恢复；这些功能的专项结果在项目验收记录中。

`templates/`、运行会话和工具二进制不提交 Git。首次从源码仓库克隆后，构建并执行 `python3 demo/prepare.py` 即可重建；本机已准备的模板可以直接演示。
