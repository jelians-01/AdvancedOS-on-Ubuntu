# 验收范围与复现

2026-11-22 的功能验收源码为 `9eda7a0316b42de380f4eee5f6129ed48532021d`。随后交付整理调整目录、工具定位、测试入口与文档，没有改写内核和用户程序的实现。历史完整日志已归档于项目维护记录；仓库保留 `validation/results.json` 摘要，避免把大体积过程磁盘作为交付内容。

| 范围 | RISC-V | LoongArch |
|---|---|---|
| 完整矩阵（各 56 条顶层记录） | PASS | PASS |
| 原生完整 usertests（完整矩阵内一条记录包含 69 项） | PASS | PASS |
| joint + MBR 新功能（各 13 条顶层记录） | PASS | PASS |
| 大簇和 VFAT 专项（各 6 条顶层记录） | PASS | PASS |
| 原样 73 字节脚本/96 字节样本兼容性补验 | PASS | PASS |

这些记录与内部断言是不同层级，不能简单相加得到“总测试数”。完整矩阵覆盖三文件系统六向 30001 字节复制、同步重启、完整内容与 fsck、坏盘/缺盘、容量边界、只读盘、VFS 专项、用户态权限隔离，以及 badwrite 前后空闲块精确计数。宿主 ASan/UBSan 检查包括 FAT32 IO 12 场景、FSInfo 7 场景、MBR 17 场景。

第一次完整矩阵因旧 `ext2write` 仍断言“子目录内创建应失败”而失败。该检查改成创建、写入/完整读回 3073 字节、删除并确认消失后，第二轮通过；没有放宽宿主分配计数或删掉检查。

功能整合时的原样补验使用新版内核与旧赛题用户镜像。最终交付中的 `demo/templates` 则已重新制作：使用当前内核、当前全部 45 个用户程序和原比赛输入，新建数据卷，并再次执行 `demo/check.py`。它们是不同轮次，不能混淆来源。迁移工具后的 joint/MBR 验证与最终展示核验另见 `validation/delivery.json`。

## 复现

先按 README 构建所需架构。运行器不会自行安装工具，输出目录必须不存在，且不得使用 `python -O`：

```bash
python3 xv6-riscv/tests/run_acceptance.py --self-test
python3 xv6-riscv/tests/run_acceptance.py --arch riscv --source "$PWD/xv6-riscv" --out "$PWD/.run/full-riscv-new"
python3 xv6-riscv/tests/run_new_feature_acceptance.py --arch riscv --source "$PWD/xv6-riscv" --out "$PWD/.run/features-riscv-new"
python3 xv6-riscv/tests/run_fat32_extra.py --arch riscv --source "$PWD/xv6-riscv" --out "$PWD/.run/extra-riscv-new"
```

另一平台将 `--arch riscv` 改为 `--arch loongarch`，同时更换输出目录。运行器优先使用项目相对路径下的便携工具；也可按各脚本帮助指定已有工具。完整原生测试耗时较长，演示不要求现场执行它。

`demo/check.py --arch all` 使用副本核对原题五条明确成功行、三份完整 96 字节内容、同步与宿主 fsck，并确认模板哈希未变。原题只直接覆盖原生文件系统与 ext2 的复制，不直接覆盖 FAT32、平台移植或断电恢复。

仍未实现手动挂载、日志与任意断电恢复、LoongArch 多核；ext2/FAT32 的格式及操作限制见 README。已删除但仍打开的对象可读，本阶段拒绝继续写入/截断，不承诺完整 POSIX 语义。错误后停写与保留分配是避免进一步损坏的措施，不等于数据回滚保证。
