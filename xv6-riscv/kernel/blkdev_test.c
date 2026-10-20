// Only linked with BLKDEV_SELFTEST=1. Never use production data volumes.
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "blkdev.h"

static uchar before[2048], after[2048], payload[1031];

static void
check(int ok, char *label)
{
  if(!ok){
    printf("D1 FAIL: %s\n", label);
    panic("D1 selftest failed");
  }
  printf("D1 PASS: %s\n", label);
}

void
blkdev_selftest(void)
{
  uint64 root, size2, size3;
  check(blk_size(1, &root) == 0 && root > 0 &&
        blk_size(2, &size2) == 0 && blk_size(3, &size3) == 0 &&
        size2 == 4*1024*1024 && size3 == size2, "capacity and three devices");
#ifdef BLKDEV_PERSISTENCE_TEST
  for(uint i = 0; i < sizeof(payload); i++)
    payload[i] = (uchar)(i*7+93);
  for(uint dev = 2; dev <= 3; dev++)
    check(blk_read(dev, 509, after, sizeof(payload)) == 0 &&
          memcmp(payload, after, sizeof(payload)) == 0, "guest read after flush and reboot");
  check(blk_read(2, size2-7, after, 7) == 0 && memcmp(payload, after, 7) == 0,
        "capacity-end bytes persist");
  printf("D1 ALL TESTS PASSED\n");
  return;
#endif
  for(uint dev = 2; dev <= 3; dev++){
    check(blk_read(dev, 0, after, 1027) == 0, "device marker read");
    for(uint i = 0; i < 1027; i++)
      if(after[i] != (uchar)(i*17+dev))
        check(0, "same offset isolation");
  }
  check(1, "same offset isolation");
  uchar stackbuf[1031];
  check(blk_read(2, 0, stackbuf, sizeof(stackbuf)) == 0,
        "kernel stack buffer aligned-sector read");
  for(uint i = 0; i < sizeof(stackbuf); i++)
    if(stackbuf[i] != (uchar)(i*17+2))
      check(0, "kernel stack buffer bytes");
  check(1, "kernel stack buffer bytes");
  check(blk_write(2, 2048, stackbuf, sizeof(stackbuf)) == 0 &&
        blk_read(2, 2048, after, sizeof(stackbuf)) == 0 &&
        memcmp(stackbuf, after, sizeof(stackbuf)) == 0,
        "kernel stack buffer aligned-sector write");
  check(blk_read(2, 511, after, 1027) == 0, "unaligned cross-sector read");
  for(uint i = 0; i < 1027; i++)
    if(after[i] != (uchar)((i+511)*17+2))
      check(0, "cross-sector bytes");
  check(1, "cross-sector bytes");
  check(blk_size(0, &root) == -1 && blk_size(4, &root) == -1 &&
        blk_size(1, 0) == -1 && blk_read(4, 0, after, 0) == -1,
        "invalid device and capacity pointer");
  check(blk_read(2, size2, 0, 0) == 0 && blk_write(2, size2, 0, 0) == 0 &&
        blk_read(2, size2+1, 0, 0) == -1 &&
        blk_read(2, ~(uint64)0, after, 2) == -1 &&
        blk_read(2, size2-1, after, 2) == -1 &&
        blk_read(2, 0, 0, 1) == -1, "zero length bounds and overflow");
  check(blk_write(1, 0, payload, 1) == -1, "root raw write rejected");
  check(blk_read(2, 0, before, sizeof(before)) == 0, "save sector guards");
  for(uint i = 0; i < sizeof(payload); i++)
    payload[i] = (uchar)(i*7+93);
  check(blk_write(2, 509, payload, sizeof(payload)) == 0 &&
        blk_read(2, 0, after, sizeof(after)) == 0, "unaligned write and readback");
  for(uint i = 0; i < sizeof(after); i++){
    uchar expected = i >= 509 && i < 509+sizeof(payload) ? payload[i-509] : before[i];
    if(after[i] != expected)
      check(0, "neighbor bytes preserved");
  }
  check(1, "neighbor bytes preserved");
  check(blk_read(3, 0, after, 1027) == 0, "other device unchanged");
  for(uint i = 0; i < 1027; i++)
    if(after[i] != (uchar)(i*17+3))
      check(0, "write device isolation");
  check(1, "write device isolation");
  check(blk_write(2, size2-7, payload, 7) == 0 &&
        blk_read(2, size2-7, after, 7) == 0 &&
        memcmp(payload, after, 7) == 0 &&
        blk_write(2, size2-1, payload, 2) == -1, "capacity end write");
#ifdef BLKDEV_READONLY_TEST
  check(blk_write(3, 0, payload, 1) == -1, "read-only device rejected write");
#else
  check(blk_write(3, 509, payload, sizeof(payload)) == 0, "third device write");
#endif
  check(blk_flush(2) == 0 && blk_flush(3) == 0 && blk_flush(4) == -1,
        "real VirtIO flush and invalid device");
  printf("D1 ALL TESTS PASSED\n");
}
