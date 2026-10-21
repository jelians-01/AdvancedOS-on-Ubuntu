"""Validate the committed integration baseline using fresh disposable images."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
TASK = Path('/mnt/d/AdvancedOS/任务分配')
OUT = TASK / '2026-10-21-正式联调验证'
WORK = Path('/home/jelians/AdvancedOS-integration')
MAIN = Path('/home/jelians/AdvancedOS')
SOURCE = WORK / 'xv6-riscv'
DATA = SOURCE / 'test-output/integration-20261021'
spec = importlib.util.spec_from_file_location('review', TASK / '2026-10-20-oswork复核.py')
review = importlib.util.module_from_spec(spec)
spec.loader.exec_module(review)
review.OUT = OUT
review.PREFIX = '正式联调'


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args]).decode().strip()


def main():
    state = json.loads((OUT / '接入清单.json').read_text())
    assert state['status'] == 'source_integrated'
    assert git(WORK, 'rev-parse', 'HEAD') == state['source_commit']
    assert git(WORK, 'branch', '--show-current') == 'codex/integration'
    assert not git(WORK, 'status', '--porcelain')
    assert not DATA.exists()
    assert not (OUT / '测试结果.json').exists()
    DATA.mkdir(parents=True)
    (DATA / 'xv6-riscv').symlink_to(SOURCE, target_is_directory=True)
    result = {'commit': state['source_commit'], 'worktree': str(WORK),
              'data_directory': str(DATA), 'cpu_count': 3, 'memory': '128M',
              'status': 'running', 'checks': {}}

    def save():
        result['log_sha256'] = {p.name: review.sha(p) for p in OUT.glob('*.log')}
        (OUT / '测试结果.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')

    def require(name, condition):
        result['checks'][name] = 'PASS' if condition else 'FAIL'
        save()
        if not condition:
            raise RuntimeError(name + ' failed; see saved evidence')

    save()
    for command in [['riscv64-linux-gnu-gcc', '--version'],
                    ['qemu-system-riscv64', '--version'], ['mke2fs', '-V']]:
        review.run(command)
    review.run(['make', '-j4', 'TOOLPREFIX=riscv64-linux-gnu-', 'kernel/kernel', 'fs.img'], SOURCE)
    result['checks']['build'] = 'PASS'
    image = DATA / 'readonly.ext2'
    with image.open('xb') as file:
        file.truncate(32 * 1024 * 1024)
    review.run(['mke2fs', '-t', 'ext2', '-F', '-b', '1024', '-I', '128', '-O', 'none', str(image)])
    (DATA / 'hello.txt').write_bytes(b'hello from ext2\n')
    (DATA / 'multi.bin').write_bytes(bytes(i % 251 for i in range(3073)))
    for name in ['hello.txt', 'multi.bin']:
        review.run(['debugfs', '-w', '-R', f'write {DATA / name} /{name}', str(image)])
    review.run(['e2fsck', '-fn', str(image)])
    result['image'] = {'path': str(image), 'size': image.stat().st_size,
                       'block_size': 1024, 'inode_size': 128, 'features': 'none',
                       'hello_bytes': 16, 'multi_bytes': 3073,
                       'multi_pattern': 'byte[i] = i % 251', 'sha256_before': review.sha(image)}
    save()
    print('Build passed; testing committed source with a real ext2 volume.', flush=True)
    result['mounted'] = review.guest(DATA, image, 'mounted', [
        ('ext2test', 60), ('ls /mnt', 30), ('cat /mnt/hello.txt', 30),
        ('wc /mnt/multi.bin', 30), ('ls /mnt/..', 30),
        ('cd /mnt', 30), ('/cat hello.txt', 30), ('cd ..', 30), ('cat README', 30),
        ('vfsdir', 90), ('vfspath', 90), ('vfsexec', 90), ('usertests', 1800)])
    save()
    commands = result['mounted']['commands']
    require('mounted_shell_commands_complete', len(commands) == 14 and
            all(row['status'] == 'prompt' for row in commands.values()))
    for name in ['vfs instances', 'vfs paths', 'vfs mounts']:
        require(name, name + ': ALL TESTS PASSED' in commands['boot']['output'])
    for name in ['vfsdir', 'vfspath', 'vfsexec']:
        require(name, name + ': ALL TESTS PASSED' in commands[name]['output'])
    native = commands['usertests']['output']
    result['native_test_names'] = re.findall(r'(?:^|\n)test (\S+):', native)
    require('native_full_69', len(result['native_test_names']) == 69 and
            'ALL TESTS PASSED' in native and 'FAILED' not in native)
    require('hello_absolute_and_relative', all('hello from ext2\n' in
            commands[name]['output'].replace('\r', '') for name in
            ['cat /mnt/hello.txt', '/cat hello.txt']))
    require('multi_file_length', '3073 /mnt/multi.bin' in commands['wc /mnt/multi.bin']['output'])
    require('mount_parent_traversal', 'README' in commands['ls /mnt/..']['output'] and
            'xv6' in commands['cat README']['output'])
    result['image']['sha256_after'] = review.sha(image)
    require('ext2_image_unchanged', result['image']['sha256_before'] == result['image']['sha256_after'])
    result['known_failures'] = {
        'directory_enumeration': 'FAIL: root directory enumeration' in commands['ext2test']['output'],
        'ls_mounted_root': 'cannot read directory' in commands['ls /mnt']['output'],
    }
    require('known_directory_failures_reproduced', all(result['known_failures'].values()))
    result['single_root'] = review.guest(DATA, None, 'single-root', [('usertests copyin', 60)])
    require('single_root_compatibility', 'ALL TESTS PASSED' in
            result['single_root']['commands'].get('usertests copyin', {}).get('output', ''))
    bad = DATA / 'bad-root-table.ext2'
    shutil.copyfile(image, bad)
    with bad.open('r+b') as file:
        file.seek(2 * 1024 + 8)
        file.write(b'\0' * 4)
    result['bad_root_table'] = review.guest(DATA, bad, 'bad-root-table', [])
    bad_boot = result['bad_root_table']['commands']['boot']
    result['known_failures']['bad_inode_table_panic'] = (
        bad_boot['status'] == 'panic' and 'panic: ext2 inode read' in
        (OUT / '正式联调-bad-root-table.log').read_text())
    require('known_metadata_panic_reproduced', result['known_failures']['bad_inode_table_panic'])
    require('source_unchanged', not git(WORK, 'status', '--porcelain') and all(
        review.sha(WORK / path) == expected for path, expected in state['source_sha256'].items()))
    require('main_unchanged', git(MAIN, 'rev-parse', 'HEAD') == state['A_base'] and
            git(MAIN, 'status', '--porcelain') == state['main_status_before'].strip() and
            review.sha(MAIN / 'xv6-riscv/fs.img') == state['main_fs_image_sha256_before'])
    result['status'] = 'integration_baseline_verified_G1_blocked'
    result['not_run'] = ['D1 matrix (same 99 source files verified on 2026-10-20)',
                         'LoongArch probes or full port', 'full ext2 error/concurrency matrix']
    save()
    print('VFS and 69 native tests passed; existing ext2 failures reproduced; G1 remains blocked.', flush=True)


if __name__ == '__main__':
    main()
