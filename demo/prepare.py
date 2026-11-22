#!/usr/bin/env python3
"""Recreate contest templates from built final sources and original test inputs."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from run import BASE, FILES, qemu_command, qemu_path, require, sha


class NativeImage:
    """Read native xv6 files independently, without booting the image."""
    def __init__(self, path):
        self.data = path.read_bytes()
        sb = struct.unpack_from('<8I', self.data, 1024)
        require(sb[0] == 0x10203040 and len(self.data) == sb[1] * 1024, 'invalid native image')
        self.inodes, self.start = sb[3], sb[6]

    def block(self, number):
        require(0 < number < len(self.data) // 1024, 'invalid native block')
        return self.data[number * 1024:(number + 1) * 1024]

    def inode(self, number):
        require(0 < number < self.inodes, 'invalid native inode')
        fields = struct.unpack_from('<4HI13I', self.data,
                                    (self.start + number // 16) * 1024 + number % 16 * 64)
        kind, size, addresses = fields[0], fields[4], fields[5:]
        require(kind in (1, 2) and size <= 268 * 1024, 'invalid native regular/directory inode')
        content = bytearray()
        for logical in range((size + 1023) // 1024):
            physical = addresses[logical] if logical < 12 else struct.unpack_from(
                '<I', self.block(addresses[12]), (logical - 12) * 4)[0]
            content.extend(self.block(physical))
        return kind, bytes(content[:size])

    def read(self, path):
        current = 1
        for name in path.strip('/').split('/'):
            kind, content = self.inode(current)
            require(kind == 1 and len(content) % 16 == 0, 'invalid parent directory')
            matches = []
            for offset in range(0, len(content), 16):
                number, raw = struct.unpack_from('<H14s', content, offset)
                if number and raw.split(b'\0')[0] == name.encode():
                    matches.append(number)
            require(len(matches) == 1, 'missing or duplicate path: ' + path)
            current = matches[0]
        kind, content = self.inode(current)
        require(kind == 2, 'not a regular file: ' + path)
        return content


class Guest:
    def __init__(self, command, log):
        self.command, self.log_path = command, log

    def __enter__(self):
        self.log = self.log_path.open('xb')
        self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.pending = bytearray()
        return self

    def until(self, pattern=rb'(?:^|[\r\n])\$ ', timeout=90):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            require(b'panic:' not in self.pending, 'kernel panic; inspect log')
            match = re.search(pattern, self.pending)
            if match:
                text = bytes(self.pending[:match.end()]).decode(errors='replace').replace('\r', '')
                del self.pending[:match.end()]
                return text
            for key, _ in self.selector.select(0.2):
                chunk = os.read(key.fileobj.fileno(), 65536)
                require(chunk, 'QEMU exited before expected output')
                self.log.write(chunk)
                self.log.flush()
                self.pending.extend(chunk)
        raise TimeoutError('QEMU output timeout; inspect ' + str(self.log_path))

    def send(self, command, **kwargs):
        self.process.stdin.write(command.encode() + b'\n')
        self.process.stdin.flush()
        return self.until(**kwargs)

    def __exit__(self, *unused):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            tail, _ = self.process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            tail, _ = self.process.communicate(timeout=10)
        self.log.write(tail)
        self.log.close()
        self.selector.close()


def host(command, log, cwd=None):
    with log.open('ab') as stream:
        stream.write(('> ' + ' '.join(map(str, command)) + '\n').encode())
        subprocess.run(command, cwd=cwd, check=True, stdout=stream, stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=BASE.parent / 'xv6-riscv')
    parser.add_argument('--out', type=Path, default=BASE / 'templates', help='must not already exist')
    parser.add_argument('--arch', choices=('riscv', 'loongarch', 'all'), default='all')
    parser.add_argument('--qemu-riscv')
    parser.add_argument('--qemu-loongarch')
    options = parser.parse_args()
    source, out = options.source.resolve(), options.out.resolve()
    project = source.parent
    portable = project / 'tools/bin'
    os.environ['PATH'] = str(portable) + os.pathsep + os.environ.get('PATH', '')
    for tool in ('mke2fs', 'mkfs.fat', 'e2fsck', 'fsck.fat'):
        require(shutil.which(tool), 'missing host tool: ' + tool)
    require(not out.exists() and not any(c.isspace() or c == ',' for c in str(out)),
            'output must be a new path without spaces or commas')
    inputs = BASE / 'tests'
    require(len((inputs / 'hello.c').read_bytes()) == 96 and
            len((inputs / 'cmds.sh').read_bytes()) == 73, 'original fixture length changed')
    for i in (1, 2):
        require((inputs / f'test{i}.c').read_bytes() == (source / f'user/vfstest{i}.c').read_bytes(),
                'original contest program source differs from built project')
    arches = ('riscv', 'loongarch') if options.arch == 'all' else (options.arch,)
    out.mkdir(parents=True, exist_ok=False)
    for arch in arches:
        directory = out / arch
        directory.mkdir()
        artifacts = source if arch == 'riscv' else source / 'build/loongarch'
        kernel = artifacts / ('kernel/kernel' if arch == 'riscv' else 'kernel.elf')
        programs = sorted((artifacts / 'user').glob('_*'))
        require(kernel.is_file() and programs, 'build kernel and all user programs first')
        required = {'_init', '_sh', '_cat', '_mkdir', '_ln', '_rm', '_sync', '_testrun', '_vfstest1', '_vfstest2'}
        require(required.issubset({p.name for p in programs}), 'build is missing required user programs')
        machine = 243 if arch == 'riscv' else 258
        for path in (kernel, *programs):
            header = path.read_bytes()[:20]
            require(header[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', header, 18)[0] == machine,
                    'wrong ELF architecture: ' + str(path))
        qemu = qemu_path(arch, project, getattr(options, 'qemu_' + arch))
        shutil.copyfile(kernel, directory / 'kernel')
        log = directory / 'prepare.log'
        mkfs = source / 'mkfs/mkfs' if arch == 'riscv' else artifacts / 'mkfs'
        require(mkfs.is_file(), 'build native mkfs first')
        with tempfile.TemporaryDirectory(prefix='.seed-', dir=out) as temp:
            seed = Path(temp)
            for path in programs:
                shutil.copyfile(path, seed / path.name)
            for name in ('cmds.sh', 'hello.c'):
                shutil.copyfile(inputs / name, seed / name)
            host([str(mkfs), str(directory / 'fs.img'), *[p.name for p in programs], 'cmds.sh', 'hello.c'], log, seed)
        for name, size in (('ext2.img', 32 * 1024 * 1024), ('fat32.img', 64 * 1024 * 1024)):
            with (directory / name).open('xb') as stream:
                stream.truncate(size)
        host(['mke2fs', '-t', 'ext2', '-F', '-b', '1024', '-I', '128', '-O', 'none', directory / 'ext2.img'], log)
        host(['mkfs.fat', '-F', '32', '-s', '1', directory / 'fat32.img'], log)
        mappings = {'test1': 'vfstest1', 'test2': 'vfstest2', 'cat': 'cat', 'mkdir': 'mkdir', 'sh': 'sh'}
        with Guest(qemu_command(arch, directory, qemu, automated=True), directory / 'prepare-qemu.log') as guest:
            boot = guest.until()
            for line in ('mount xv6 dev=1 /: OK', 'mount ext2 dev=2 /mnt: OK', 'mount fat32 dev=3 /fat: OK'):
                require(line in boot.splitlines(), 'mount failed during template preparation')
            commands = ['mkdir /tests']
            commands += [f'ln /{root} /tests/{name}' for name, root in mappings.items()]
            commands += ['ln /hello.c /tests/hello.c', 'ln /cmds.sh /tests/cmds.sh',
                         'rm /hello.c /cmds.sh', '/testrun /sync']
            for command in commands:
                text = guest.send(command)
                require(not re.search(r'failed|panic:|exec .* fail|\bFAIL\b', text), 'guest setup failed: ' + command)
                if command == '/testrun /sync':
                    require('sync: OK' in text and 'TESTRUN /sync status=0' in text, 'setup sync failed')
        native = NativeImage(directory / 'fs.img')
        for name in ('cmds.sh', 'hello.c'):
            require(native.read('/tests/' + name) == (inputs / name).read_bytes(), 'prepared input mismatch')
        for name, root in mappings.items():
            require(native.read('/tests/' + name) == (artifacts / 'user' / ('_' + root)).read_bytes(),
                    'prepared user program mismatch: ' + name)
        host(['e2fsck', '-fn', directory / 'ext2.img'], log)
        host(['fsck.fat', '-n', directory / 'fat32.img'], log)
        manifest = {
            'arch': arch, 'source_commit': subprocess.check_output(
                ['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip(),
            'scope': 'Current kernel and all current compiled user programs; original contest inputs',
            'sha256': {name: sha(directory / name) for name in FILES},
            'input_sha256': {p.name: sha(p) for p in sorted(inputs.iterdir()) if p.is_file()},
            'user_binary_sha256': {p.name: sha(p) for p in programs},
            'kernel_source_sha256': {str(p.relative_to(source)): sha(p)
                                    for p in sorted((source / 'kernel').rglob('*'))
                                    if p.is_file() and p.suffix in ('.c', '.h', '.S', '.ld')},
            'qemu_version': subprocess.check_output([qemu, '--version'], text=True).splitlines()[0],
        }
        (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(arch + ' template prepared and checked', flush=True)


if __name__ == '__main__':
    main()
