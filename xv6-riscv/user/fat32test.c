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
test_read_only(void)
{
  static const char replacement[] = "X";
  int fd;
  struct stat st;

  // The VFS may let us open an existing file for writing, but the FAT32
  // backend has no write callback, so data changes must fail.
  fd = open("/fat/hello.txt", O_WRONLY);
  check(fd >= 0, "open existing FAT32 file for write test");
  check(write(fd, replacement, 1) == -1, "reject FAT32 file write");
  close(fd);
  check(open("/fat/hello.txt", O_WRONLY | O_TRUNC) == -1,
        "reject FAT32 truncate");
  check(open("/fat/new.txt", O_CREATE | O_WRONLY) == -1,
        "reject FAT32 create");
  check(stat("/fat/new.txt", &st) == -1,
        "rejected FAT32 create left no directory entry");
  test_hello();
}

int
main(void)
{
  struct stat st;
  check(stat("/fat", &st) == 0 && st.type == T_DIR, "FAT32 mount type");
  test_directory();
  test_hello();
  test_fragmented();
  test_nested();
  test_read_only();
  check(stat("/fat/empty.txt", &st) == 0 && st.size == 0,
        "empty file attributes");
  check(stat("/fat/late.txt", &st) == 0 && st.size == 0,
        "root directory continuation");
  printf("fat32test: ALL TESTS PASSED\n");
  exit(0);
}
