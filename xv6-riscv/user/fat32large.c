#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define CLUSTER_BYTES 8192
#define FILE_BYTES (2 * CLUSTER_BYTES + 513)
#define PATCH_OFFSET (CLUSTER_BYTES - 3)

static const char path[] = "/fat/bigdir/data.bin";
static const char patch[] = "cross-cluster overwrite";
static uchar expected[FILE_BYTES], actual[FILE_BYTES];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "fat32large: FAIL: %s\n", what);
    exit(1);
  }
}

int
main(void)
{
  struct stat st;
  int fd;

  for(int i = 0; i < FILE_BYTES; i++)
    expected[i] = (i * 37 + i / 256) & 255;
  check(mkdir("/fat/bigdir") == 0, "allocate directory cluster");
  fd = open(path, O_CREATE | O_WRONLY);
  check(fd >= 0, "create file");
  check(write(fd, expected, FILE_BYTES) == FILE_BYTES, "write across three clusters");
  close(fd);

  fd = open(path, O_RDWR);
  check(fd >= 0 && read(fd, actual, PATCH_OFFSET) == PATCH_OFFSET,
        "reach the unaligned overwrite offset");
  check(write(fd, patch, sizeof(patch) - 1) == sizeof(patch) - 1,
        "overwrite across a cluster boundary");
  close(fd);
  memmove(expected + PATCH_OFFSET, patch, sizeof(patch) - 1);

  fd = open(path, O_RDONLY);
  check(fd >= 0 && fstat(fd, &st) == 0 && st.size == FILE_BYTES,
        "size after overwrite");
  check(read(fd, actual, FILE_BYTES) == FILE_BYTES &&
        memcmp(actual, expected, FILE_BYTES) == 0, "verify data and neighbors");
  close(fd);
  printf("fat32large: PASS\n");
  exit(0);
}
