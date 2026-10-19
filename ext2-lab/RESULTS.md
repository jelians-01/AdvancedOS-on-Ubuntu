# ext2 Host Experiment Results

Date: 2026-10-17

## Verified

- `ext2_reader.c` compiles in WSL Ubuntu with `gcc -std=c11 -Wall -Wextra -Werror -O2`.
- Read-only inspection matches the reference `dumpe2fs`/`debugfs` values for the test image, and the parser has readback coverage for direct and indirect file blocks.
- On disposable copies of the 32 MiB, 1 KiB-block, 128-byte-inode image: created a 3073-byte file spanning four data blocks and verified byte-for-byte readback; overwrote it with 16 bytes and verified readback; truncated it to zero; created an empty file.
- `e2fsck -fn` passed after each of those four write scenarios.
- The original `base.ext2` SHA-256 remained `5389c626f9f1355b8e6ce47fd83b434293caa735d43e612dd44834d3c9fda8dc`.

## Scope

These results validate host-side ext2 image parsing and a constrained write experiment only. The tool is not linked into xv6. Its write path supports only root-level regular files, 1 KiB blocks, 128-byte inodes, no ext2 feature flags, and up to 12 direct data blocks. It does not provide transactions or crash recovery. Always test writes on an image copy.

The `hal-platform` base provides C's D1 byte-range device API (`blk_read`, `blk_write`, `blk_size`, `blk_flush`), with device 2 designated for an ext2 data volume. A's VFS interface is now merged, and `xv6-riscv/kernel/ext2.c` provides a read-only backend with mount, inode, directory lookup, directory enumeration, and offset reads. Write, truncate, unlink, and allocation operations remain intentionally unsupported in this milestone.

## Kernel build and runtime status

- On 2026-10-19, `make -j4 TOOLPREFIX=riscv64-unknown-elf- kernel/kernel fs.img` completed successfully in WSL Ubuntu with RISC-V GCC 10.2.0. This also built the `ext2test` user program into the xv6 image.
- QEMU available in that WSL distribution is 6.2.0; the project requires 7.2 or newer. Manual boot attempts with QEMU 6.2 produced no xv6 serial output, with and without a second data disk. Runtime validation of the kernel backend is therefore pending and is not claimed as passed.
- A disposable 32 MiB ext2 image with `hello.txt` and a 3073-byte `multi.bin` was created at `/tmp/ext2-kernel-check-20261019/ext2.img`. The host parser read its root directory and data crossing a block boundary; `e2fsck -fn` passed. Its copy at `/home/chenliu/AdvancedOS/ext2-vfs-build/ext2-dev2-check.img` is for local QEMU testing and is not part of the repository.

After installing QEMU 7.2 or newer in WSL, run from `xv6-riscv`:

```bash
make qemu TOOLPREFIX=riscv64-unknown-elf- \
  QEMU=qemu-system-riscv64 \
  DISK2=/home/chenliu/AdvancedOS/ext2-vfs-build/ext2-dev2-check.img
```

At the xv6 shell, run `ext2test`, then independently check `ls /mnt` and `cat /mnt/hello.txt`. A passing test prints `ext2test: ALL TESTS PASSED`. The earlier disposable `ext2-dev2.img` contains only `hello.txt`, so it does not satisfy `ext2test`'s directory and 3073-byte multi-block file checks.
