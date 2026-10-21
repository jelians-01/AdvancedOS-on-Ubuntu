"""Destructive checks use only newly created throwaway images in test-output.
No caller-provided disks are accepted. Called by run_probes.py.
"""
import hashlib
from pathlib import Path

SIZE = 8*1024*1024
MARKER = b'LOONGARCH-D1-PERSIST\0'

def run_disk_matrix(work, out, run_guest, results):
    def new_images(name, count=3):
        folder = out/('images-'+name)
        folder.mkdir()
        paths, original = [], []
        for dev in range(1,count+1):
            seed = bytes((dev*31+i*7)%256 for i in range(256))
            content = seed*(SIZE//256)
            path = folder/f'dev{dev}.img'
            path.write_bytes(content)
            paths.append(path)
            original.append(content)
        return paths, original

    def devices(paths,mode,debug=None):
        args = []
        for index,path in enumerate(paths):
            dev = index+1
            image = f'blkdebug:{debug}:{path}' if debug and dev==2 else str(path)
            args += ['-drive',f'file={image},if=none,format=raw,id=d{dev}'+
                     (',readonly=on' if dev==3 else '')]
            modern = mode=='modern' or (mode=='mixed' and dev!=2)
            args += ['-device',f'virtio-blk-pci,drive=d{dev},addr=0x{dev+4:x},'+
                     ('disable-legacy=on' if modern else 'disable-modern=on')]
        return args

    def verify(name,paths,before,expected):
        entries = []
        for dev,(path,initial,want) in enumerate(zip(paths,before,expected),1):
            actual = path.read_bytes()
            if actual != want:
                mismatch = next((i for i,(a,b) in enumerate(zip(actual,want)) if a!=b),-1)
                raise RuntimeError(f'{name}: dev{dev} host bytes differ at {mismatch}')
            entries.append({'device':dev,'before_sha256':hashlib.sha256(initial).hexdigest(),
                            'after_sha256':hashlib.sha256(actual).hexdigest(),
                            'expected_sha256':hashlib.sha256(want).hexdigest()})
        results.append({'case':name,'status':'PASS','images':entries})
        print(name+': PASS',flush=True)

    run_guest('disk-no-device','disk-probe.elf',['disk: no device handled PASS'],[])
    paths,before = new_images('single',1)
    run_guest('disk-single','disk-single.elf',['disk: single-root compatibility PASS'],devices(paths,'modern'))
    verify('disk-single-host',paths,before,before)
    expected_tokens = ['disk: three devices capacity and initial bytes PASS',
                       'disk: D1 unaligned RMW range RO root protection PASS',
                       'disk: queue reuse and flush PASS','disk: ALL PASS']
    for mode in ('modern','legacy','mixed'):
        paths,before = new_images(mode)
        args = devices(paths,mode)
        run_guest('disk-'+mode,'disk-probe.elf',expected_tokens,args)
        expected = list(before)
        writable = bytearray(before[1])
        writable[777:777+1031] = bytes((i*13+7)%256 for i in range(1031))
        writable[4096:4096+len(MARKER)] = MARKER
        expected[1] = bytes(writable)
        # Check exact host bytes before reboot as well, including adjacent bytes.
        for path,want in zip(paths,expected):
            if path.read_bytes() != want:
                raise RuntimeError(mode+': write/flush host bytes failed before reboot')
        run_guest('disk-'+mode+'-reboot','disk-reboot.elf',
                  ['disk: same-image reboot persistence PASS','disk: ALL PASS'],args)
        verify('disk-'+mode+'-host',paths,before,expected)

    paths,before = new_images('io-error')
    config = out/'blkdebug-error.conf'
    config.write_text('[inject-error]\nevent = "read_aio"\nerrno = "5"\n'
                      'sector = "14"\nonce = "on"\nimmediately = "on"\n')
    run_guest('disk-io-error','disk-error.elf',
              ['disk: device I/O error propagation and retry PASS','disk: ALL PASS'],
              devices(paths,'modern',config))
    verify('disk-io-error-host',paths,before,before)

    for mode in ('modern','legacy'):
        paths,before = new_images('user-'+mode)
        run_guest('user-disk-'+mode,'user-disk-probe.elf',[
            'user: PLV3 syscalls cross-page copies and register stacks PASS',
            'user: two address spaces and timer preemption PASS',
            'user: kernel window privileged page RO NX guard isolation PASS',
            'user: PLV3 syscall D1 PCI disk roundtrip PASS',
            'user: all process memory reclaimed PASS','user: ALL PASS'],devices(paths,mode))
        expected = list(before)
        writable = bytearray(before[1])
        writable[8192:8704] = bytes([0x60])*512
        writable[8704:9216] = bytes([0x61])*512
        expected[1] = bytes(writable)
        verify('user-disk-'+mode+'-host',paths,before,expected)
