"""Finish evidence checks, record integration state, and commit local handoff docs."""
import ast
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path('/mnt/d/AdvancedOS')
TASK = ROOT / '任务分配'
OUT = TASK / '2026-10-21-正式联调验证'
WORK = Path('/home/jelians/AdvancedOS-integration')
MAIN = Path('/home/jelians/AdvancedOS')
REPORT = '2026-10-21-01-正式联调分支建立与验证.md'


def git(repo, *args):
    return subprocess.check_output(['git', '-c', 'core.quotePath=false', '-C', str(repo), *args]).decode()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def insert_after_title(path, entry):
    content = path.read_text(encoding='utf-8-sig')
    assert entry not in content
    title, rest = content.split('\n', 1)
    path.write_text(title + '\n\n' + entry + '\n' + rest)


state = json.loads((OUT / '接入清单.json').read_text())
result_path = OUT / '测试结果.json'
result = json.loads(result_path.read_text())
assert not (OUT / '最终状态.json').exists()
assert result['checks']['native_full_69'] == 'PASS'
assert len(result['native_test_names']) == 69
assert result['checks']['hello_absolute_and_relative'] == 'PASS'
assert result['bad_root_table']['commands']['boot']['status'] == 'panic'
assert 'panic: ext2 inode read' in (OUT / '正式联调-bad-root-table.log').read_text()
assert git(WORK, 'rev-parse', 'HEAD').strip() == state['source_commit']
assert not git(WORK, 'status', '--porcelain')
assert git(MAIN, 'status', '--porcelain') == state['main_status_before']
assert git(MAIN, 'rev-parse', 'HEAD').strip() == state['A_base']
assert sha(MAIN / 'xv6-riscv/fs.img') == state['main_fs_image_sha256_before']
for relative, expected in state['source_sha256'].items():
    assert sha(WORK / relative) == expected
shutil.copyfile(result_path, OUT / 'panic输出分段核对前.json')
result['known_failures']['bad_inode_table_panic'] = True
result['checks']['known_metadata_panic_reproduced'] = 'PASS'
result['checks']['source_unchanged'] = 'PASS'
result['checks']['main_unchanged'] = 'PASS'
result['harness_correction']['panic_capture_note'] = (
    'The helper returns as soon as panic: arrives; the remaining text is saved '
    'in the complete log during process shutdown. Confirmed the full '
    'panic: ext2 inode read line in that original log without repeating the test.')
assert all(value == 'PASS' for value in result['checks'].values())
assert all(result['known_failures'].values())
result['status'] = 'integration_baseline_verified_G1_blocked'
result['not_run'] = ['D1 matrix (same 99 source files verified on 2026-10-20)',
                     'LoongArch probes or full port', 'full ext2 error/concurrency matrix']
result['log_sha256'] = {p.name: sha(p) for p in OUT.glob('*.log')}
dump(result_path, result)

report_path = TASK / REPORT
report = report_path.read_text()
pending = '测试结果正在执行，最终以 [测试结果.json](2026-10-21-正式联调验证/测试结果.json) 和本节完成后的状态为准。'
assert pending in report
report = report.replace(pending, '''| 检查 | 本轮结果 |
|---|---|
| 编译及 ext2 镜像一致性检查 | 通过 |
| 三项 VFS 启动自测 | 通过 |
| vfsdir、vfspath、vfsexec | 全部通过 |
| 挂载真实 ext2 时的完整原生 usertests | 69 项通过，含慢速组 |
| 绝对/相对路径读取 hello.txt、挂载根 `..` 返回原生根 | 通过 |
| multi.bin 读取长度 | 3073 字节；本轮未完成该文件的逐字节内核校验 |
| ext2 镜像测试前后 SHA-256 | 相同 |
| 不接第二盘时启动及 usertests copyin | 通过 |
| ext2test、ls /mnt | 失败：目录枚举问题复现 |
| inode 表指针损坏的 ext2 镜像 | 失败：`panic: ext2 inode read`，内核安全拒绝要求尚未满足 |

**分支建立和基线回归完成，G1 尚未通过。** JSON 中的 `known_*_reproduced: PASS` 仅表示已成功复现缺陷，不表示该 ext2 功能通过验收。

测试脚本首次使用 `cd /mnt` 后的 `cat hello.txt`，因 xv6 shell 直接执行 argv[0] 而报 `exec cat failed`。核对 shell 源码后改为 `/cat hello.txt`，只补跑相对路径检查；已通过的完整 69 项没有重复运行。另一次脚本判定过早读取到 `panic:` 前缀，完整原始日志中已有 `panic: ext2 inode read`，已依据完整日志核对，未改内核或重复坏镜像测试。两次脚本诊断的原记录均保留。

D1 快速矩阵沿用 10 月 20 日对相同 99 个源码文件的验证，本轮没有重跑；LoongArch、完整 ext2 异常/并发矩阵、内核写入和持久化也未在本轮验收。具体判定、命令及日志哈希见 [测试结果.json](2026-10-21-正式联调验证/测试结果.json)。''')
report_path.write_text(report)

entry = '''## 2026-10-21 正式联调分支（最新）

- 接续先读 `任务分配/2026-10-21-01-正式联调分支建立与验证.md`。用户已明确授权直接建立正式联调分支。
- 原项目 `/home/jelians/AdvancedOS` 仍为 `codex/vfs@9f6add0`；正式联调使用同一外层仓库的独立工作树 `/home/jelians/AdvancedOS-integration`、分支 `codex/integration`。原工作树、18 个未跟踪日志及 fs.img 保持不变。
- 已本地分步提交原版 mkfs 补全 d44afc0、C 新版接入 9090728、B ext2 接入 3e5c98d。源码基线 `3e5c98d0ffdde1b3145fce8c30304fd15fc94795`，99 个源码文件与此前提取候选完全一致；另有接续文档提交，不能把文档 HEAD 与源码测试基线混淆。
- 本轮构建、三项 VFS 启动自测、vfsdir/vfspath/vfsexec、挂载真实 ext2 时的完整原生 69 项（含慢速组）、相对路径与单盘 copyin 均通过；ext2 镜像前后哈希一致。目录枚举失败和坏 inode 表 panic 已复现，G1 仍未通过。D1 沿用相同源码的 10 月 20 日快速矩阵，本轮未重跑 LoongArch。
- 下一步 B 修 readdir/元数据错误传播，A 补挂载结果诊断并组织修复后统一验收，C 推进平台。未推送、未合回 VFS/master、未安装依赖；不要重复接入或要求 C 重修 D1。后续设备交接需记录关联工作树，不能只复制其 .git 指针文件。
'''
insert_after_title(ROOT / '.codex/README.md', entry)
insert_after_title(ROOT / '.workbuddy/memory/MEMORY.md', entry)
agent_entry = ('- 2026-10-21 正式联调：最新接续为 `任务分配/' + REPORT +
               '`。Ubuntu `/home/jelians/AdvancedOS-integration` 为同一外层仓库关联工作树，'
               '`codex/integration` 已本地接入原版 mkfs、C 新版、B ext2；源码基线 '
               '`3e5c98d0ffdde1b3145fce8c30304fd15fc94795`，99 文件核对一致。'
               '构建、VFS 专项、真实 ext2 挂载下完整 69 项、相对路径与单盘兼容通过；'
               '目录枚举和坏 inode 表 panic 仍待 B 修复，G1 未通过，A 下一步补挂载诊断。'
               '原 `/home/jelians/AdvancedOS` 保持 codex/vfs@9f6add0，原镜像及日志未动；'
               '未推送或合回主线。D1 沿用相同源码的前轮快速矩阵，本轮未重跑 LoongArch。'
               '交接时需处理关联工作树，不能只携带工作树 .git 指针文件。\n')
insert_after_title(ROOT / 'AGENTS.md', agent_entry)
intro = ('## 最新接续：2026-10-21\n\n正式联调分支已建立，见 [' + REPORT[:-3] + '](' + REPORT +
         ')。在 Ubuntu `~/AdvancedOS-integration` 的 `codex/integration` 继续；源码基线 3e5c98d。'
         'VFS 专项、真实 ext2 挂载下完整 69 项与路径/单盘检查通过，G1 仍因 ext2 枚举及元数据 panic 未放行。'
         '原 VFS 主线保留，未推送。下方为历史记录。\n')
insert_after_title(TASK / 'README.md', intro)
notice = ('2026-10-21 正式联调：`codex/integration` 已在 `~/AdvancedOS-integration` 建立，源码基线 '
          '`3e5c98d`。VFS 专项、挂载真实 ext2 时的完整 69 项原生回归、路径及单盘兼容通过；'
          '目录枚举和坏元数据 panic 仍待修，G1 未放行。本轮不变更公共 ABI。见 [正式联调记录](' + REPORT + ')。\n')
for name in ['统一架构与接口约定.md', '联调验收与交付清单.md']:
    insert_after_title(TASK / name, notice)
insert_after_title(ROOT / '双设备交接指南/设备与交接状态.md',
    '2026-10-21 本地开发状态：原 `~/AdvancedOS` 保持 `codex/vfs@9f6add0`；'
    '新增同一仓库的关联工作树 `~/AdvancedOS-integration`、本地分支 `codex/integration`，'
    '源码基线 3e5c98d，另有接续文档提交。三方代码正式接入，VFS 与完整 69 项回归通过，'
    'ext2 仍有枚举与坏元数据问题，G1 未通过。未推送，本轮不是设备交接。'
    '下次交接须包含该分支及需要保留的联调数据，并正确重建或修复关联工作树；'
    '仅复制 AdvancedOS-integration 内的 .git 文件不能形成独立仓库。'
    '详情见 [正式联调记录](../任务分配/' + REPORT + ')。\n')

# Save only the current handoff documentation and evidence in the new branch.
files = ['AGENTS.md', '双设备交接指南/设备与交接状态.md']
files += ['任务分配/' + name for name in [REPORT, '统一架构与接口约定.md',
           '联调验收与交付清单.md', '三人分工与阶段计划.md',
           '2026-10-20-oswork复核.py', '2026-10-21-建立正式联调分支.py',
           '2026-10-21-正式联调验证.py', '2026-10-21-补充联调核验.py',
           '2026-10-21-联调归档与记忆同步.py']]
files += [p.relative_to(ROOT).as_posix() for p in OUT.iterdir()
          if p.suffix in ('.json', '.log')]
for relative in files:
    source, target = ROOT / relative, WORK / relative
    assert not target.exists(), target
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.suffix in ('.md', '.py'):
        target.write_text(source.read_text(encoding='utf-8-sig'))
    else:
        shutil.copyfile(source, target)
    if source.suffix == '.py':
        ast.parse(source.read_text(encoding='utf-8-sig'))

new_docs = {
    '.codex/README.md': '# 正式联调工作树接续\n\n' + entry +
        '\n项目完整历史资料仍保存在 Windows `D:\\AdvancedOS`。本工作树保存本轮记录和共同约定。\n',
    '任务分配/README.md': '# 正式联调资料入口\n\n'
        f'先读 [正式联调记录]({REPORT})，再读 [统一架构](统一架构与接口约定.md) 和 '
        '[验收清单](联调验收与交付清单.md)。\n\n'
        '本目录保存本轮日志和来源核对清单；历史提取包及旧报告位于 Windows '
        '`D:\\AdvancedOS\\任务分配`，统一约定中的旧报告链接请从该完整资料目录打开。\n',
}
for relative, content in new_docs.items():
    target = WORK / relative
    assert not target.exists()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content)
    files.append(relative)
assert not git(WORK, 'diff', '--cached', '--name-only')
git(WORK, 'add', '-f', '--', *files)
staged = set(git(WORK, 'diff', '--cached', '--name-only', '-z').split('\0')) - {''}
assert staged == set(files)
# Raw terminal logs are evidence, so check newly authored prose/scripts separately.
authored = [p for p in files if p.endswith('.py') or p.endswith(REPORT) or
            p in ('任务分配/README.md', '.codex/README.md', 'AGENTS.md')]
git(WORK, 'diff', '--cached', '--check', '--', *authored)
message = OUT / '文档提交说明.txt'
message.write_text('Document integration baseline, validation and remaining ext2 blockers\n\n'
                   'Record the unchanged source baseline and full native/VFS regression.\n'
                   'Preserve ext2 failures and harness diagnostics; G1 remains pending.\n')
with (OUT / '文档提交.log').open('wb') as log:
    subprocess.run(['git', '-C', str(WORK), 'commit', '--file', str(message)],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
assert not git(WORK, 'status', '--porcelain')
assert git(MAIN, 'status', '--porcelain') == state['main_status_before']
assert git(MAIN, 'rev-parse', 'HEAD').strip() == state['A_base']
assert sha(MAIN / 'xv6-riscv/fs.img') == state['main_fs_image_sha256_before']
for relative, expected in state['source_sha256'].items():
    assert sha(WORK / relative) == expected
final = {'branch': 'codex/integration', 'worktree': str(WORK),
         'head': git(WORK, 'rev-parse', 'HEAD').strip(),
         'source_commit': state['source_commit'], 'working_tree_clean': True,
         'main_unchanged': True, 'native_tests_passed': 69, 'G1': 'blocked_by_ext2',
         'pushed': False, 'document_files_committed': len(files)}
dump(OUT / '最终状态.json', final)
print(json.dumps(final, ensure_ascii=False, indent=2))
