# ext2 Host Experiment

`ext2_reader.c` is a Linux host-side parser and constrained write experiment for the project's 1 KiB, 128-byte-inode ext2 image. It is a format-validation tool and reference implementation; it is not an xv6 kernel filesystem backend and does not use a VFS or block-device interface.

Build in WSL Ubuntu:

```sh
gcc -std=c11 -Wall -Wextra -Werror -O2 -o ext2_reader ext2_reader.c
```

Read-only commands:

```sh
./ext2_reader base.ext2 info
./ext2_reader base.ext2 ls /
./ext2_reader base.ext2 stat /hello.txt
./ext2_reader base.ext2 cat /multi.bin
./ext2_reader base.ext2 read /multi.bin 1020 10
```

The `put` and `truncate` commands are deliberately limited to root-level regular files in an unfeatured ext2 image with 1 KiB blocks and 128-byte inodes. Writes support at most 12 direct data blocks (12 KiB); indirect-block writes, directory growth, deletion, crash consistency, and general ext2 compatibility are not supported. Run them only on a disposable image copy, then check it with `e2fsck -fn`.

```sh
cp base.ext2 work.ext2
./ext2_reader work.ext2 put host-file /new-file
./ext2_reader work.ext2 truncate /new-file
e2fsck -fn work.ext2
```
