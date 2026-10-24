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
test_mkdir_and_nested_write(void)
{
  static const char payload[] = "directory write works\n";
  static const char deep_payload[] = "deeper\n";
  struct vfs_dirent de;
  struct stat st, parent_st;
  int fd, r, found = 0;

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
  test_write_create_truncate();
  test_mkdir_and_nested_write();
  check(stat("/fat/empty.txt", &st) == 0 && st.size == 0,
        "empty file attributes");
  check(stat("/fat/late.txt", &st) == 0 && st.size == 0,
        "root directory continuation");
  printf("fat32test: ALL TESTS PASSED\n");
  exit(0);
}
