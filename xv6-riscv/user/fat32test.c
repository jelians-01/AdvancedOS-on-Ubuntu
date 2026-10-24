#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static void
fail(const char *what)
{
  fprintf(2, "fat32test: FAIL: %s\n", what);
  exit(1);
}

static void
check(int ok, const char *what)
{
  if(!ok)
    fail(what);
}

static void
test_directory(void)
{
  struct vfs_dirent de;
  int fd = open("/fat", O_RDONLY), r;
  int hello = 0, frag = 0, empty = 0, subdir = 0, late = 0;

  check(fd >= 0, "open FAT32 root");
  while((r = readdir(fd, &de)) == 1){
    if(strcmp(de.name, "HELLO.TXT") == 0) hello = 1;
    if(strcmp(de.name, "FRAG.BIN") == 0) frag = 1;
    if(strcmp(de.name, "EMPTY.TXT") == 0) empty = 1;
    if(strcmp(de.name, "SUBDIR") == 0) subdir = 1;
    if(strcmp(de.name, "LATE.TXT") == 0) late = 1;
  }
  close(fd);
  check(r == 0 && hello && frag && empty && subdir && late,
        "root directory enumeration");
}

static void
test_hello(void)
{
  static const char expected[] = "hello from FAT32\n";
  char buf[sizeof(expected)];
  struct stat st;
  int fd = open("/fat/hello.txt", O_RDONLY);

  check(fd >= 0, "open HELLO.TXT case-insensitively");
  check(fstat(fd, &st) == 0 && st.type == T_FILE &&
        st.size == sizeof(expected) - 1, "HELLO.TXT attributes");
  check(read(fd, buf, sizeof(expected) - 1) == sizeof(expected) - 1 &&
        memcmp(buf, expected, sizeof(expected) - 1) == 0,
        "HELLO.TXT contents");
  check(read(fd, buf, 1) == 0, "HELLO.TXT EOF");
  close(fd);
}

static void
test_fragmented(void)
{
  static uchar buf[1300];
  int fd = open("/fat/frag.bin", O_RDONLY);

  check(fd >= 0, "open FRAG.BIN");
  check(read(fd, buf, sizeof(buf)) == sizeof(buf), "FRAG.BIN length");
  for(int i = 0; i < sizeof(buf); i++)
    if(buf[i] != (uchar)((i * 37 + 11) % 256))
      fail("FRAG.BIN fragmented content");
  check(read(fd, buf, 1) == 0, "FRAG.BIN EOF");
  close(fd);
}

static void
test_nested(void)
{
  static const char expected[] = "nested read works\n";
  char buf[sizeof(expected)];
  struct stat st;
  int fd = open("/fat/subdir/nest.txt", O_RDONLY);

  check(fd >= 0, "open nested NEST.TXT");
  check(fstat(fd, &st) == 0 && st.size == sizeof(expected) - 1,
        "NEST.TXT attributes");
  check(read(fd, buf, sizeof(expected) - 1) == sizeof(expected) - 1 &&
        memcmp(buf, expected, sizeof(expected) - 1) == 0,
        "NEST.TXT contents");
  close(fd);
}

static void
test_write_create_truncate(void)
{
  static uchar payload[1700];
  static uchar buf[1700];
  int fd, r;
  struct stat st;

  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = (uchar)((i * 13 + 7) % 256);
  fd = open("/fat/new.txt", O_CREATE | O_RDWR | O_TRUNC);
  check(fd >= 0, "create FAT32 NEW.TXT");
  check(write(fd, payload, sizeof(payload)) == sizeof(payload),
        "write FAT32 NEW.TXT across clusters");
  close(fd);
  fd = open("/fat/new.txt", O_RDONLY);
  check(fd >= 0 && fstat(fd, &st) == 0 && st.size == sizeof(payload),
        "stat FAT32 NEW.TXT");
  r = read(fd, buf, sizeof(buf));
  check(r == sizeof(payload) && memcmp(buf, payload, sizeof(payload)) == 0,
        "read FAT32 NEW.TXT");
  close(fd);

  fd = open("/fat/new.txt", O_WRONLY | O_TRUNC);
  check(fd >= 0, "open FAT32 NEW.TXT for truncate");
  close(fd);
  check(stat("/fat/new.txt", &st) == 0 && st.size == 0,
        "truncate FAT32 NEW.TXT");

  fd = open("/fat/new.txt", O_WRONLY);
  check(fd >= 0 && write(fd, "ok\n", 3) == 3,
        "rewrite truncated FAT32 NEW.TXT");
  close(fd);
}

static void
test_shared_nodes(void)
{
  char buf[8];
  struct stat st;
  int a = open("/fat/shared.txt", O_CREATE | O_RDWR | O_TRUNC);
  int b = open("/fat/shared.txt", O_RDWR);
  check(a >= 0 && b >= 0, "open one FAT32 file twice");
  check(write(a, "first", 5) == 5, "write through first FAT32 handle");
  check(fstat(b, &st) == 0 && st.size == 5,
        "second FAT32 handle observes growth");
  check(read(b, buf, 5) == 5 && memcmp(buf, "first", 5) == 0,
        "second FAT32 handle observes new cluster chain");
  int c = open("/fat/shared.txt", O_WRONLY | O_TRUNC);
  check(c >= 0, "truncate shared FAT32 node");
  close(c);
  check(fstat(a, &st) == 0 && st.size == 0 &&
        fstat(b, &st) == 0 && st.size == 0,
        "all FAT32 handles observe truncate");
  close(a);
  close(b);
  check(unlink("/fat/shared.txt") == 0, "remove shared-node fixture");
}

static void
test_mkdir_and_nested_write(void)
{
  static const char payload[] = "directory write works\n";
  static const char deep_payload[] = "deeper\n";
  struct vfs_dirent de;
  struct stat st, parent_st;
  int fd, readfd, r, found = 0;

  check(mkdir("/fat/newdir") == 0, "create FAT32 directory");
  check(mkdir("/fat/newdir") < 0, "reject duplicate FAT32 directory");
  check(mkdir("/fat/newdir/inner") == 0,
        "create nested FAT32 directory");
  check(mkdir("/fat/newdir/inner") < 0,
        "reject duplicate nested FAT32 directory");
  check(stat("/fat/newdir", &parent_st) == 0 &&
        stat("/fat/newdir/inner/..", &st) == 0 &&
        st.type == T_DIR && st.ino == parent_st.ino,
        "FAT32 dot-dot resolves to parent");
  check(mkdir("/fat/newdir/inner/third") == 0 &&
        mkdir("/fat/newdir/inner/third/fourth") == 0,
        "create four FAT32 directory levels");
  check(stat("/fat/newdir/inner/third/fourth/../../..", &st) == 0 &&
        st.ino == parent_st.ino,
        "FAT32 repeated parent traversal retains ancestry");
  check(unlink("/fat/newdir/inner/third/fourth") == 0 &&
        unlink("/fat/newdir/inner/third") == 0,
        "remove deep FAT32 directory fixture");
  fd = open("/fat/newdir/child.txt", O_CREATE | O_RDWR);
  check(fd >= 0, "create file in FAT32 directory");
  check(write(fd, payload, sizeof(payload) - 1) == sizeof(payload) - 1,
        "write file in FAT32 directory");
  close(fd);
  fd = open("/fat/newdir/inner/deep.txt", O_CREATE | O_RDWR);
  check(fd >= 0 &&
        write(fd, deep_payload, sizeof(deep_payload) - 1) ==
          sizeof(deep_payload) - 1,
        "write file in nested FAT32 directory");
  close(fd);

  fd = open("/fat/newdir", O_RDONLY);
  check(fd >= 0, "open FAT32 directory");
  while((r = readdir(fd, &de)) == 1)
    if(strcmp(de.name, "CHILD.TXT") == 0)
      found = 1;
  close(fd);
  check(r == 0 && found, "enumerate FAT32 child directory");

  fd = open("/fat/newdir/child.txt", O_RDONLY);
  check(fd >= 0 && fstat(fd, &st) == 0 && st.type == T_FILE &&
        st.size == sizeof(payload) - 1, "stat nested FAT32 file");
  close(fd);
  fd = open("/fat/newdir/inner/../child.txt", O_RDONLY);
  check(fd >= 0, "resolve FAT32 parent path component");
  close(fd);

  check(unlink("/fat/newdir") < 0,
        "reject removal of nonempty FAT32 directory");
  check(mkdir("/fat/delroot") == 0 &&
        mkdir("/fat/delroot/inner") == 0,
        "create FAT32 deletion tree");
  fd = open("/fat/delroot/inner/gone.txt", O_CREATE | O_RDWR);
  check(fd >= 0 && write(fd, "gone", 4) == 4,
        "create FAT32 deletion file");
  close(fd);
  check(unlink("/fat/delroot") < 0,
        "reject removal of nonempty FAT32 deletion tree");
  check(unlink("/fat/delroot/inner/gone.txt") == 0 &&
        stat("/fat/delroot/inner/gone.txt", &st) < 0,
        "remove FAT32 file from deletion tree");
  fd = open("/fat/delroot/inner", O_RDONLY);
  check(fd >= 0, "open empty directory before unlink");
  check(unlink("/fat/delroot/inner") == 0 &&
        stat("/fat/delroot/inner", &st) < 0,
        "remove open empty FAT32 directory");
  r = readdir(fd, &de);
  check(r == 0, "open directory remains readable after unlink");
  close(fd);
  check(unlink("/fat/delroot") == 0 &&
        stat("/fat/delroot", &st) < 0,
        "remove FAT32 file and empty directory tree");

  fd = open("/fat/open.txt", O_CREATE | O_RDWR);
  check(fd >= 0 && write(fd, "open data", 9) == 9,
        "create open-unlink FAT32 fixture");
  readfd = open("/fat/open.txt", O_RDONLY);
  check(readfd >= 0, "open FAT32 file before unlink");
  check(unlink("/fat/open.txt") == 0 && stat("/fat/open.txt", &st) < 0,
        "unlink open FAT32 file");
  char open_buf[9];
  check(read(readfd, open_buf, sizeof(open_buf)) == sizeof(open_buf) &&
        memcmp(open_buf, "open data", sizeof(open_buf)) == 0,
        "open FAT32 file remains readable after unlink");
  check(write(fd, "x", 1) < 0,
        "unlinked FAT32 file rejects further writes");
  close(readfd);
  close(fd);
  fd = open("/fat/open.txt", O_CREATE | O_RDWR);
  check(fd >= 0 && write(fd, "new", 3) == 3,
        "recreate unlinked FAT32 name");
  close(fd);
}

static void
test_large_cluster(void)
{
  static uchar payload[9000], result[9000];
  struct stat st;
  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = (uchar)((i * 29 + 3) % 251);
  int fd = open("/fat/large.bin", O_CREATE | O_RDWR | O_TRUNC);
  check(fd >= 0 && write(fd, payload, sizeof(payload)) == sizeof(payload),
        "write across pages and large FAT32 clusters");
  close(fd);
  fd = open("/fat/large.bin", O_RDWR);
  for(int i = 0; i < 5001; i++)
    payload[i] ^= 0x5a;
  check(fd >= 0 && write(fd, payload, 5001) == 5001,
        "overwrite partial large FAT32 cluster");
  close(fd);
  fd = open("/fat/large.bin", O_RDONLY);
  check(fd >= 0 && fstat(fd, &st) == 0 && st.size == sizeof(payload) &&
        read(fd, result, sizeof(result)) == sizeof(result) &&
        memcmp(result, payload, sizeof(payload)) == 0,
        "large FAT32 cluster preserves unwritten suffix");
  close(fd);
  printf("fat32test: LARGE CLUSTER PASSED\n");
}

static void
test_rollback_io_failure(void)
{
  static uchar payload[1024], old[512];
  struct stat st;
  int fd = open("/fat/io.bin", O_RDWR);
  check(fd >= 0 && read(fd, old, sizeof(old)) == sizeof(old),
        "open rollback I/O fixture");
  for(int i = 0; i < sizeof(old); i++)
    check(old[i] == 0x6b, "rollback fixture old bytes");
  memset(payload, 0x39, sizeof(payload));
  check(write(fd, payload, sizeof(payload)) < 0,
        "allocation/rollback injected failure is reported");
  check(write(fd, payload, 1) < 0,
        "FAT32 rejects writes after rollback I/O failure");
  check(fstat(fd, &st) == 0 && st.size == sizeof(old),
        "failed extension does not publish a new file size");
  close(fd);
  fd = open("/fat/io.bin", O_RDONLY);
  check(fd >= 0 && read(fd, old, sizeof(old)) == sizeof(old),
        "read original file after rollback failure");
  for(int i = 0; i < sizeof(old); i++)
    check(old[i] == 0x6b, "rollback failure preserves old bytes");
  close(fd);
  printf("fat32test: ROLLBACK IO FAILURE PASSED\n");
}

int
main(int argc, char **argv)
{
  struct stat st;
  if(argc == 2 && strcmp(argv[1], "iofail") == 0){
    test_rollback_io_failure();
    exit(0);
  }
  if(argc == 2 && strcmp(argv[1], "large") == 0){
    test_large_cluster();
    exit(0);
  }
  check(stat("/fat", &st) == 0 && st.type == T_DIR, "FAT32 mount type");
  test_directory();
  test_hello();
  test_fragmented();
  test_nested();
  test_write_create_truncate();
  test_shared_nodes();
  test_mkdir_and_nested_write();
  check(stat("/fat/empty.txt", &st) == 0 && st.size == 0,
        "empty file attributes");
  check(stat("/fat/late.txt", &st) == 0 && st.size == 0,
        "root directory continuation");
  printf("fat32test: ALL TESTS PASSED\n");
  exit(0);
}
