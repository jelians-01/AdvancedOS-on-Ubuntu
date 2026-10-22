"""Build a fresh demo with disposable images; never accepts a real disk path."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--transport',choices=('modern','legacy'),default='modern')
parser.add_argument('--check',action='store_true',help='capture output, verify completion, then stop QEMU')
args = parser.parse_args()
source = Path(__file__).resolve().parent
out = source/'test-output'/('demo-'+time.strftime('%Y%m%d-%H%M%S')+'-'+str(time.time_ns()%1000000))
work = out/'work'
work.mkdir(parents=True)
for path in source.iterdir():
    if path.is_file() and (path.suffix in ('.c','.h','.S','.ld') or path.name=='Makefile'):
        shutil.copyfile(path,work/path.name)
cross = os.environ.get('LOONGARCH_CROSS','loongarch64-unknown-linux-gnu-')
qemu = os.environ.get('LOONGARCH_QEMU','qemu-system-loongarch64')
with (out/'build.log').open('wb') as log:
    subprocess.run(['make','user-disk','CROSS='+cross],cwd=work,stdout=log,
                   stderr=subprocess.STDOUT,check=True)
command = [qemu,'-machine','virt','-smp','1','-m','2G','-nographic','-nic','none',
           '-kernel','build/user-disk-probe.elf']
for dev in range(1,4):
    path = out/f'dev{dev}.img'
    seed = bytes((dev*31+i*7)%256 for i in range(256))
    path.write_bytes(seed*(8*1024*1024//256))
    command += ['-drive',f'file={path},if=none,format=raw,id=d{dev}'+(',readonly=on' if dev==3 else ''),
                '-device',f'virtio-blk-pci,drive=d{dev},addr=0x{dev+4:x},'+
                ('disable-legacy=on' if args.transport=='modern' else 'disable-modern=on')]
(out/'demo-manifest.json').write_text(json.dumps({
    'command':command,'disposable_images':True,
    'source_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in work.iterdir() if p.is_file()}},indent=2))
print('LoongArch PLV3 + scheduler + D1 PCI disk demo.',flush=True)
print('Output:',out,flush=True)
if args.check:
    try:
        result = subprocess.run(command,cwd=work,stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,timeout=4)
        raise RuntimeError('QEMU exited before demo completed: '+result.stdout.decode(errors='replace'))
    except subprocess.TimeoutExpired as event:
        output = event.stdout or b''
    (out/'qemu.log').write_bytes(output)
    if b'user: ALL PASS' not in output or b': FAIL' in output:
        raise RuntimeError('Demo failed; see qemu.log')
    print('DEMO CHECK PASS',flush=True)
else:
    print('After user: ALL PASS, press Ctrl+A then X to exit QEMU.',flush=True)
    subprocess.run(command,cwd=work,check=True)
