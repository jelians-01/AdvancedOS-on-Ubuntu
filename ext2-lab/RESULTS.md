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

- On 2026-10-20, `make -j4 TOOLPREFIX=riscv64-unknown-elf- kernel/kernel fs.img` completed successfully in WSL Ubuntu with RISC-V GCC 10.2.0. This also builds the `ext2test` user program into the xv6 image.
- QEMU 8.2.2 successfully booted xv6 with a second virtio disk assigned to device 2. `ext2test` printed `ext2test: ALL TESTS PASSED`; it checked mounted-root enumeration, file attributes, the complete `hello.txt` contents, EOF behavior, and every byte of a 3073-byte `multi.bin` across four data blocks. `ls /mnt` listed both files and `cat /mnt/hello.txt` printed `hello from ext2`.
- The runtime test exposed and fixed two issues: the superblock buffer passed to virtio must be page-allocated (DMA cannot use that stack buffer), and directory record bounds must be measured from the start of the current block when the readdir cookie is nonzero.
- The disposable 32 MiB ext2 image with `hello.txt` and `multi.bin` is at `/home/ext2-test/ext2.img` inside Ubuntu 24.04. `e2fsck -fn` passed before boot. The image is a local test artifact and is not part of the repository.

To repeat the test in WSL Ubuntu 24.04, run from `xv6-riscv`:

```bash
make qemu TOOLPREFIX=riscv64-unknown-elf- \
  QEMU=qemu-system-riscv64 \
  DISK2=/home/ext2-test/ext2.img
```

At the xv6 shell, run `ext2test`, then independently check `ls /mnt` and `cat /mnt/hello.txt`. A passing test prints `ext2test: ALL TESTS PASSED`.
