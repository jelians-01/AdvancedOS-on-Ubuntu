# 项目维护约定

- 自然语言回复以“喵，”开头。
- 当前目录是独立 Git 仓库，交付分支为 master；不要恢复内外嵌套仓库。
- 先读 README.md 和 VALIDATION.md；核心源码在 xv6-riscv/，两种架构共享上层实现。
- RISC-V 构建：make -C xv6-riscv -j4 kernel/kernel fs.img。
- LoongArch 构建：make -C xv6-riscv -j4 ARCH=loongarch all；工具默认从 tools/ 获取。
- 展示入口见 demo/README.md。templates 是只读基线，运行只使用 .run/ 内的副本。
- 不把内核自动挂载说成手动 mount，不把正常同步重启说成断电恢复；贡献归属见报告。
- 不修改原比赛输入。测试必须核对明确成功断言和实际文件内容，不能只看退出码。
- 保留有意义的回归；改动后按影响运行适当验证。凭据、工具二进制和运行镜像不提交 Git。
