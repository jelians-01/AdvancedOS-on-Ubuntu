#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static void
fail(const char *what)
{
  fprintf(2, "ext2test: FAIL: %s\n", what);
  exit(1);
}

static void
test_hello(void)
{
  static const char expected[] = "hello from ext2\n";
  char buf[sizeof(expected)];
  struct stat st;
  int fd = open("/mnt/hello.txt", O_RDONLY);

  if(fd < 0)
    fail("open hello.txt");
  if(fstat(fd, &st) < 0 || st.type != T_FILE || st.size != sizeof(expected) - 1)
    fail("hello.txt attributes");
  if(read(fd, buf, sizeof(expected) - 1) != sizeof(expected) - 1 ||
     memcmp(buf, expected, sizeof(expected) - 1) != 0)
    fail("hello.txt contents");
  if(read(fd, buf, 1) != 0)
    fail("hello.txt eof");
  close(fd);
}

static void
test_multi(void)
{
  char buf[197];
  struct stat st;
  int fd = open("/mnt/multi.bin", O_RDONLY);
  uint offset = 0;

  if(fd < 0)
    fail("open multi.bin");
  if(fstat(fd, &st) < 0 || st.type != T_FILE || st.size != 3073)
    fail("multi.bin attributes");
  while(offset < st.size){
    int want = sizeof(buf);
    int got;
    if(want > st.size - offset)
      want = st.size - offset;
    got = read(fd, buf, want);
    if(got != want)
      fail("multi.bin short read");
    for(int i = 0; i < got; i++)
      if((uchar)buf[i] != (offset + i) % 251)
        fail("multi.bin content mismatch");
    offset += got;
  }
  close(fd);
}

static void
test_directory(void)
{
  struct vfs_dirent de;
  int fd = open("/mnt", O_RDONLY);
  int hello = 0, multi = 0, r;

  if(fd < 0)
    fail("open mounted root");
  while((r = readdir(fd, &de)) == 1){
    if(strcmp(de.name, "hello.txt") == 0)
      hello = 1;
    if(strcmp(de.name, "multi.bin") == 0)
      multi = 1;
  }
  close(fd);
  if(r != 0 || !hello || !multi)
    fail("root directory enumeration");
}

int
main(void)
{
  test_directory();
  test_hello();
  test_multi();
  printf("ext2test: ALL TESTS PASSED\n");
  exit(0);
}
