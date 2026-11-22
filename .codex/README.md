# 正式联调工作树接续

## 2026-11-22 B 新版整合完成（最新）

- 用户授权分析并整合 `队友进展/AdvancedOS-on-Ubuntu_New/AdvancedOS-on-Ubuntu`。这是 B（刘晨）的文件系统更新，HEAD `fa68bfd`，并包含尚未提交的 MBR 修改；旧 B 基线 `54681da`。原始交付未修改，不用整目录覆盖 A/C。
- 当前 Ubuntu 项目仍为 `/home/jelians/AdvancedOS-final`，分支 `codex/final-report`，已从 `42d026e` 快进到验收源码 `9eda7a0316b42de380f4eee5f6129ed48532021d`（集成 `e937ea9`，旧测试修订 `9eda7a0`）；后续仅提交整合说明和接续记忆。隔离分支 `codex/b-update-20261122` 保留在 `/home/jelians/AdvancedOS-update-20261122`。未推送，未改变 VFS 原分支或早期 integration 分支。
- 新增 ext2 嵌套目录、目录扩容、删除/延迟回收、普通文件硬链接；新增 FAT32 VFAT 长名、唯一 MBR FAT32 主分区及主 FSInfo 计数维护。保留最终版统一 sync、ext2 坏 inode 错误传播、FAT32 全节点共享/真实父引用、整段用户缓冲预检、断链刷新后释放的回滚保护及双架构构建。A/C 的 VFS、块设备和平台实现没有被队友旧副本覆盖。
- 本机 RISC-V/LoongArch 完整矩阵均通过（各 56 条顶层记录，包含完整原生 69 项），新增联合/MBR 每平台 13 条、大簇/VFAT 每平台 6 条通过；FAT32 IO 12 项、FSInfo 7 项、MBR 17 项宿主检查通过。原样 73 字节脚本/96 字节样本补验双平台通过，其范围是旧赛题用户镜像配新内核。首次 full-01 旧断言失败保留，改为正向子目录读写及清理后 full-02 通过，没有降低宿主计数或内容检查。
- 正式目录已重新构建，148 个 tracked xv6 文件与候选一致，两平台各 46 个 ELF 的入口/加载内容/布局一致。完整证据与镜像在 `/home/jelians/AdvancedOS-update-tests-20261122`；更新前两套 root 镜像和两个内核保存在该目录 `before-current-update/`，四文件 SHA256 读回一致。
- 先读 Windows `任务分配/2026-11-22-B新版整合/整合说明.md`（Ubuntu 同步副本 `任务分配/2026-11-22-B新版整合说明.md`）。Windows 同目录保存补丁、来源清单和轻量验收证据 ZIP。`结题汇报/` 的 PPT、源码包、演示启动器、模板及历史证据未改；它们仍对应 `42d026e`，旧模板启动命令不会自动使用新版内核。
- 新边界：ext2 可写配置仍为干净无特性位/1KiB 块/128 字节 inode，文件 268KiB、目录 12KiB；FAT32 支持原始卷或唯一 0x0B/0x0C 主分区，不支持 GPT/扩展分区；手动 mount、日志/断电恢复仍未实现。LoongArch 仍为单 CPU。不能把本次通过解释为这些缺口已补齐。

## 2026-10-21 正式联调分支（最新）

- 接续先读 `任务分配/2026-10-21-01-正式联调分支建立与验证.md`。用户已明确授权直接建立正式联调分支。
- 原项目 `/home/jelians/AdvancedOS` 仍为 `codex/vfs@9f6add0`；正式联调使用同一外层仓库的独立工作树 `/home/jelians/AdvancedOS-integration`、分支 `codex/integration`。原工作树、18 个未跟踪日志及 fs.img 保持不变。
- 已本地分步提交原版 mkfs 补全 d44afc0、C 新版接入 9090728、B ext2 接入 3e5c98d。源码基线 `3e5c98d0ffdde1b3145fce8c30304fd15fc94795`，99 个源码文件与此前提取候选完全一致；另有接续文档提交，不能把文档 HEAD 与源码测试基线混淆。
- 本轮构建、三项 VFS 启动自测、vfsdir/vfspath/vfsexec、挂载真实 ext2 时的完整原生 69 项（含慢速组）、相对路径与单盘 copyin 均通过；ext2 镜像前后哈希一致。目录枚举失败和坏 inode 表 panic 已复现，G1 仍未通过。D1 沿用相同源码的 10 月 20 日快速矩阵，本轮未重跑 LoongArch。
- 下一步 B 修 readdir/元数据错误传播，A 补挂载结果诊断并组织修复后统一验收，C 推进平台。未推送、未合回 VFS/master、未安装依赖；不要重复接入或要求 C 重修 D1。后续设备交接需记录关联工作树，不能只复制其 .git 指针文件。

项目完整历史资料仍保存在 Windows `D:\AdvancedOS`。本工作树保存本轮记录和共同约定。
