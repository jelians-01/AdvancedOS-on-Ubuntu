"""Run all existing LoongArch probes on fresh build copies, using stdlib only.
LOONGARCH_CROSS: tool prefix (default loongarch64-unknown-linux-gnu-).
LOONGARCH_QEMU: qemu-system-loongarch64 path. Run in Ubuntu/WSL.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import time

source = Path(__file__).resolve().parent
out = source/'test-output'/time.strftime('run-%Y%m%d-%H%M%S')
work = out/'work'
work.mkdir(parents=True)
for p in source.iterdir():
    if p.is_file() and (p.suffix in ('.c', '.h', '.S', '.ld') or p.name == 'Makefile'):
        shutil.copyfile(p, work/p.name)
cross = os.environ.get('LOONGARCH_CROSS', 'loongarch64-unknown-linux-gnu-')
qemu = os.environ.get('LOONGARCH_QEMU', 'qemu-system-loongarch64')
manifest = {'source_sha256': {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in work.iterdir()}, 'results': []}
manifest['compiler'] = subprocess.check_output([cross+'gcc','--version'],text=True).splitlines()[0]
manifest['host_compiler'] = subprocess.check_output(
    [os.environ.get('HOST_CC','cc'),'--version'],text=True).splitlines()[0]
manifest['qemu'] = subprocess.check_output([qemu,'--version'],text=True).splitlines()[0]
qemu_executable = Path(shutil.which(qemu) or qemu).resolve()
manifest['qemu_sha256'] = hashlib.sha256(qemu_executable.read_bytes()).hexdigest()
manifest['runner_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
manifest['support_sha256'] = {'apply_qemu_pnx_fix.py': hashlib.sha256(
    (source/'apply_qemu_pnx_fix.py').read_bytes()).hexdigest()}
cases = [
    ('all','loongarch-probe.elf',['loongarch probe: hello']),
    ('exception','exception-probe.elf',['exception probe: before syscall','E']),
    ('timer','timer-probe.elf',['T']),
    ('return','return-probe.elf',['trap return: before syscall','R','trap return: after ertn']),
    ('context','context-probe.elf',['context: main','context: worker','context: main resumed',
                                  'context: worker resumed','context: complete']),
    ('memory','memory-probe.elf',[
        'memory: initialization bounds PASS',
        'memory: alignment uniqueness zero-fill PASS',
        'memory: exhaustion PASS',
        'memory: page isolation PASS',
        'memory: invalid free and reinit rejection PASS',
        'memory: double free rejection and reuse clearing PASS',
        'memory: eight full allocation cycles PASS',
        'memory: ALL PASS']),
    ('vm','vm-probe.elf',[
        'vm: paging enabled DMW kernel PASS',
        'vm: software walk refill even-odd alias PASS',
        'vm: read-only store fault PME PASS',
        'vm: permission update and TLB invalidation PASS',
        'vm: no-read load fault PNR PASS',
        'vm: invalid page load fault PIL PASS',
        'vm: mapped execution and no-execute fault PNX PASS',
        'vm: unmapped refill miss recovery PASS',
        'vm: remap to different physical page PASS',
        'vm: table and data page reclamation PASS',
        'vm: ALL PASS'])]
try:
    for host_case in ('memory','vm'):
        host_command = ['make','host-'+host_case]
        with (out/(host_case+'-host.log')).open('wb') as log:
            subprocess.run(host_command,cwd=work,stdout=log,
                           stderr=subprocess.STDOUT,check=True)
        manifest['results'].append({'case':host_case+'-host','status':'PASS',
                                    'command':host_command})
        print(host_case+'-host (ASan/UBSan): PASS',flush=True)
    for target, elf, expected in cases:
        command = [qemu,'-machine','virt','-smp','1','-m','2G','-nographic',
                   '-nic','none','-kernel','build/'+elf]
        if target == 'vm' and os.environ.get('LOONGARCH_QEMU_TRACE') == '1':
            command += ['-d','int','-D',str(out/'vm-exceptions.log')]
        with (out/(target+'-build.log')).open('wb') as log:
            subprocess.run(['make',target,'CROSS='+cross],cwd=work,
                           stdout=log,stderr=subprocess.STDOUT,check=True)
        try:
            completed = subprocess.run(command,cwd=work,stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT,timeout=4)
            output = completed.stdout
            raise RuntimeError(f'{target}: unexpectedly exited with {completed.returncode}: {output!r}')
        except subprocess.TimeoutExpired as event:
            output = event.stdout or b''
        (out/(target+'-qemu.log')).write_bytes(output)
        if b'memory: FAIL' in output or b'vm: FAIL' in output:
            raise RuntimeError(f'{target}: explicit failure; see log')
        cursor = 0
        for token in expected:
            found = output.find(token.encode(),cursor)
            if found < 0:
                raise RuntimeError(f'{target}: missing {token!r}; see log')
            cursor = found+len(token)
        manifest['results'].append({'case':target,'status':'PASS','command':command})
        print(target+': PASS',flush=True)
except Exception as error:
    manifest['results'].append({'status':'FAIL','error':str(error)})
    raise
finally:
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
print('ALL LOONGARCH PROBES PASSED',out)
