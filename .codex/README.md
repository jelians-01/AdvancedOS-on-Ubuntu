# 正式联调工作树接续

## 2026-10-21 正式联调分支（最新）

- 接续先读 `任务分配/2026-10-21-01-正式联调分支建立与验证.md`。用户已明确授权直接建立正式联调分支。
- 原项目 `/home/jelians/AdvancedOS` 仍为 `codex/vfs@9f6add0`；正式联调使用同一外层仓库的独立工作树 `/home/jelians/AdvancedOS-integration`、分支 `codex/integration`。原工作树、18 个未跟踪日志及 fs.img 保持不变。
- 已本地分步提交原版 mkfs 补全 d44afc0、C 新版接入 9090728、B ext2 接入 3e5c98d。源码基线 `3e5c98d0ffdde1b3145fce8c30304fd15fc94795`，99 个源码文件与此前提取候选完全一致；另有接续文档提交，不能把文档 HEAD 与源码测试基线混淆。
- 本轮构建、三项 VFS 启动自测、vfsdir/vfspath/vfsexec、挂载真实 ext2 时的完整原生 69 项（含慢速组）、相对路径与单盘 copyin 均通过；ext2 镜像前后哈希一致。目录枚举失败和坏 inode 表 panic 已复现，G1 仍未通过。D1 沿用相同源码的 10 月 20 日快速矩阵，本轮未重跑 LoongArch。
- 下一步 B 修 readdir/元数据错误传播，A 补挂载结果诊断并组织修复后统一验收，C 推进平台。未推送、未合回 VFS/master、未安装依赖；不要重复接入或要求 C 重修 D1。后续设备交接需记录关联工作树，不能只复制其 .git 指针文件。

项目完整历史资料仍保存在 Windows `D:\AdvancedOS`。本工作树保存本轮记录和共同约定。
