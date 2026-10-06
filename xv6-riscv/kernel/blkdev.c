#ifdef BLKDEV_HOST_TEST
#include "../tests/blkdev_host.h"
#else
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#endif
#include "virtio_disk.h"
#include "blkdev.h"

// Covers the entire read-modify-write, including both transport requests.
static struct {
  struct sleeplock lock;
  uchar sector[VDISK_SECTOR_SIZE];
} ranges[VDISK_COUNT];

void
blk_init(void)
{
  for(int i = 0; i < VDISK_COUNT; i++)
    initsleeplock(&ranges[i].lock, "blk_range");
}

int
blk_size(uint dev, uint64 *out_bytes)
{
  return out_bytes ? virtio_disk_size(dev, out_bytes) : -1;
}

static int
range_io(uint dev, uint64 off, void *data, uint n, int write)
{
  uint64 size;
  if(dev == 0 || dev > VDISK_COUNT || blk_size(dev, &size) < 0 ||
     off > size || (uint64)n > size - off)
    return -1; // Subtraction avoids overflowing off+n.
  if(n == 0)
    return 0;
  if(data == 0 || (write && dev == ROOTDEV))
    return -1; // Never bypass the mounted native root's cache/log.
  acquiresleep(&ranges[dev-1].lock);
  uchar *p = data;
  uchar *bounce = ranges[dev-1].sector;
  int result = 0;
  while(n){
    uint skip = off % VDISK_SECTOR_SIZE;
    uint take = VDISK_SECTOR_SIZE - skip;
    if(take > n)
      take = n;
    uint64 sector = off / VDISK_SECTOR_SIZE;
    if(skip == 0 && take == VDISK_SECTOR_SIZE){
      if(virtio_disk_transfer(dev, sector, p, take, write) < 0){
        result = -1;
        break;
      }
    } else {
      if(virtio_disk_transfer(dev, sector, bounce, VDISK_SECTOR_SIZE, 0) < 0){
        result = -1;
        break;
      }
      if(write){
        memmove(bounce + skip, p, take);
        if(virtio_disk_transfer(dev, sector, bounce, VDISK_SECTOR_SIZE, 1) < 0){
          result = -1;
          break;
        }
      } else {
        memmove(p, bounce + skip, take);
      }
    }
    p += take;
    off += take;
    n -= take;
  }
  releasesleep(&ranges[dev-1].lock);
  return result;
}

int
blk_read(uint dev, uint64 off, void *dst, uint n)
{
  return range_io(dev, off, dst, n, 0);
}

int
blk_write(uint dev, uint64 off, const void *src, uint n)
{
  return range_io(dev, off, (void *)src, n, 1);
}

int
blk_flush(uint dev)
{
  uint64 size;
  if(blk_size(dev, &size) < 0)
    return -1;
  acquiresleep(&ranges[dev-1].lock);
  int result = virtio_disk_flush(dev);
  releasesleep(&ranges[dev-1].lock);
  return result;
}
