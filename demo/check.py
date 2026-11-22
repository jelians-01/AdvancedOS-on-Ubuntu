#!/usr/bin/env python3
"""Check the unchanged original contest script and all three 96-byte outputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from run import BASE, FILES, new_session, qemu_command, qemu_path, require, sha, verify_template
from prepare import Guest, NativeImage, host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch', 'all'), default='all')
    parser.add_argument('--out', type=Path, help='new evidence directory; defaults below project .run')
    parser.add_argument('--templates', type=Path, default=BASE / 'templates')
    parser.add_argument('--qemu-riscv')
    parser.add_argument('--qemu-loongarch')
    args = parser.parse_args()
    os.environ['PATH'] = str(BASE.parent / 'tools/bin') + os.pathsep + os.environ.get('PATH', '')
    if args.out:
        out = args.out.resolve()
        out.mkdir(parents=True, exist_ok=False)
    else:
        sessions = BASE.parent / '.run'
        sessions.mkdir(exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix='contest-check-', dir=sessions))
    require(not any(c.isspace() or c == ',' for c in str(out)), 'evidence path must not contain spaces or commas')
    hello = (BASE / 'tests/hello.c').read_bytes()
    script = (BASE / 'tests/cmds.sh').read_bytes()
    require(len(hello) == 96 and len(script) == 73, 'original fixture length mismatch')
    expected = [f'test_copy_file({a},{b}): succeed' for a, b in (
        ('/mnt/hello.c', '/hello.c'), ('/mnt/hello.c', '/tests/hello.c'),
        ('/hello.c', '/mnt/hello.c'), ('/tests/hello.c', '/mnt/hello.c'))]
    expected.append('test_cross_fs_copy(): succeed')
    arches = ('riscv', 'loongarch') if args.arch == 'all' else (args.arch,)
    results = {'status': 'RUNNING', 'scope': 'Current compiled user programs and kernel; original contest inputs',
               'architectures': {}, 'runner_sha256': sha(Path(__file__))}
    try:
        for arch in arches:
            template = args.templates.resolve() / arch
            manifest = verify_template(template, arch)
            for name, expected_hash in manifest['input_sha256'].items():
                require(sha(BASE / 'tests' / name) == expected_hash, 'test input changed: ' + name)
            qemu = qemu_path(arch, BASE.parent, getattr(args, 'qemu_' + arch))
            directory = new_session(template, arch, out)
            record = {'status': 'RUNNING', 'directory': directory.name, 'template': manifest}
            results['architectures'][arch] = record
            command = qemu_command(arch, directory, qemu, automated=True)
            (directory / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
            with Guest(command, directory / 'contest.log') as guest:
                boot = guest.until()
                for line in ('mount xv6 dev=1 /: OK', 'mount ext2 dev=2 /mnt: OK', 'mount fat32 dev=3 /fat: OK'):
                    require(boot.splitlines().count(line) == 1, 'mount missing or duplicated: ' + line)
                text = guest.send('cd /tests')
                require('cannot cd' not in text, 'cannot enter original test directory')
                text = guest.send('cat cmds.sh | /testrun /sh',
                                  pattern=rb'TESTRUN /sh status=-?\d+\r?\n\$ ', timeout=180)
                text = re.sub(r'(?m)^(?:\$ )+', '', text)
                lines = text.splitlines()
                for line in [*expected, 'TESTRUN /sh status=0', 'mkdir: /mnt failed to create']:
                    require(lines.count(line) == 1, 'missing/duplicate required result: ' + line)
                require(not re.search(r'\bFAIL(?:ED)?\b|panic:|test_.*: fail', text), 'explicit contest failure')
                text = guest.send('/testrun /sync')
                require(text.splitlines().count('sync: OK') == 1 and
                        text.splitlines().count('TESTRUN /sync status=0') == 1, 'sync failed')
            native = NativeImage(directory / 'fs.img')
            require(native.read('/tests/cmds.sh') == script, 'original script changed')
            compared = {}
            for path in ('/hello.c', '/tests/hello.c'):
                content = native.read(path)
                require(content == hello, 'native content mismatch: ' + path)
                compared[path] = {'size': len(content), 'sha256': hashlib.sha256(content).hexdigest()}
            exported = directory / 'ext2-hello.c'
            host(['debugfs', '-R', f'dump /hello.c {exported}', directory / 'ext2.img'], directory / 'host.log')
            require(exported.read_bytes() == hello, 'ext2 content mismatch')
            compared['/mnt/hello.c'] = {'size': exported.stat().st_size, 'sha256': sha(exported)}
            require(sha(directory / 'fat32.img') == manifest['sha256']['fat32.img'],
                    'original contest unexpectedly changed FAT32')
            host(['e2fsck', '-fn', directory / 'ext2.img'], directory / 'host.log')
            host(['fsck.fat', '-n', directory / 'fat32.img'], directory / 'host.log')
            verify_template(template, arch)
            record.update(status='PASS', assertions=expected, full_byte_comparison=compared,
                          final_image_sha256={name: sha(directory / name) for name in FILES},
                          log_sha256={p.name: sha(p) for p in directory.glob('*.log')},
                          qemu_version=subprocess.check_output([qemu, '--version'], text=True).splitlines()[0])
            print(arch + ': original contest PASS (five assertions, three exact 96-byte files, sync and fsck)', flush=True)
        results['status'] = 'PASS'
    except BaseException as error:
        results.update(status='FAIL', error=str(error))
        raise
    finally:
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
