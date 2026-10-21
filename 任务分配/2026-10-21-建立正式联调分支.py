"""Create the authorized integration worktree from verified local contributions.

Run once under WSL. Existing branches/directories are never replaced.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path('/mnt/d/AdvancedOS')
TASK = ROOT / '任务分配'
PACKAGE = TASK / '队友独立成果-20261020'
OUT = TASK / '2026-10-21-正式联调验证'
MAIN = Path('/home/jelians/AdvancedOS')
WORK = Path('/home/jelians/AdvancedOS-integration')
BRANCH = 'codex/integration'
A = '9f6add06599766a88a2dd3b65643f03ef873b312'
B = ROOT / '队友进展/oswork/AdvancedOS-on-Ubuntu'
C = ROOT / '队友进展/AdvancedOS-on-Ubuntu'


def git(repo, *args):
    return subprocess.check_output(
        ['git', '-c', 'core.quotePath=false', '-C', str(repo), *args])


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(data):
    (OUT / '接入清单.json').write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + '\n')


def commit(paths, message):
    staged_before = set(git(WORK, 'diff', '--cached', '--name-only', '-z').decode().split('\0')) - {''}
    assert not staged_before or staged_before == set(paths)
    # mkfs.c and historical evidence can match upstream ignore rules.
    git(WORK, 'add', '-f', '--', *paths)
    staged = set(git(WORK, 'diff', '--cached', '--name-only', '-z').decode().split('\0')) - {''}
    assert staged == set(paths), (staged, paths)
    check = subprocess.run(['git', '-c', 'core.quotePath=false', '-C', str(WORK), 'diff', '--cached', '--check'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with (OUT / '格式检查.log').open('ab') as log:
        log.write((message.splitlines()[0] + '\n').encode() + check.stdout)
    if check.returncode:
        # Imported evidence and original mkfs retain their source bytes.
        # Keep failed format diagnostics; accept only byte-proven inherited
        # whitespace, never an unknown diagnostic or a modified source file.
        manifest = json.loads((PACKAGE / '提取与校验清单.json').read_text())
        imported = {r['source_path']: r for r in manifest['files']
                    if '/新增文件/' in r['output'] or r['output'].startswith('基线补全/')}
        for line in check.stdout.decode().splitlines():
            if line.startswith('+'):
                continue
            match = re.fullmatch(r'(.+):\d+: (trailing whitespace\.|new blank line at EOF\.)', line)
            assert match, line
            relative = match[1]
            row = imported[relative]
            assert sha(WORK / relative) == row['sha256'], relative
            assert git(WORK, 'show', ':' + relative) == (PACKAGE / row['output']).read_bytes()
    body = OUT / 'commit-message.txt'
    body.write_text(message + '\n')
    with (OUT / '接入提交.log').open('ab') as log:
        subprocess.run(['git', '-C', str(WORK), 'commit', '--file', str(body)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    head = git(WORK, 'rev-parse', 'HEAD').decode().strip()
    print(message.splitlines()[0] + ': ' + head, flush=True)
    return head


def copy_new(source, target, repo, revision, relative):
    assert not target.exists(), f'Refusing to overwrite {target}'
    expected = git(repo, 'show', revision + ':' + relative)
    assert source.read_bytes() == expected, relative
    mode = git(repo, 'ls-tree', revision, '--', relative).decode().split()[0]
    assert mode in ('100644', '100755'), (relative, mode)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o755 if mode == '100755' else 0o644)


def main():
    manifest = json.loads((PACKAGE / '提取与校验清单.json').read_text())
    assert manifest['A_base'] == A
    assert git(MAIN, 'branch', '--show-current').decode().strip() == 'codex/vfs'
    assert git(MAIN, 'rev-parse', 'HEAD').decode().strip() == A
    assert not git(MAIN, 'diff', '--name-only')
    assert not git(MAIN, 'diff', '--cached', '--name-only')
    resume = sys.argv[1:] in (['--resume-original-mkfs'], ['--resume-C-evidence-whitespace'])
    resume_c = sys.argv[1:] == ['--resume-C-evidence-whitespace']
    assert resume or not sys.argv[1:]
    if resume:
        state = json.loads((OUT / '接入清单.json').read_text())
        assert state['status'] == 'preflight_passed'
        assert git(WORK, 'branch', '--show-current').decode().strip() == BRANCH
        if resume_c:
            assert len(state['commits']) == 1
            assert git(WORK, 'rev-parse', 'HEAD').decode().strip() == state['commits'][0]['commit']
        else:
            assert not state['commits']
            assert git(WORK, 'rev-parse', 'HEAD').decode().strip() == A
            assert git(WORK, 'status', '--porcelain').decode() == 'A  xv6-riscv/mkfs/mkfs.c\n'
    else:
        assert not git(MAIN, 'branch', '--list', BRANCH)
        assert not WORK.exists() and not OUT.exists()
    assert not (MAIN / 'xv6-riscv/.git').exists()
    # Resolve the configured local commit identity without exposing it in logs.
    git(MAIN, 'var', 'GIT_AUTHOR_IDENT')
    for repo, head in [(B, manifest['B_head']), (C, manifest['C_head'])]:
        assert git(repo, 'rev-parse', 'HEAD').decode().strip() == head
        assert not git(repo, '-c', 'core.filemode=false', '-c', 'core.autocrlf=true',
                       'status', '--porcelain')
    for row in manifest['files'] + manifest['patches']:
        assert sha(PACKAGE / row['output']) == row['sha256'], row['output']
    checksums = TASK / '队友独立成果-20261020-文件校验.sha256'
    for line in checksums.read_text().splitlines():
        expected, name = line.split('  ', 1)
        assert sha(TASK / name) == expected, name
    archive_hash = (TASK / '队友独立成果-20261020.zip.sha256').read_text().split()[0]
    assert sha(TASK / '队友独立成果-20261020.zip') == archive_hash
    if not resume:
        OUT.mkdir()
        state = {
        'branch': BRANCH, 'main': str(MAIN), 'worktree': str(WORK),
        'A_base': A, 'B_source': manifest['B_head'], 'C_source': manifest['C_head'],
        'archive_sha256': archive_hash,
        'main_status_before': git(MAIN, 'status', '--porcelain').decode(),
        'main_fs_image_sha256_before': sha(MAIN / 'xv6-riscv/fs.img'),
        'commits': [], 'status': 'preflight_passed',
    }
        save(state)
        git(MAIN, 'worktree', 'add', '-b', BRANCH, str(WORK), A)
        assert not git(WORK, 'status', '--porcelain')
    relative = 'xv6-riscv/mkfs/mkfs.c'
    if resume:
        assert (WORK / relative).read_bytes() == (PACKAGE / '基线补全' / relative).read_bytes()
    else:
        copy_new(PACKAGE / '基线补全' / relative, WORK / relative,
                 C, manifest['C_head'], relative)
    if not resume_c:
        state['commits'].append({'step': 'original_mkfs', 'commit': commit([relative],
            'Restore original mkfs source for reproducible builds\n\n'
            'Original xv6 build source, identical to the verified baseline copy.\n'
            'This is not new teammate functionality.')})
        save(state)
    for owner, repo, head, title in [
        ('C-HAL与LoongArch', C, manifest['C_head'],
         'Integrate C multi-disk D1 fixes and LoongArch probes'),
        ('B-ext2', B, manifest['B_head'],
         'Integrate B read-only ext2 backend on the VFS baseline')]:
        additions = PACKAGE / owner / '新增文件'
        added = []
        for source in sorted(additions.rglob('*')):
            if source.is_file():
                relative = source.relative_to(additions).as_posix()
                if resume_c and owner == 'C-HAL与LoongArch':
                    assert (WORK / relative).read_bytes() == source.read_bytes()
                    assert source.read_bytes() == git(repo, 'show', head + ':' + relative)
                else:
                    copy_new(source, WORK / relative, repo, head, relative)
                added.append(relative)
        patch = next(row for row in manifest['patches'] if row['owner'] == owner)
        patch_path = str(PACKAGE / patch['output'])
        if resume_c and owner == 'C-HAL与LoongArch':
            git(WORK, 'apply', '--reverse', '--check', patch_path)
        else:
            git(WORK, 'apply', '--check', patch_path)
            git(WORK, 'apply', patch_path)
        state['commits'].append({'step': owner, 'commit': commit(
            added + patch['paths'], title + '\n\n'
            'Import only the independently extracted additions and shared-file delta.\n'
            f'Source-Commit: {head}\n'
            'Existing VFS and original code are not re-imported.\n'
            'Known ext2 directory and metadata issues remain pending G1 acceptance.')})
        save(state)
    names = git(B, 'ls-tree', '-r', '--name-only', manifest['B_head'], '--',
                'xv6-riscv').decode().splitlines()
    updates = set(manifest['C_updates_over_B_inherited_version'])
    hashes = {}
    for relative in names:
        repo, head = (C, manifest['C_head']) if relative in updates else (B, manifest['B_head'])
        expected = git(repo, 'show', head + ':' + relative)
        assert (WORK / relative).read_bytes() == expected, relative
        hashes[relative] = sha(WORK / relative)
    tracked = git(WORK, 'ls-files', 'xv6-riscv').decode().splitlines()
    assert set(tracked) == set(names)
    assert not git(WORK, 'status', '--porcelain')
    assert git(MAIN, 'status', '--porcelain').decode() == state['main_status_before']
    assert sha(MAIN / 'xv6-riscv/fs.img') == state['main_fs_image_sha256_before']
    assert git(MAIN, 'rev-parse', 'HEAD').decode().strip() == A
    state.update(status='source_integrated', source_files_matched=len(hashes),
                 source_sha256=hashes, source_commit=git(WORK, 'rev-parse', 'HEAD').decode().strip(),
                 main_unchanged=True, pushed=False)
    save(state)
    print(f'Integration source verified: {len(hashes)} files; original worktree unchanged.', flush=True)


if __name__ == '__main__':
    main()
