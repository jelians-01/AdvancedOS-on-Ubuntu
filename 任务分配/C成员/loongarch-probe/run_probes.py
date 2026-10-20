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
    if p.is_file() and (p.suffix in ('.c', '.S', '.ld') or p.name == 'Makefile'):
        shutil.copyfile(p, work/p.name)
cross = os.environ.get('LOONGARCH_CROSS', 'loongarch64-unknown-linux-gnu-')
qemu = os.environ.get('LOONGARCH_QEMU', 'qemu-system-loongarch64')
manifest = {'source_sha256': {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in work.iterdir()}, 'results': []}
manifest['compiler'] = subprocess.check_output([cross+'gcc','--version'],text=True).splitlines()[0]
manifest['qemu'] = subprocess.check_output([qemu,'--version'],text=True).splitlines()[0]
cases = [
    ('all','loongarch-probe.elf',['loongarch probe: hello']),
    ('exception','exception-probe.elf',['exception probe: before syscall','E']),
    ('timer','timer-probe.elf',['T']),
    ('return','return-probe.elf',['trap return: before syscall','R','trap return: after ertn']),
    ('context','context-probe.elf',['context: main','context: worker','context: main resumed',
                                  'context: worker resumed','context: complete'])]
try:
    for target, elf, expected in cases:
        command = [qemu,'-machine','virt','-m','2G','-nographic','-kernel','build/'+elf]
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
