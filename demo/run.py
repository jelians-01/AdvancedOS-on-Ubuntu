#!/usr/bin/env python3
"""Run xv6 contest tests on a writable copy; templates are never booted directly."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

BASE = Path(__file__).resolve().parent
FILES = ('kernel', 'fs.img', 'ext2.img', 'fat32.img')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def qemu_path(arch, project, override=None):
    portable = project / 'tools/bin/qemu-system-loongarch64'
    value = override or ('qemu-system-riscv64' if arch == 'riscv' else str(portable))
    require(shutil.which(value) is not None, 'QEMU unavailable: ' + value)
    return value


def qemu_command(arch, directory, qemu, automated=False):
    require(not any(c in str(directory) for c in ',\n\r'), 'unsupported QEMU image path')
    args = [qemu, '-machine', 'virt', '-kernel', str(directory / 'kernel')]
    if arch == 'riscv':
        args += ['-bios', 'none', '-m', '128M', '-smp', '3',
                 '-global', 'virtio-mmio.force-legacy=false']
    else:
        args += ['-m', '1G', '-smp', '1', '-nic', 'none']
    args += (['-display', 'none', '-serial', 'stdio', '-monitor', 'none']
             if automated else ['-nographic'])
    for slot, name in enumerate(FILES[1:]):
        args += ['-drive', f'file={directory / name},if=none,format=raw,id=d{slot},cache=writeback']
        device = (f'virtio-blk-device,drive=d{slot},bus=virtio-mmio-bus.{slot}'
                  if arch == 'riscv' else
                  f'virtio-blk-pci,drive=d{slot},disable-legacy=on,addr={slot + 1}')
        args += ['-device', device]
    return args


def verify_template(template, arch):
    data = json.loads((template / 'manifest.json').read_text())
    require(data['arch'] == arch and set(data['sha256']) == set(FILES), 'invalid template manifest')
    for name, expected in data['sha256'].items():
        require(not (template / name).is_symlink() and sha(template / name) == expected,
                'template checksum mismatch: ' + name)
    return data


def new_session(template, arch, sessions):
    data = verify_template(template, arch)
    sessions.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=arch + '-', dir=sessions))
    for name in FILES:
        shutil.copyfile(template / name, directory / name)
        require(sha(directory / name) == data['sha256'][name], 'session copy mismatch')
    (directory / 'session.json').write_text(json.dumps(data, indent=2) + '\n')
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('riscv', 'loongarch'), required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--new', action='store_true', help='create fresh disks for this demonstration')
    mode.add_argument('--resume', action='store_true', help='reuse this architecture\'s last session')
    parser.add_argument('--qemu')
    args = parser.parse_args()
    project = BASE.parent
    qemu = qemu_path(args.arch, project, args.qemu)
    sessions = project / '.run'
    latest = sessions / ('latest-' + args.arch + '.json')
    if args.new:
        directory = new_session(BASE / 'templates' / args.arch, args.arch, sessions)
        latest.write_text(json.dumps({'directory': directory.name}) + '\n')
    else:
        require(latest.is_file(), 'No previous session. Start with --new.')
        name = json.loads(latest.read_text())['directory']
        require(Path(name).name == name and name.startswith(args.arch + '-'), 'invalid session name')
        directory = (sessions / name).resolve()
        require(directory.parent == sessions.resolve() and not (sessions / name).is_symlink(),
                'session escaped .run directory')
        data = json.loads((directory / 'session.json').read_text())
        require(data['arch'] == args.arch and sha(directory / 'kernel') == data['sha256']['kernel'],
                'session kernel mismatch')
        require(all((directory / name).is_file() and not (directory / name).is_symlink()
                    for name in FILES), 'session files missing or linked')
    command = qemu_command(args.arch, directory, qemu)
    (directory / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    print('Session:', directory, flush=True)
    print('xv6 commands: cd /tests  then  cat cmds.sh | sh', flush=True)
    print('Expect four test_copy_file successes and one test_cross_fs_copy success.', flush=True)
    print('mkdir /mnt already exists because the kernel mounts it automatically.', flush=True)
    print('Run /sync before exit; exit QEMU with Ctrl+A, then X.', flush=True)
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
