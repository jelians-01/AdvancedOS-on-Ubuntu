# B 部分 ext2 进展记录

更新：2026-10-21。分支：`B-ext2`；本阶段基于 `3d276f0` 开发。源码位于项目仓库，宿主解析器与内核后端分别维护。

## 已完成

- 宿主端：超级块、块组描述符、inode、目录项和路径解析；直接块及单/双/三级间接块读取；受限创建、覆盖与截断实验。宿主读回和 `e2fsck` 已通过。
- 内核端：通过 A 的 `vnode_ops` 接入 VFS，通过 C 的 D1 字节接口访问设备 2，在 `/mnt` 挂载 ext2；已实现目录查找/枚举、属性和按偏移读取。
- 本次新增 `create_file`、`write_at`、`truncate_zero`：创建普通文件、分配 inode 和直接数据块、覆盖/扩展内容、清空文件并回收数据块，同步更新主超级块及块组的空闲计数。
- 写入使用每卷睡眠锁，顺序为卷写锁→inode 锁；读取继续使用 inode 锁。创建查找与发布目录项在同一个临界区。DMA 使用页分配缓冲区；没有修改 A/C 的公共接口。
- 每次修改前写入未清洁标记并刷盘，结束时先刷数据/元数据，再恢复清洁标记并刷盘。检测到写入或刷盘错误后拒绝后续修改；读取仍可用。这是错误检测措施，不提供事务或崩溃恢复。

2026-10-20 已修复超级块缓冲区的 DMA 地址问题，以及 `readdir` 从非零目录偏移继续枚举时的边界计算问题；该修复已推送。

## 实际验证结果

环境：WSL2；Ubuntu 22.04 编译和宿主检查（RISC-V GCC 10.2.0、e2fsprogs 1.46.5）；Ubuntu 24.04 运行 QEMU 8.2.2。机器 `virt`、RISC-V、128 MiB 内存、3 核，第二块 virtio 盘为设备 2，QEMU `cache=writeback`，驱动报告 `flush=1`。

| 测试 | 结果 |
|---|---|
| `/mnt` 目录枚举、hello 内容/属性/EOF、3073 字节跨块读取 | `ext2test: ALL TESTS PASSED` |
| xv6→ext2→xv6 复制 3073 字节，全文件比较 | 通过 |
| 覆盖前三个字节并保留尾部、反复截断并写入短内容、截断为空 | 通过 |
| 12288 字节上限、拒绝 12289 字节和满文件后的追加 | 通过 |
| 无效用户地址、间接块文件写入/截断、子目录创建拒绝 | 通过 |
| 不重建 ext2 镜像，重新启动 QEMU 后比较全部写入文件 | `ext2write: PERSISTENCE PASSED` |
| 将同镜像设为只读，拒绝写入，原内容与 SHA-256 不变 | 通过 |
| 128 KiB 小卷：数据块耗尽、部分进度、失败重试及截断后恢复写入 | 通过（9 个填充文件） |
| inode 耗尽、反复失败不泄漏 inode | 通过（5 个文件） |
| 根目录无可用目录项空间，反复失败不泄漏 inode | 通过（9 个长名称文件） |
| 四份镜像 `e2fsck -fn`、逐字节宿主导出比较、精确空闲计数 | 全部通过 |
| 原生定向回归 `usertests copyin`、`truncate2`、`sharedfd` | 全部通过 |

正常测试卷由脚本重新生成，32 MiB、1 KiB 块、128 字节 inode、`-O none`；预置 hello（16 字节，`hello from ext2\n`）、multi（3073 字节）和 indirect（13000 字节）。

本次正常卷初始空闲块/inode 为 `31695/8178`，结束为 `31674/8172`，即净占用 21 个数据块、6 个 inode，与预期一致。初始镜像 SHA-256：`dca2fe65e68264339c9a64e4d4d5b067c0ffac57b55046d90c5c872fcea89092`；结束：`6b98fba3219b97c3c3add8eac7385c2784871689c3755179dad868ef3adf804f`。每次重建 UUID/时间不同，因此镜像哈希也会不同。

原始用户 `base.ext2` 未用于此次写入测试；历史基准哈希为 `5389c626f9f1355b8e6ce47fd83b434293caa735d43e612dd44834d3c9fda8dc`。可复现脚本为 `xv6-riscv/tests/run_ext2_tests.py`；原始日志和 manifest 见 [本次验证证据](test-evidence/2026-10-21/)。本地完整镜像位于 `xv6-riscv/test-output/ext2-write-20261021-verified/`，已从 Git 排除。

## 当前支持范围

- 读取保持已有直接块和间接块能力；写入仅接受清洁的 1 KiB 块、128 字节 inode、无特性位镜像。只读可识别的其他受支持格式不会因此获得写权限。
- 新建只支持根目录普通文件，复用已有目录项空隙，不扩容目录。已有普通文件仅在不带扩展属性、特殊标志、间接块且大小不超过 12 KiB 时支持修改。
- 普通 `write` 保留未覆盖尾部；缩短文件需要 `O_TRUNC`。不支持越过 EOF 写入形成新空洞；新分配块先清零，避免旧数据泄漏。
- 块不足可产生部分进度；后端返回实际字节数，现有 xv6 文件层按其约定对不完整写入向用户返回 `-1`。已写入内容可能保留，不能把失败解释为原子回滚。
- 尚未实现目录扩容、mkdir、link、unlink、间接块写入/回收、时间戳维护、事务或崩溃恢复。inode 的一般删除回收尚未实现。
- 本次未执行 ext2 并发压力测试、I/O 故障注入、指定 VFS 测例、完整 usertests、FAT32 或 LoongArch 验收。

## 复现命令

本机编译器和新版 QEMU 分布在两个 WSL 发行版中。以下均从 PowerShell 执行，输出目录必须不存在；重复测试时更换最后的 `ext2-retest` 目录名。

```powershell
wsl.exe -d Ubuntu-22.04 -- bash -lc 'cd /mnt/c/Users/a1863/Documents/ChatGPT/oswork/AdvancedOS-on-Ubuntu/xv6-riscv && make -j4 TOOLPREFIX=riscv64-unknown-elf- kernel/kernel fs.img && python3 tests/run_ext2_tests.py prepare /mnt/c/Users/a1863/Documents/ChatGPT/oswork/AdvancedOS-on-Ubuntu/xv6-riscv/test-output/ext2-retest'
wsl.exe -d Ubuntu-24.04 -u root -- bash -lc 'cd /mnt/c/Users/a1863/Documents/ChatGPT/oswork/AdvancedOS-on-Ubuntu/xv6-riscv && python3 tests/run_ext2_tests.py run /mnt/c/Users/a1863/Documents/ChatGPT/oswork/AdvancedOS-on-Ubuntu/xv6-riscv/test-output/ext2-retest'
wsl.exe -d Ubuntu-22.04 -- bash -lc 'cd /mnt/c/Users/a1863/Documents/ChatGPT/oswork/AdvancedOS-on-Ubuntu/xv6-riscv && python3 tests/run_ext2_tests.py check /mnt/c/Users/a1863/Documents/ChatGPT/oswork/AdvancedOS-on-Ubuntu/xv6-riscv/test-output/ext2-retest'
```

最终通过提示为 `EXT2 WRITE TEST MATRIX PASSED`。脚本只操作新建的测试目录，使用独立根盘副本，并在退出时仅结束自己启动的 QEMU。

下一步：扩展间接块写入与回收、目录扩容和删除，然后推进 FAT32。
