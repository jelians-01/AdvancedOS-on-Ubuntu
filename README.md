# 双平台三文件系统 xv6 课程设计

在同一套 xv6 上层内核中，通过 VFS 统一原生 xv6、ext2 和 FAT32 的文件操作，并支持 RISC-V 与 LoongArch。项目成员：陈建良、刘晨、周梦武。课程设计报告及具体贡献说明见 [report/](report/)。

## 目录

| 目录 | 内容 |
|---|---|
| `xv6-riscv/` | 两平台共享的内核、用户程序、构建与回归测试；LoongArch 专属部分在 `kernel/arch/loongarch/` |
| `demo/` | 原赛题输入、模板制作、启动和严格演示核验 |
| `fat32-lab/`、`ext2-lab/` | 宿主文件系统读取与夹具校验源码 |
| `tools/` | 本机保留的便携工具，未提交 Git；来源及替代方式见 [support/TOOLS.md](support/TOOLS.md) |
| `report/` | 中文 IEEE 双栏课程设计报告，含 PDF 和可编辑 LaTeX |
| `validation/` | 精简验收摘要；口径及复现命令见 [VALIDATION.md](VALIDATION.md) |
| `.run/` | 演示或验证时自动产生的工作副本，未提交 Git |

## 本机展示

在 Ubuntu 的项目根目录执行；本机已准备两平台模板，无需重新编译：

```bash
cd ~/AdvancedOS-final
python3 demo/run.py --arch riscv --new
```

LoongArch 将 `riscv` 改为 `loongarch`。进入 xv6 后输入：

```sh
cd /tests
cat cmds.sh | sh
```

应看到四条 `test_copy_file(...): succeed` 与一条 `test_cross_fs_copy(): succeed`。测试程序的退出 0 不能单独证明成功。`mkdir /mnt` 的已存在诊断来自内核已自动挂载 ext2，不表示支持手动挂载。

结束前可输入 `/sync`，然后按 `Ctrl+A`、松开后按 `X` 退出 QEMU。`--new` 每次新建工作副本，`--resume` 接续该架构上次会话。详见 [demo/README.md](demo/README.md)。

## 从 GitHub 获取并构建

```bash
git clone --branch master https://github.com/jelians-01/AdvancedOS-on-Ubuntu.git AdvancedOS-final
cd AdvancedOS-final
```

私有仓库需要拥有访问权限并完成 GitHub 认证。源码仓库不包含数百 MB 的工具和磁盘，首次克隆需要安装构建工具或放入对应的便携工具包；本机整理后的目录已经具备工具和模板。

已验证宿主为 Ubuntu 24.04 x86-64。RISC-V 需要 GCC/binutils 交叉工具、`qemu-system-riscv64`、make、宿主 C 编译器、Python 3、Perl；制作镜像还需要 e2fsprogs 和 dosfstools。LoongArch 使用 Clang/LLD 18，以及包含 PNX 修复的 QEMU。具体版本、项目工具来源及配置见 [工具说明](support/TOOLS.md)。

```bash
make -C xv6-riscv -j4 kernel/kernel fs.img
make -C xv6-riscv -j4 ARCH=loongarch all
python3 demo/prepare.py
python3 demo/check.py --arch all
```

上述 LoongArch 命令默认使用项目 `tools/llvm18`。若使用已安装的系统 LLVM，可显式传入 `LA_CC=clang-18 LA_LD=ld.lld-18`。`demo/prepare.py` 默认同时制作两平台，须先构建两平台；已有模板时使用 `--out demo/templates-new`，核验后再安排替换，不能直接覆盖运行数据。

仅启动不带额外文件系统盘的 xv6，可使用 `make -C xv6-riscv qemu`；三文件系统展示优先使用 `demo/run.py`，它会正确接入三张磁盘。

## 实现范围

- VFS：统一路径、挂载点分派、原生适配、目录枚举和 `sync`。根目录为原生 xv6，`/mnt` 为 ext2，`/fat` 为 FAT32。
- ext2：普通文件读写、嵌套目录、目录扩容、文件/空目录删除、延迟回收、同卷普通文件硬链接。写入限定初始干净、无特性位、1 KiB 块、128 字节 inode；文件上限 268 KiB，目录上限 12 KiB。
- FAT32：8.3 与 VFAT 长名、读写与目录操作、共享对象、主 FSInfo 维护；支持原始卷或唯一的 0x0B/0x0C MBR 主分区，不支持 GPT、扩展/逻辑分区及多候选卷选择。
- 平台：RISC-V 为 3 CPU/128 MiB；LoongArch 为 1 CPU/QEMU 1 GiB，内核使用低 128 MiB，PCI 磁盘轮询。两平台共享上层代码和用户程序源码。

手动 `mount/umount`、任意断电恢复、完整 POSIX 语义、LoongArch 多核不在当前实现中。正常 `sync` 后重启读回通过，不代表掉电一致性。具体测试证据及失败处理边界见 [VALIDATION.md](VALIDATION.md) 和课程报告。

## 贡献和许可

陈建良负责 VFS/原生适配与集成组织，刘晨负责 ext2/FAT32，周梦武负责 D1、多盘和平台机制。结题阶段的正式 LoongArch 上层接入、若干修复及自动化集成有工具辅助，报告按实际来源区分，不将集成补充倒算为队友原始交付。原始 xv6 的 MIT 许可保留在 `xv6-riscv/LICENSE`；便携工具保留各自许可，IEEEtran 模板保留其原许可声明。
