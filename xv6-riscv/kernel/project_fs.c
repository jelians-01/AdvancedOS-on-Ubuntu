#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "blkdev.h"
#include "ext2.h"
#include "fat32.h"

// Called once by the first process after the native VFS root is available.
// A missing or invalid data volume must not prevent native init from running.
void
project_mounts(void)
{
  uint64 bytes;
  printf("mount xv6 dev=1 /: OK\n");
  if(blk_size(2, &bytes) < 0)
    printf("mount ext2 dev=2 /mnt: SKIP (device unavailable)\n");
  else if(ext2_mount(2, "/mnt") < 0)
    printf("mount ext2 dev=2 /mnt: FAILED\n");
  else
    printf("mount ext2 dev=2 /mnt: OK\n");
  if(blk_size(3, &bytes) < 0)
    printf("mount fat32 dev=3 /fat: SKIP (device unavailable)\n");
  else if(fat32_mount(3, "/fat") < 0)
    printf("mount fat32 dev=3 /fat: FAILED\n");
  else
    printf("mount fat32 dev=3 /fat: OK\n");
}
