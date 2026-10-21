"""Build and test on disposable copies. Run from Ubuntu: python3 tests/run_c_tests.py.
QEMU override: XV6_QEMU=/path/to/qemu-system-riscv64.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import selectors
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--quick', action='store_true', help='quick original usertests instead of full suite')
parser.add_argument('--persistence-only', type=Path, help='read previously written d1 volumes without modifying them')
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]
out = source / 'test-output' / time.strftime('run-%Y%m%d-%H%M%S')
out.mkdir(parents=True)
qemu = os.environ.get('XV6_QEMU', 'qemu-system-riscv64')
env = dict(os.environ)
env['PATH'] = str(Path(qemu).parent) + ':' + env.get('PATH', '')
manifest = {'qemu': subprocess.check_output([qemu, '--version'], text=True).splitlines()[0],
            'compiler': subprocess.check_output(['riscv64-linux-gnu-gcc', '--version'], text=True).splitlines()[0],
            'cpu_count': 3, 'machine': 'virt', 'cache': 'writeback', 'results': [],
            'source_sha256': {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                for folder in ('kernel', 'user', 'tests') for p in (source/folder).rglob('*')
                if p.is_file() and p.suffix in ('.c','.h','.S','.py')},
            'makefile_sha256': hashlib.sha256((source/'Makefile').read_bytes()).hexdigest()}

def copy_build(name, selftest=False, readonly=False, persistence=False):
    work = out / name
    if work.exists():
        raise RuntimeError(f'{work} already exists; use a new output directory to preserve evidence')
    # Copy bytes only: WSL users may not set Windows-owned file timestamps.
    for base, dirs, files in os.walk(source):
        dirs[:] = [d for d in dirs if d not in ('.git', 'test-output')]
        for filename in files:
            if filename.startswith('_') or filename in ('kernel', 'usys.S') or Path(filename).suffix in ('.o', '.d', '.asm', '.sym', '.img'):
                continue
            old = Path(base) / filename
            new = work / old.relative_to(source)
            new.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(old, new)
    cmd = ['make', '-j4', 'kernel/kernel', 'fs.img']
    if selftest:
        cmd.append('BLKDEV_SELFTEST=1')
    if readonly:
        cmd.append('EXTRA_CFLAGS=-DBLKDEV_READONLY_TEST')
    if persistence:
        cmd.append('EXTRA_CFLAGS=-DBLKDEV_PERSISTENCE_TEST')
    with (out / (name+'-build.log')).open('wb') as log:
        subprocess.run(cmd, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    for dev in (2, 3):
        block = bytes((i*17+dev) % 256 for i in range(256))
        image = block * (4*1024*1024//256)
        (work / f'dev{dev}.img').write_bytes(image)
        manifest.setdefault('initial_sha256', {})[f'dev{dev}'] = hashlib.sha256(image).hexdigest()
    return work

def run(work, name, commands=(), readonly=False, selftest=False, expected_disks=3, data_from=None):
    command = [qemu, '-machine', 'virt', '-bios', 'none', '-kernel', 'kernel/kernel', '-m', '128M',
               '-smp', '3', '-nographic', '-global', 'virtio-mmio.force-legacy=false']
    for slot in range(expected_disks):
        image = 'fs.img' if slot == 0 else f'dev{slot+1}.img'
        if slot and data_from:
            image = str(data_from / image)
        drive = f'file={image},if=none,format=raw,id=d{slot},cache=writeback'
        if readonly and slot == 2:
            drive += ',readonly=on'
        command += ['-drive', drive, '-device', f'virtio-blk-device,drive=d{slot},bus=virtio-mmio-bus.{slot}']
    p = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    poller = selectors.DefaultSelector()
    poller.register(p.stdout, selectors.EVENT_READ)
    transcript = bytearray()
    live_log = (out / (name+'.log')).open('wb')
    cursor = 0
    def until(token, timeout):
        nonlocal cursor
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if b'panic:' in transcript[cursor:]:
                raise RuntimeError('kernel panic; see QEMU log')
            position = transcript.find(token, cursor)
            if position >= 0:
                part = bytes(transcript[cursor:position+len(token)])
                cursor = position+len(token)
                return part
            for key, _ in poller.select(.2):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    raise RuntimeError('QEMU ended before expected prompt')
                transcript.extend(chunk)
                live_log.write(chunk)
                live_log.flush()
        raise TimeoutError(token)
    try:
        boot = until(b'$ ', 45)
        if selftest and b'D1 ALL TESTS PASSED' not in boot:
            raise RuntimeError('D1 selftest did not pass')
        for cmd in commands:
            p.stdin.write(cmd.encode()+b'\n'); p.stdin.flush()
            result = until(b'$ ', 1800 if cmd.startswith('usertests') else 30)
            if cmd.startswith('usertests') and b'ALL TESTS PASSED' not in result:
                raise RuntimeError(f'{cmd} failed')
            if cmd == 'cat ccheck' and b'C_READY\n' not in result.replace(b'\r', b''):
                raise RuntimeError('file content mismatch')
            print(f'{name}: {cmd}: PASS', flush=True)
        manifest['results'].append({'case': name, 'status': 'PASS', 'command': command, 'guest_commands': list(commands)})
        print(f'{name}: PASS', flush=True)
    except Exception as error:
        manifest['results'].append({'case': name, 'status': 'FAIL', 'error': str(error)})
        raise
    finally:
        if p.poll() is None:
            try:
                p.stdin.write(b'\x01x'); p.stdin.flush()
            except BrokenPipeError:
                pass
        try:
            tail, _ = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            tail, _ = p.communicate(timeout=10)
        transcript.extend(tail)
        live_log.write(tail)
        live_log.close()
        (out / (name+'.log')).write_bytes(transcript)
        (out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2))

if args.persistence_only:
    for dev in (2, 3):
        if not (args.persistence_only / f'dev{dev}.img').is_file():
            parser.error('persistence directory must contain existing dev2.img and dev3.img')
    work = copy_build('persistence', selftest=True, persistence=True)
    run(work, 'guest-reboot-readback', selftest=True, data_from=args.persistence_only.resolve())
    raise SystemExit(0)

work = copy_build('d1', selftest=True)
run(work, 'three-device-d1', selftest=True)
payload = bytes((i*7+93) % 256 for i in range(1031))
for dev in (2, 3):
    data = (work / f'dev{dev}.img').read_bytes()
    assert data[509:1540] == payload, f'dev{dev}: persisted content mismatch'
manifest['results'].append({'case': 'host image bytes after flush', 'status': 'PASS'})
normal = copy_build('normal')
# Reuse the written data volumes without rebuilding them; normal kernel has no selftest writes.
for dev in (2, 3):
    shutil.copyfile(work / f'dev{dev}.img', normal / f'dev{dev}.img')
run(normal, 'same-images-reboot', ['echo C_READY > ccheck', 'cat ccheck', 'rm ccheck'])
for dev in (2, 3):
    assert (normal / f'dev{dev}.img').read_bytes() == (work / f'dev{dev}.img').read_bytes()
persist = copy_build('persistence', selftest=True, persistence=True)
run(persist, 'guest-reboot-readback', selftest=True, data_from=work)
run(normal, 'native-regression', ['usertests -q' if args.quick else 'usertests'])
run(normal, 'single-root-compatibility', ['usertests copyin'], expected_disks=1)
ro = copy_build('readonly', selftest=True, readonly=True)
run(ro, 'read-only-device', selftest=True, readonly=True)
(out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
# Exercise the actual D1 implementation through the host transport adapter.
binary = out/'blkdev_host_test'
with (out/'host-sanitizer.log').open('wb') as log:
    subprocess.run(['gcc','-DBLKDEV_HOST_TEST','-g','-O1','-pthread',
                    '-fsanitize=address,undefined','kernel/blkdev.c',
                    'tests/blkdev_host_test.c','-o',str(binary)],
                   cwd=source,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    subprocess.run([str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
manifest['results'].append({'case':'host concurrency and ASan/UBSan','status':'PASS'})
(out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2))
print('D1 AND NATIVE TEST MATRIX PASSED', flush=True)
