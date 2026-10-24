from pathlib import Path
import re
print('#include "kernel/syscall.h"')
for name in re.findall(r'entry\("([a-z_]+)"\)',Path('user/usys.pl').read_text()):
    symbol='sys_sbrk' if name=='sbrk' else name
    print(f'.globl {symbol}\n{symbol}:\n  addi.d $a7, $zero, SYS_{name}\n  syscall 0\n  jirl $zero, $ra, 0')
