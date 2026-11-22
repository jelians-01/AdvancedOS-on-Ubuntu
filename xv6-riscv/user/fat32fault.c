#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void
check(int ok, const char *what, const char *path)
{
  if(!ok){
    fprintf(2, "fat32fault: FAIL: %s: %s\n", what, path);
    exit(1);
  }
}

static void
test_bad_chain(const char *path, char first)
{
  char data[2048];
  int fd = open(path, O_RDONLY);

  check(fd >= 0, "open directory entry", path);
  check(read(fd, data, sizeof(data)) == 512 && data[0] == first,
        "read only the valid prefix", path);
  check(read(fd, data, sizeof(data)) == -1, "error at damaged link", path);
  close(fd);

  fd = open(path, O_WRONLY);
  check(fd >= 0, "open for overwrite", path);
  check(write(fd, "x", 1) == -1, "reject overwrite", path);
  close(fd);
  check(open(path, O_WRONLY | O_TRUNC) == -1, "reject truncate", path);
  check(unlink(path) == -1, "reject unlink", path);
}

int
main(void)
{
  char hello[17];
  int fd;

  test_bad_chain("/fat/LOOP.BIN", 'L');
  test_bad_chain("/fat/SHORT.BIN", 'S');
  test_bad_chain("/fat/BAD.BIN", 'B');
  test_bad_chain("/fat/FRAG.BIN", 11);
  fd = open("/fat/HELLO.TXT", O_RDONLY);
  check(fd >= 0 && read(fd, hello, sizeof(hello)) == sizeof(hello) &&
        memcmp(hello, "hello from FAT32\n", sizeof(hello)) == 0,
        "unrelated file remains readable", "/fat/HELLO.TXT");
  close(fd);
  printf("fat32fault: PASS\n");
  exit(0);
}
