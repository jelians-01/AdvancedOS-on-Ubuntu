#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static const char first[] = "/fat/Report 初稿 🧪.txt";
static const char second[] = "/fat/Report 终稿 🧪.txt";
static const char directory[] = "/fat/目录 Long 🧪";
static const char nested[] = "/fat/目录 Long 🧪/nested child 文件.txt";
static char payload[1300], readback[1300];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "fat32vwrite: FAIL: %s\n", what);
    exit(1);
  }
}

static void
put(const char *path, const char *bytes, int len)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  check(fd >= 0, "create");
  check(write(fd, bytes, len) == len, "write");
  close(fd);
}

int
main(void)
{
  struct stat a, b;
  struct vfs_dirent de;
  int fd, stale, r, first_names = 0, exact_names = 0;

  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = (i * 17 + 9) & 255;
  put(first, payload, sizeof(payload));
  put(second, "second", 6);
  check(stat(first, &a) == 0 && stat("/fat/REPORT~1.TXT", &b) == 0 &&
        a.ino == b.ino, "first alias");
  check(stat(second, &a) == 0 && stat("/fat/REPORT~2.TXT", &b) == 0 &&
        a.ino == b.ino, "alias collision suffix");
  stale = open(first, O_RDWR);
  check(stale >= 0, "hold old reference");
  check(unlink("/fat/REPORT~1.TXT") == 0 &&
        stat(first, &a) < 0, "delete by short alias");
  put(first, "kept", 4);
  check(write(stale, "x", 1) < 0, "deleted handle refuses writes");
  check(read(stale, readback, sizeof(readback)) == sizeof(readback) &&
        memcmp(payload, readback, sizeof(payload)) == 0,
        "old handle still reads original data after name reuse");
  close(stale);
  check(unlink(second) == 0 && stat("/fat/REPORT~2.TXT", &a) < 0,
        "delete by long name");

  check(mkdir(directory) == 0, "create long directory");
  put(nested, "nested", 6);
  check(unlink(directory) < 0, "reject nonempty long directory");
  check(unlink(nested) == 0 && unlink(directory) == 0 &&
        stat(directory, &a) < 0, "remove child and long directory");

  put("/fat/Exactly13.txt", "exact", 5);
  check(open("/fat/bad\xc0\xaf.txt", O_CREATE | O_WRONLY) < 0 &&
        open("/fat/trailing.", O_CREATE | O_WRONLY) < 0 &&
        mkdir("/fat/invalid:name") < 0, "reject invalid names");
  fd = open("/fat", O_RDONLY);
  check(fd >= 0, "open root");
  while((r = readdir(fd, &de)) == 1){
    if(strcmp(de.name, first + 5) == 0)
      first_names++;
    if(strcmp(de.name, "Exactly13.txt") == 0)
      exact_names++;
    check(strcmp(de.name, second + 5) != 0 &&
          strcmp(de.name, directory + 5) != 0, "deleted names absent");
  }
  close(fd);
  check(r == 0 && first_names == 1 && exact_names == 1,
        "long name enumeration including unpadded entry");
  fd = open(first, O_RDONLY);
  check(fd >= 0 && read(fd, readback, 4) == 4 &&
        memcmp(readback, "kept", 4) == 0, "recreated data");
  close(fd);
  check(stat("/fat/F0000000", &a) == 0, "neighbors remain intact");
  printf("fat32vwrite: PASS\n");
  exit(0);
}
