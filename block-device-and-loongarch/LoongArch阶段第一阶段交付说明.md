# LoongArch 第一阶段交付说明

## 交付范围

本阶段按统一架构约定，先验证平台启动事实，不改动 `xv6-riscv/` 的公共内核文件。交付目录为 `loongarch-probe/`，包含：

- 交叉编译器构建入口、链接脚本和最小 C/汇编入口；
- QEMU `virt` 串口启动探查；
- `syscall 0` 到 `CSR.EENTRY` 的异常入口探查；
- `CSR.ECFG`、`CSR.TCFG`、`CRMD.IE` 定时器中断探查；
- 32 个通用寄存器保存、`CSR.ERA` 推进、恢复和 `ertn` 返回探查；
- 两个独立栈之间的 `ra/sp/fp/s0-s8` 上下文切换探查；
- 构建日志和 QEMU 串口日志。

## 已验证结果

| 项目 | 结果 | 证据 |
|---|---|---|
| GCC 交叉编译器 | GCC 13.0.0（20220906 experimental） | `loongarch-probe/evidence/build.log` |
| ELF 构建 | Machine=LoongArch，入口 `0x9000000000200000` | `loongarch-probe/evidence/build.log` |
| QEMU 启动与串口 | 输出 `loongarch probe: hello` | `loongarch-probe/evidence/qemu.log` |
| 软件异常入口 | 输出 `exception probe: before syscall` 后输出 `E` | `loongarch-probe/evidence/exception-qemu.log` |
| 定时器中断入口 | 输出 `T` | `loongarch-probe/evidence/timer-qemu.log` |
| 寄存器保存与 `ertn` 返回 | 输出 `R` 后继续执行异常后的代码 | `loongarch-probe/evidence/return-qemu.log` |
| 上下文切换 | `main → worker → main resumed → worker resumed → complete` | `loongarch-probe/evidence/context-qemu.log` |

## 复现方法

工具链放在仓库外，不上传 GitHub。Ubuntu/WSL 中执行：

```sh
export PATH=/mnt/d/gpt_work/高级操作系统/tools/loongarch-cross/root/cross-tools/bin:$PATH
cd block-device-and-loongarch/loongarch-probe
make
make exception
make timer
make return
make context
```

QEMU 运行命令和预期输出见该目录的 README。探查程序会在输出后停住，使用 `timeout 5` 结束属于正常行为。

## 边界与后续

本交付不宣称 LoongArch xv6 已完成移植。探查目录已有独立的栈、保存/恢复和上下文切换样例，但尚未实现完整异常码分派、页表、用户态、系统调用和 VirtIO。下一阶段先把这些平台机制封装成独立入口，再与 A/B 的公共接口联调。

