# B 部分 ext2 进展记录

更新：2026-10-23。分支：`B-ext2`。源码位于项目仓库，宿主解析器与内核后端分别维护。下面先记录最新阶段，随后保留之前记录。

## 2026-10-23：FAT32 只读接入 xv6 VFS

在 ext2 开发之外，新增内核 FAT32 后端，通过 C 的 D1 字节设备接口读取卷数据，并实现 A 的 VFS 节点回调。启动时尝试将设备 3 挂载到 `/fat`；设备不存在或卷格式不受支持时不影响 xv6 启动。测试卷以 QEMU 只读磁盘方式挂载。

- 解析 FAT32 BPB 和卷布局，检查扇区/簇几何、FAT 容量、卷大小和根目录簇范围；按 active FAT 或镜像 FAT 规则读取 FAT 表项。
- 通过簇链查找和枚举目录项，拒绝空闲、坏、保留、越界和循环簇链；只解码 8.3 短文件名，不支持 VFAT 长文件名。
- 支持根目录和嵌套目录查找、`stat`/属性、目录枚举、空文件及按偏移读取；没有 FAT 写入、创建、截断、删除回调。
- 新增 `fat32test` 和 `tests/run_fat32_vfs_tests.py`，对根/嵌套目录、大小写不敏感的 8.3 查找、碎片文件数据、EOF、只读写入拒绝及目录创建拒绝做集成测试。

| 本次验证 | 结果 |
|---|---|
| Ubuntu 22.04 RISC-V 强制重编译内核与 `fs.img` | 通过 |
| Ubuntu 24.04 / QEMU 8.2.2：`fat32test`、`vfsdir`、`usertests copyin` | 全部通过 |
| FAT32 写入、截断、新建操作拒绝；测试后原文件仍可完整读取 | 通过 |
| FAT32 测试镜像 SHA-256 前后相同，`fsck.fat -n` 和宿主端读回 | 通过 |
| ext2 全量写入/扩容测试矩阵：重启、只读盘、空间耗尽、恢复和边界 | 通过 |
| ext2 主测试卷及空间/目录边界镜像的 `e2fsck -fn`、逐字节数据和精确计数 | 全部通过 |
| FAT32 宿主解析器 `make clean test` | 通过 |

运行环境分开使用 Ubuntu 22.04 的 RISC-V 工具链和 FAT 检查工具、Ubuntu 24.04 的 QEMU 8.2.2；RISC-V `virt`、3 核、128 MiB 内存。可复现脚本为 `xv6-riscv/tests/run_fat32_vfs_tests.py`；本地镜像和日志在 `xv6-riscv/test-output/fat32-vfs-integration-2026-10-23-rotest/`，Git 已忽略这些生成文件。ext2 回归脚本为 `xv6-riscv/tests/run_ext2_tests.py`，输出在 `xv6-riscv/test-output/ext2-regression-2026-10-23/`。

当前 FAT32 限制：只接受从设备偏移 0 开始的 FAT32 原始卷，支持 FAT 8.3 短文件名；没有分区表解析、VFAT 长文件名、写入、格式化、权限、符号链接或修复功能。没有完成全量 `usertests`、畸形 FAT32 卷的内核端故障注入或 LoongArch 移植验收。下一步可补足 FAT32 内核端异常卷边界测试，并评估 VFAT；ext2 后续仍可推进子目录和删除。

## 2026-10-23：根目录扩容

本阶段让 ext2 根目录在原有目录项空间用完后，继续分配新的目录数据块；范围只覆盖根目录和 12 个直接块，不包含子目录创建或间接目录块。

实现与验证：

- 找不到足够大的现有目录记录时，确认根目录大小、`i_blocks` 和直接块指针一致，并确认剩余块指针为空，再申请一个新目录块。
- 先把新文件记录写入并初始化新块，再更新根 inode 的大小、数据块指针和 512 字节扇区计数，使查找和枚举可以读到扩容后的目录。
- 空闲 inode/数据块不足时拒绝创建；遇到设备写入错误时标记卷为写失败，避免继续写入。重复 `O_CREATE` 打开现有文件不应再新增目录项或 inode。
- 新增 `ext2dirgrow` 测试，检查 12 KiB 目录边界、空间耗尽、inode 耗尽、目录枚举、重复创建和同镜像重启后的持久性。

| 本次验证 | 结果 |
|---|---|
| Ubuntu 22.04 RISC-V 交叉编译：`make -B -j4` 与 `make -B -j4 fs.img` | 通过 |
| 标准 ext2 测试、间接块测试和针对性原生 usertests | 通过 |
| 测试根目录增长、12 个直接块上限、块耗尽和 inode 耗尽 | 通过 |
| 同一镜像重启后验证扩容内容 | 通过 |
| 测试镜像 `e2fsck -fn`、导出文件逐字节比较和精确空间计数 | 全部通过 |

运行环境：Ubuntu 22.04 编译；Ubuntu 24.04 中 QEMU 8.2.2 与 e2fsprogs 执行测试。测试输出位于 WSL 本地目录 `/home/chenliu/AdvancedOS/ext2-vfs-build/dirgrow-2026-10-23/`，测试脚本使用临时副本，没有写入用户的 `base.ext2`。

当前限制：根目录最多 12 个直接数据块；创建仍只支持普通文件且只允许放在根目录。不支持子目录创建、link/unlink、双/三级间接目录块、时间戳维护或崩溃恢复。详见 [本分支总览](../README.md)。

## 2026-10-22：单级间接块写入与回收

基于 `af1d818` 开发。内核文件写入上限从 12 KiB 提升到 268 KiB（274432 字节）：12 个直接数据块，加上单级索引块中 256 个块号指向的数据块。索引块额外占用 1 KiB，不计入文件内容长度。

已实现：

- 首次跨过 12288 字节时分配索引块和数据块，后续从索引块读取/更新块号；支持追加、跨边界覆盖和已存在单级间接文件的修改。
- `truncate_zero` 回收所有直接块、单级间接数据块及索引块，并核对/更新 `i_blocks` 和空闲计数。
- 修改前检查单级映射的块号范围、重复引用、索引块自引用和占用扇区计数；继续拒绝双/三级间接文件的写入和截断。这不是对任意损坏镜像的完整验证。
- 当进入间接区只剩一个空闲块时，归还尚未发布到 inode 的数据块分配，保留原文件；只有零个空闲块时直接失败。先验证/复制用户内容，再申请新块。
- 新增用户测试 `ext2single`，扩展三阶段自动测试脚本。

实际执行环境仍为 Ubuntu 22.04 编译/检查、Ubuntu 24.04 QEMU 8.2.2 运行；RISC-V `virt`、3 核、128 MiB 内存、设备 2、`cache=writeback`、`flush=1`。

| 本次验证 | 结果 |
|---|---|
| 写入/完整读回 274432 字节；拒绝 274433 字节和超上限追加 | 通过 |
| 30001 字节 xv6→ext2→xv6 完整比较 | 通过 |
| 从 12288 字节追加、跨直接/间接边界覆盖、边界无效用户地址拒绝 | 通过 |
| 写满的单级索引清空、反复间接文件→短文件替换、debugfs 预置文件截断重写 | 通过 |
| 同镜像重启验证全部内容、只读设备拒绝修改及哈希不变 | 通过 |
| 只剩 0/1 个空闲块的索引分配失败、反复重试、释放空间后恢复 | 通过 |
| 旧 `ext2test/ext2write`、块/inode/目录耗尽、原生 copyin/truncate2/sharedfd | 通过 |
| 六份镜像 e2fsck、宿主完整字节比较、精确空闲块/inode 计数 | 全部通过 |

`debugfs stat /s-max` 实测：大小 `274432`，`Blockcount: 538`，`TOTAL: 269`，其中 268 个数据块和 1 个 `(IND)` 索引块；`i_blocks` 使用 512 字节单位，所以为 `269×2=538`。本机这次索引块号为 599，仅用于解释该份镜像，不是固定格式常量。

正常测试卷初始空闲块/inode 为 `31423/8177`，结束为 `31087/8166`，净占用 336 块、11 个 inode。预置文件还包括 274433 字节 `double.bin`，用于确认双级间接文件被拒绝修改且内容保持原样。镜像 SHA-256 初始 `630f46c9157288593bb3c365e60c31e750c1ae001e409673bdc69c746744fc43`，结束 `a7a5481c3fc3c602ecc4ed56dd41c86618b1e9b81acb57484515dde2f9c5a5e0`。

原始日志见 [2026-10-22 验证证据](test-evidence/2026-10-22/)，完整本地镜像位于 `xv6-riscv/test-output/ext2-single-20261022/`。沿用文末三阶段复现命令，输出目录须新建。原始 `base.ext2` 未用于写入。

截至 2026-10-22 的限制：仍要求清洁的 1 KiB 块、128 字节 inode、无特性位 ext2；创建仅支持根目录普通文件，根目录尚未实现扩容。不支持 mkdir/link/unlink、双/三级间接写入、时间戳维护或崩溃恢复；宿主工具仍限直接块写入。未执行全量 usertests、指定 VFS 测例、ext2 并发压力或故障注入、FAT32/LoongArch 验收。

## 2026-10-21：直接块可写阶段（历史记录）

本阶段基于 `3d276f0`，完成后提交为 `af1d818`。

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

## 2026-10-21 时的支持范围（历史记录）

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

下一步：扩展目录容量和删除，然后推进 FAT32；双/三级间接块写入另行扩展。
