"""Disposable ext2 VFS tests; build first. Supports split WSL tool installations.

  python3 tests/run_ext2_tests.py prepare /absolute/output/directory
  python3 tests/run_ext2_tests.py run     /absolute/output/directory
  python3 tests/run_ext2_tests.py check   /absolute/output/directory

prepare/check need e2fsprogs. run needs QEMU >= 7.2. No third-party Python modules.
Never operates on the user's base.ext2. Logs, hashes and tool versions stay in out.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import selectors
import shutil
import subprocess
import time


SOURCE = Path(__file__).resolve().parents[1]


def command(args, log):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    with log.open('ab') as handle:
        handle.write(('> ' + ' '.join(map(str, args)) + '\n').encode())
        handle.write(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exited {result.returncode}: {result.stdout.decode(errors="replace")}')
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(out):
    out.mkdir(parents=True, exist_ok=False)
    for name in ('kernel/kernel', 'fs.img'):
        shutil.copyfile(SOURCE / name, out / Path(name).name)
    log = out / 'prepare.log'
    image = out / 'ext2.img'
    with image.open('wb') as f:
        f.truncate(32 * 1024 * 1024)
    command(['mke2fs', '-t', 'ext2', '-F', '-b', '1024', '-I', '128', '-O', 'none', image], log)
    fixtures = {'hello.txt': b'hello from ext2\n',
                'multi.bin': bytes(i % 251 for i in range(3073)),
                'indirect.bin': bytes(i % 251 for i in range(13000)),
                'double.bin': bytes(i % 251 for i in range(274433))}
    for name, data in fixtures.items():
        host = out / name
        host.write_bytes(data)
        command(['debugfs', '-w', '-R', f'write {host} /{name}', image], log)
    command(['e2fsck', '-fn', image], log)
    manifest = {'format': 'ext2,32MiB,1KiB blocks,128-byte inodes,-O none',
                'cpu_count': 3, 'machine': 'virt', 'cache': 'writeback',
                'kernel_sha256': digest(out / 'kernel'),
                'initial_image_sha256': digest(image),
                'initial_super_free': super_free(image),
                'results': []}
    manifest['edge_initial_free'] = {}
    for kind in ('blocks', 'inodes', 'directory', 'zero', 'one',
                 'dirlimit', 'dirblocks', 'dirinodes'):
        edge = out / f'{kind}.img'
        with edge.open('wb') as f:
            f.truncate((1024 if kind == 'dirlimit' else 128) * 1024)
        command(['mke2fs', '-t', 'ext2', '-F', '-b', '1024', '-I', '128',
                 '-O', 'none', '-m', '0', '-N',
                 '16' if kind == 'inodes' else '24' if kind == 'dirinodes' else
                 '256' if kind == 'dirlimit' else '64', edge], log)
        if kind == 'dirinodes':
            empty = out / 'empty'
            empty.write_bytes(b'')
            # 24 inodes minus 11 reserved and 6 fixtures leaves 7. Seven
            # 120-byte names then fill this directory exactly to its next block.
            for i in range(6):
                command(['debugfs', '-w', '-R', f'write {empty} /i{i}', edge], log)
        command(['e2fsck', '-fn', edge], log)
        manifest['edge_initial_free'][kind] = super_free(edge)
        if kind in ('zero', 'one'):
            assert super_free(edge) == [102, 53], 'transition fixture geometry changed'
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))


def super_free(image):
    with image.open('rb') as f:
        f.seek(1024 + 12)
        data = f.read(8)
    return [int.from_bytes(data[:4], 'little'), int.from_bytes(data[4:], 'little')]


def guest(out, name, commands, readonly=False, image='ext2.img'):
    qemu = os.environ.get('XV6_QEMU', 'qemu-system-riscv64')
    args = [qemu, '-machine', 'virt', '-bios', 'none', '-kernel', str(out / 'kernel'),
            '-m', '128M', '-smp', '3', '-display', 'none', '-serial', 'stdio',
            '-monitor', 'none', '-global', 'virtio-mmio.force-legacy=false']
    for slot, filename in enumerate(('fs.img', image)):
        drive = f'file={out / filename},if=none,format=raw,id=d{slot},cache=writeback'
        if readonly and slot == 1:
            drive += ',readonly=on'
        args += ['-drive', drive, '-device',
                 f'virtio-blk-device,drive=d{slot},bus=virtio-mmio-bus.{slot}']
    p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    poller = selectors.DefaultSelector()
    poller.register(p.stdout, selectors.EVENT_READ)
    transcript = bytearray()
    cursor = 0
    with (out / f'{name}.log').open('wb') as log:
        def until_prompt(timeout):
            nonlocal cursor
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                end = transcript.find(b'$ ', cursor)
                if end >= 0:
                    result = bytes(transcript[cursor:end + 2])
                    cursor = end + 2
                    if b'panic:' in result:
                        raise RuntimeError(result.decode(errors='replace'))
                    return result
                for key, _ in poller.select(0.2):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError(f'QEMU exited; see {name}.log')
                    transcript.extend(chunk)
                    log.write(chunk)
                    log.flush()
            raise TimeoutError(f'guest prompt; see {name}.log')
        try:
            until_prompt(45)
            for cmd, expected in commands:
                p.stdin.write(cmd.encode() + b'\n')
                p.stdin.flush()
                output = until_prompt(300 if cmd == 'ext2single' else 120)
                if expected.encode() not in output:
                    raise RuntimeError(f'{cmd} failed: {output.decode(errors="replace")}')
                print(f'{name}: {cmd}: PASS', flush=True)
        finally:
            # Backend callbacks flush synchronously. Terminate only this child,
            # after the guest prompt; do not kill unrelated QEMU instances.
            if p.poll() is None:
                p.terminate()
            try:
                tail, _ = p.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                tail, _ = p.communicate(timeout=10)
            log.write(tail)
            poller.close()


def run(out):
    manifest = json.loads((out / 'manifest.json').read_text())
    manifest['qemu'] = subprocess.check_output(
        [os.environ.get('XV6_QEMU', 'qemu-system-riscv64'), '--version'], text=True).splitlines()[0]
    guest(out, 'write', [('ext2test', 'ext2test: ALL TESTS PASSED'),
                         ('ext2write', 'ext2write: ALL TESTS PASSED'),
                         ('ext2single', 'ext2single: ALL TESTS PASSED'),
                         ('ext2test', 'ext2test: ALL TESTS PASSED'),
                         ('usertests copyin', 'ALL TESTS PASSED'),
                         ('usertests truncate2', 'ALL TESTS PASSED'),
                         ('usertests sharedfd', 'ALL TESTS PASSED')])
    written_hash = digest(out / 'ext2.img')
    manifest['results'].append('write and native targeted regression: PASS')
    guest(out, 'reboot', [('ext2write verify', 'ext2write: PERSISTENCE PASSED'),
                          ('ext2single verify', 'ext2single: PERSISTENCE PASSED'),
                          ('ext2test', 'ext2test: ALL TESTS PASSED')])
    assert digest(out / 'ext2.img') == written_hash, 'reboot verification changed data image'
    manifest['results'].append('same-image reboot readback: PASS')
    guest(out, 'readonly', [('ext2test', 'ext2test: ALL TESTS PASSED'),
                            ('echo forbidden > /mnt/b-copy.bin', 'open /mnt/b-copy.bin failed'),
                            ('ext2write verify', 'ext2write: PERSISTENCE PASSED'),
                            ('ext2single verify', 'ext2single: PERSISTENCE PASSED')], readonly=True)
    assert digest(out / 'ext2.img') == written_hash, 'readonly disk changed'
    manifest['results'].append('readonly device refusal and unchanged hash: PASS')
    manifest['final_image_sha256'] = written_hash
    for kind in ('blocks', 'inodes', 'directory'):
        guest(out, kind, [(f'ext2write {kind}', f'ext2write: SPACE {kind} PASSED')],
              image=f'{kind}.img')
        manifest['results'].append(f'{kind} exhaustion/retry: PASS')
    for kind in ('zero', 'one'):
        guest(out, kind, [(f'ext2single {kind}', 'ext2single: TRANSITION PASSED')],
              image=f'{kind}.img')
        manifest['results'].append(f'indirect transition with {kind} free blocks: PASS')
    for kind, mode in (('dirlimit', ''), ('dirblocks', 'blocks'), ('dirinodes', 'inodes')):
        guest(out, kind, [(f'ext2dirgrow {mode}'.rstrip(),
                          f'ext2dirgrow: {"LIMIT" if not mode else mode.upper()} PASSED')],
              image=f'{kind}.img')
        manifest['results'].append(f'{kind} growth/limit/duplicate create: PASS')
    growth_hash = digest(out / 'dirlimit.img')
    guest(out, 'dirreboot', [('ext2dirgrow verify', 'ext2dirgrow: PERSISTENCE PASSED')],
          image='dirlimit.img')
    assert digest(out / 'dirlimit.img') == growth_hash, 'growth reboot changed image'
    manifest['results'].append('expanded-directory same-image reboot: PASS')
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))


def check(out):
    log = out / 'check.log'
    command(['e2fsck', '-fn', out / 'ext2.img'], log)
    payload = bytes(i % 251 for i in range(274433))
    expected = {'b-copy.bin': payload[:3073], 'b-empty': b'', 'b-short': b'short\n',
                'b-limit': payload[:12288], 'b-overwrite': b'XYZ' + payload[3:3073], 'b-badptr': b'',
                'hello.txt': b'hello from ext2\n', 'multi.bin': payload[:3073],
                'indirect.bin': payload[:13000], 'double.bin': payload,
                's-max': payload[:274432], 's-copy': payload[:30001],
                's-empty': b'', 's-short': payload[:6],
                's-edge': payload[:12285] + b'BOUNDARY'}
    for name, data in expected.items():
        target = out / f'export-{name}'
        command(['debugfs', '-R', f'dump /{name} {target}', out / 'ext2.img'], log)
        assert target.read_bytes() == data, f'{name}: host byte comparison failed'
    manifest = json.loads((out / 'manifest.json').read_text())
    before = manifest['initial_super_free']
    after = super_free(out / 'ext2.img')
    # Previous 21 blocks + (269 max + 31 copy + 1 short + 14 edge).
    # Includes index blocks. Rewritten imported inode has no net change.
    assert before[0] - after[0] == 336 and before[1] - after[1] == 11, (before, after)
    manifest['final_super_free'] = after
    manifest['results'] += ['e2fsck -fn: PASS', 'all host byte comparisons: PASS',
                            'exact allocation/reclamation counters: PASS']
    for kind in ('blocks', 'inodes', 'directory'):
        edge = out / f'{kind}.img'
        command(['e2fsck', '-fn', edge], log)
        before_edge = manifest['edge_initial_free'][kind]
        after_edge = super_free(edge)
        count = int(re.search(r'PASSED \((\d+) files\)',
                             (out / f'{kind}.log').read_text()).group(1))
        if kind == 'blocks':
            assert after_edge[0] == 0 and before_edge[1] - after_edge[1] == count + 1
        elif kind == 'directory':
            assert count == 40
            assert before_edge[0] - after_edge[0] == 4
            assert before_edge[1] - after_edge[1] == count
        else:
            assert before_edge[0] == after_edge[0]
            assert before_edge[1] - after_edge[1] == count
            if kind == 'inodes':
                assert after_edge[1] == 0
        manifest['results'].append(f'{kind} fsck and exact counters: PASS')
    for kind in ('zero', 'one'):
        edge = out / f'{kind}.img'
        command(['e2fsck', '-fn', edge], log)
        before_edge = manifest['edge_initial_free'][kind]
        after_edge = super_free(edge)
        # Target: 13 data + 1 index; filler is fully truncated. The one-block
        # case also consumes that last returned block in /last.
        extra = int(kind == 'one')
        assert before_edge[0] - after_edge[0] == 14 + extra
        assert before_edge[1] - after_edge[1] == 2 + extra
        for name, data in {'target': payload[:12289], 'filler': b'',
                           **({'last': payload[:1024]} if extra else {})}.items():
            target = out / f'export-{kind}-{name}'
            command(['debugfs', '-R', f'dump /{name} {target}', edge], log)
            assert target.read_bytes() == data
        manifest['results'].append(f'{kind} transition fsck/bytes/counters: PASS')
    for kind, used_blocks, used_inodes, root_size, count, length in (
            ('dirlimit', 119, 108, 12288, 108, 96),
            ('dirblocks', 1, 8, 2048, 8, 120),
            ('dirinodes', 0, 7, 1024, 7, 120)):
        edge = out / f'{kind}.img'
        command(['e2fsck', '-fn', edge], log)
        before_edge = manifest['edge_initial_free'][kind]
        after_edge = super_free(edge)
        assert before_edge[0] - after_edge[0] == used_blocks, (kind, before_edge, after_edge)
        assert before_edge[1] - after_edge[1] == used_inodes, (kind, before_edge, after_edge)
        root_info = command(['debugfs', '-R', 'stat /', edge], log).decode()
        assert re.search(r'Size:\s+' + str(root_size) + r'\b', root_info)
        assert re.search(r'Blockcount:\s+' + str(root_size // 512) + r'\b', root_info)
        for i in range(count):
            name = f'g{i:03d}' + 'a' * (length - 4)
            target = out / f'export-{kind}-{i}'
            command(['debugfs', '-R', f'dump /{name} {target}', edge], log)
            assert target.read_bytes() == (bytes([i]) if kind == 'dirlimit' else b'')
        manifest['results'].append(f'{kind} fsck/all bytes/exact counters: PASS')
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print('EXT2 WRITE TEST MATRIX PASSED', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('prepare', 'run', 'check'))
    parser.add_argument('out', type=Path)
    options = parser.parse_args()
    globals()[options.phase](options.out.resolve())
