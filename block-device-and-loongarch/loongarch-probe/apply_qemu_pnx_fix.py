"""Backport QEMU's db2325f PNX recursion fix to a SEPARATE 7.2 source copy.

Usage: python3 apply_qemu_pnx_fix.py /path/to/qemu-7.2-source-copy
This does not rebuild/install QEMU. Original source/install should be retained.
Upstream: https://github.com/qemu/qemu/commit/db2325f79481fab87211e5a287580d753f582cb8
Upstream author: rail5 (Andrew S. Rightenburg).
"""
from pathlib import Path
import difflib
import hashlib
import json
import sys

source = Path(sys.argv[1]).resolve()
path = source/'target/loongarch/cpu.c'
before = path.read_bytes()
text = before.decode()
old_fetch = '    case EXCCODE_PIF:\n    case EXCCODE_ADEF:'
new_fetch = '    case EXCCODE_PIF:\n    case EXCCODE_PNX:\n    case EXCCODE_ADEF:'
old_data = '    case EXCCODE_PNR:\n    case EXCCODE_PNX:\n    case EXCCODE_PPI:'
new_data = '    case EXCCODE_PNR:\n    case EXCCODE_PPI:'
if text.count(old_fetch) != 1 or text.count(old_data) != 1:
    raise SystemExit('Refusing unknown or already patched source; use a fresh QEMU 7.2 copy')
text = text.replace(old_fetch,new_fetch).replace(old_data,new_data)
patch = ''.join(difflib.unified_diff(before.decode().splitlines(True),text.splitlines(True),
                                    'a/target/loongarch/cpu.c','b/target/loongarch/cpu.c'))
record = {
    'upstream':'db2325f79481fab87211e5a287580d753f582cb8',
    'file':'target/loongarch/cpu.c',
    'before_sha256':hashlib.sha256(before).hexdigest(),
    'after_sha256':hashlib.sha256(text.encode()).hexdigest(),
    'change':'Move PNX into instruction-fetch faults; skip recursive BADI fetch',
}
(source/'pnx-backport.patch').write_text(patch)
(source/'pnx-backport.json').write_text(json.dumps(record,indent=2)+'\n')
path.write_text(text)
print(json.dumps(record,indent=2))
