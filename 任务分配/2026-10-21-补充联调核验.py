"""Finish checks after correcting the xv6 shell executable path in the harness.

Preserve the first transcript and its failed harness assertion. Do not repeat
the completed 69-test native regression or modify any kernel/user source.
"""
import importlib.util
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
task = Path('/mnt/d/AdvancedOS/任务分配')
spec = importlib.util.spec_from_file_location('validation', task / '2026-10-21-正式联调验证.py')
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)
state = json.loads((v.OUT / '接入清单.json').read_text())
path = v.OUT / '测试结果.json'
result = json.loads(path.read_text())
assert result['checks']['native_full_69'] == 'PASS'
assert result['checks']['hello_absolute_and_relative'] == 'FAIL'
assert 'exec cat failed' in result['mounted']['commands']['cat hello.txt']['output']
assert v.git(v.WORK, 'rev-parse', 'HEAD') == state['source_commit']
assert not v.git(v.WORK, 'status', '--porcelain')
for relative, expected in state['source_sha256'].items():
    assert v.review.sha(v.WORK / relative) == expected
backup = v.OUT / '测试脚本首次检查.json'
assert not backup.exists()
shutil.copyfile(path, backup)
result['harness_correction'] = {
    'initial_assertion': 'hello_absolute_and_relative: FAIL',
    'reason': 'xv6 sh executes argv[0] directly; cat is at /cat, not /mnt/cat',
    'corrected_command': '/cat hello.txt',
    'initial_evidence': backup.name,
    'repeated': 'relative-path check only; completed full native regression retained',
}


def save():
    result['log_sha256'] = {p.name: v.review.sha(p) for p in v.OUT.glob('*.log')}
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')


def require(name, condition):
    result['checks'][name] = 'PASS' if condition else 'FAIL'
    save()
    if not condition:
        raise RuntimeError(name + ' failed')


image = v.DATA / 'readonly.ext2'
result['relative_path_recheck'] = v.review.guest(v.DATA, image, 'relative-path', [
    ('cd /mnt', 30), ('/cat hello.txt', 30), ('cd ..', 30), ('/cat README', 30)])
commands = result['mounted']['commands']
relative = result['relative_path_recheck']['commands']
require('hello_absolute_and_relative',
        'hello from ext2\n' in commands['cat /mnt/hello.txt']['output'].replace('\r', '') and
        'hello from ext2\n' in relative['/cat hello.txt']['output'].replace('\r', '') and
        all(row['status'] == 'prompt' for row in relative.values()))
require('multi_file_length', '3073 /mnt/multi.bin' in commands['wc /mnt/multi.bin']['output'])
require('mount_parent_traversal', 'README' in commands['ls /mnt/..']['output'] and
        'xv6' in relative['/cat README']['output'])
result['image']['sha256_after'] = v.review.sha(image)
require('ext2_image_unchanged', result['image']['sha256_before'] == result['image']['sha256_after'])
result['known_failures'] = {
    'directory_enumeration': 'FAIL: root directory enumeration' in commands['ext2test']['output'],
    'ls_mounted_root': 'cannot read directory' in commands['ls /mnt']['output'],
}
require('known_directory_failures_reproduced', all(result['known_failures'].values()))
result['single_root'] = v.review.guest(v.DATA, None, 'single-root', [('usertests copyin', 60)])
require('single_root_compatibility', 'ALL TESTS PASSED' in
        result['single_root']['commands'].get('usertests copyin', {}).get('output', ''))
bad = v.DATA / 'bad-root-table.ext2'
assert not bad.exists()
shutil.copyfile(image, bad)
with bad.open('r+b') as file:
    file.seek(2 * 1024 + 8)
    file.write(b'\0' * 4)
result['bad_root_table'] = v.review.guest(v.DATA, bad, 'bad-root-table', [])
bad_boot = result['bad_root_table']['commands']['boot']
result['known_failures']['bad_inode_table_panic'] = (
    bad_boot['status'] == 'panic' and 'panic: ext2 inode read' in
    (v.OUT / '正式联调-bad-root-table.log').read_text())
require('known_metadata_panic_reproduced', result['known_failures']['bad_inode_table_panic'])
require('source_unchanged', not v.git(v.WORK, 'status', '--porcelain') and all(
    v.review.sha(v.WORK / relative) == expected for relative, expected in state['source_sha256'].items()))
require('main_unchanged', v.git(v.MAIN, 'rev-parse', 'HEAD') == state['A_base'] and
        v.git(v.MAIN, 'status', '--porcelain') == state['main_status_before'].strip() and
        v.review.sha(v.MAIN / 'xv6-riscv/fs.img') == state['main_fs_image_sha256_before'])
result['status'] = 'integration_baseline_verified_G1_blocked'
result['not_run'] = ['D1 matrix (same 99 source files verified on 2026-10-20)',
                     'LoongArch probes or full port', 'full ext2 error/concurrency matrix']
save()
print('VFS, 69 native tests, relative paths and single-root compatibility passed. G1 blocked by ext2.', flush=True)
