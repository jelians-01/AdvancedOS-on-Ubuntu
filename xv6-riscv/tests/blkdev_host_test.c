// Thin transport/lock substitute; tests the actual kernel/blkdev.c algorithm.
#include "blkdev_host.h"
#include "../kernel/blkdev.h"
#include "../kernel/virtio_disk.h"
#include <assert.h>
#include <stdio.h>
#include <sched.h>

static uchar disks[3][4096];
static int noflush, readonly, fail_sector = -1;
int virtio_disk_size(uint dev, uint64 *size)
{
  if(dev < 1 || dev > 3 || !size) return -1;
  *size = sizeof(disks[0]); return 0;
}
int virtio_disk_transfer(uint dev, uint64 sector, void *data, uint n, int write)
{
  if(dev < 1 || dev > 3 || sector > 8 || n > 4096-sector*512 ||
     sector == (uint64)fail_sector || (write && readonly)) return -1;
  if(write) memcpy(disks[dev-1]+sector*512, data, n);
  else memcpy(data, disks[dev-1]+sector*512, n);
  sched_yield(); // Encourage interleaving between partial-sector read and write.
  return 0;
}
int virtio_disk_flush(uint dev)
{ return dev >= 1 && dev <= 3 && !noflush ? 0 : -1; }

static void *writer(void *arg)
{
  uint off = (uintptr_t)arg;
  uchar bytes[4];
  for(uint i = 0; i < 10000; i++){
    memset(bytes, (i+off)&255, sizeof(bytes));
    assert(blk_write(2, off, bytes, sizeof(bytes)) == 0);
  }
  return 0;
}

int main(void)
{
  blk_init();
  memset(disks, 0x5a, sizeof(disks));
  pthread_t a, b;
  assert(pthread_create(&a, 0, writer, (void *)100) == 0);
  assert(pthread_create(&b, 0, writer, (void *)200) == 0);
  pthread_join(a, 0); pthread_join(b, 0);
  for(uint i = 0; i < 512; i++){
    uchar expect = i >= 100 && i < 104 ? (9999+100)&255 :
                   i >= 200 && i < 204 ? (9999+200)&255 : 0x5a;
    assert(disks[1][i] == expect);
  }
  puts("PASS: 20000 concurrent partial-sector writes and neighbor guards");
  uchar buf[1024]; memset(buf, 0xc3, sizeof(buf));
  readonly = 1;
  assert(blk_write(3, 0, buf, 512) == -1);
  assert(disks[2][0] == 0x5a);
  readonly = 0; noflush = 1;
  assert(blk_flush(2) == -1);
  noflush = 0;
  fail_sector = 1;
  assert(blk_write(3, 509, buf, 1024) == -1);
  assert(disks[2][508] == 0x5a && disks[2][509] == 0xc3 && disks[2][512] == 0x5a);
  fail_sector = -1;
  assert(blk_read(3, ~(uint64)0, buf, 1024) == -1);
  assert(blk_read(0, 0, buf, 0) == -1);
  assert(blk_read(2, 4096, 0, 0) == 0);
  assert(blk_read(2, 4097, 0, 0) == -1);
  puts("PASS: I/O errors, partial failure, unsupported flush, readonly and overflow");
}
