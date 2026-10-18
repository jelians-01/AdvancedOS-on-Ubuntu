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

The `hal-platform` base provides C's D1 byte-range device API (`blk_read`, `blk_write`, `blk_size`, `blk_flush`), with device 2 designated for an ext2 data volume. This branch does not yet contain A's VFS interface; kernel integration remains pending that interface and its mount/operation contracts.
