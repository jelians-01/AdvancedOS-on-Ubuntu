#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define DIRECT_BYTES (12 * 1024)
#define LIMIT ((12 + 256) * 1024)
static char payload[LIMIT + 1];

static void
require(int ok, const char *message)
{
  if(!ok){
    fprintf(2, "ext2single: FAIL: %s\n", message);
    exit(1);
  }
}

static int
create(const char *path)
{
  int fd = open(path, O_CREATE | O_RDWR | O_TRUNC);
  require(fd >= 0, path);
  return fd;
}

static void
check(const char *path, int size, int patched)
{
  struct stat st;
  char buf[997];
  int fd = open(path, O_RDONLY);
  require(fd >= 0 && fstat(fd, &st) == 0 && st.size == size, "size");
  for(int off = 0; off < size;){
    int n = size - off;
    if(n > sizeof(buf)) n = sizeof(buf);
    require(read(fd, buf, n) == n, "read count");
    for(int i = 0; i < n; i++){
      int pos = off + i;
      uchar expected = pos % 251;
      if(patched && pos >= DIRECT_BYTES - 3 && pos < DIRECT_BYTES + 5)
        expected = "BOUNDARY"[pos - (DIRECT_BYTES - 3)];
      require((uchar)buf[i] == expected, "all bytes");
    }
    off += n;
  }
  require(read(fd, buf, 1) == 0, "EOF");
  close(fd);
}

static void
copy(const char *src, const char *dst)
{
  char *buf = malloc(4093);
  require(buf != 0, "copy buffer");
  int in = open(src, O_RDONLY), out = create(dst), n;
  require(in >= 0, "copy source");
  while((n = read(in, buf, 4093)) > 0)
    require(write(out, buf, n) == n, "copy write");
  require(n == 0, "copy read");
  close(in); close(out); free(buf);
}

static void
verify(void)
{
  check("/mnt/s-max", LIMIT, 0);
  check("/mnt/s-copy", 30001, 0);
  check("/mnt/s-empty", 0, 0);
  check("/mnt/s-short", 6, 0);
  check("/mnt/s-edge", DIRECT_BYTES + 5, 1);
  check("/mnt/indirect.bin", 13000, 0);
}

// 128 KiB test volume has 102 free blocks at preparation. Leave zero/one
// free block at the direct-to-indirect transition, then recover via truncate.
static void
transition(int one)
{
  int target = create("/mnt/target"), filler = create("/mnt/filler");
  require(write(target, payload, DIRECT_BYTES) == DIRECT_BYTES, "target direct");
  int bytes = (one ? 88 : 89) * 1024;
  require(write(filler, payload, bytes) == bytes, "fill remaining blocks");
  close(filler);
  for(int i = 0; i < 3; i++)
    require(write(target, payload + DIRECT_BYTES, 1) == -1, "transition ENOSPC");
  check("/mnt/target", DIRECT_BYTES, 0);
  if(one){
    int last = create("/mnt/last");
    require(write(last, payload, 1024) == 1024, "reservation returned on failure");
    close(last);
  }
  close(create("/mnt/filler"));
  require(write(target, payload + DIRECT_BYTES, 1) == 1, "transition recovery");
  close(target);
  check("/mnt/target", DIRECT_BYTES + 1, 0);
  check("/mnt/filler", 0, 0);
  printf("ext2single: TRANSITION PASSED\n");
}

int
main(int argc, char **argv)
{
  for(int i = 0; i < sizeof(payload); i++) payload[i] = i % 251;
  if(argc == 2 && strcmp(argv[1], "verify") == 0){
    verify();
    printf("ext2single: PERSISTENCE PASSED\n");
    exit(0);
  }
  if(argc == 2 && (strcmp(argv[1], "zero") == 0 || strcmp(argv[1], "one") == 0)){
    transition(strcmp(argv[1], "one") == 0);
    exit(0);
  }
  int fd = create("/mnt/s-max");
  require(write(fd, payload, LIMIT + 1) == -1, "oversize refused");
  check("/mnt/s-max", 0, 0);
  require(write(fd, payload, LIMIT) == LIMIT, "full single indirect range");
  require(write(fd, payload, 1) == -1, "past single indirect limit");
  close(fd);
  check("/mnt/s-max", LIMIT, 0);

  fd = create("s-source");
  require(write(fd, payload, 30001) == 30001, "native fixture");
  close(fd);
  copy("s-source", "/mnt/s-copy");
  copy("/mnt/s-copy", "s-return");
  check("s-return", 30001, 0);
  unlink("s-source"); unlink("s-return");

  fd = create("/mnt/s-edge");
  require(write(fd, payload, DIRECT_BYTES) == DIRECT_BYTES, "boundary direct");
  require(write(fd, (void *)0xffffffffffffffffUL, 1) == -1, "bad pointer boundary");
  require(write(fd, payload + DIRECT_BYTES, 5) == 5, "boundary append");
  close(fd);
  fd = open("/mnt/s-edge", O_RDWR);
  require(fd >= 0, "boundary reopen");
  char *skip = malloc(DIRECT_BYTES);
  require(skip != 0, "skip buffer");
  require(read(fd, skip, DIRECT_BYTES - 3) == DIRECT_BYTES - 3, "position before boundary");
  require(write(fd, "BOUNDARY", 8) == 8, "overwrite across boundary");
  free(skip); close(fd);

  // Reclaim a completely filled index block, not just its first entry.
  fd = create("/mnt/s-empty");
  require(write(fd, payload, LIMIT) == LIMIT, "truncate max fixture");
  close(fd);
  close(create("/mnt/s-empty"));
  for(int i = 0; i < 3; i++){
    fd = create("/mnt/s-short");
    require(write(fd, payload, 30001) == 30001, "reuse indirect fixture");
    close(fd);
    fd = create("/mnt/s-short");
    require(write(fd, payload, 6) == 6, "short rewrite");
    close(fd);
  }
  // Exercise a pre-existing image inode produced by debugfs.
  check("/mnt/indirect.bin", 13000, 0);
  fd = create("/mnt/indirect.bin");
  require(write(fd, payload, 13000) == 13000, "imported inode rewrite");
  close(fd);
  verify();
  printf("ext2single: ALL TESTS PASSED\n");
  exit(0);
}
