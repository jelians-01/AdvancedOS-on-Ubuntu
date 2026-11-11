#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static const char long_name[] = "Project 长名字 🧪 Report.txt";
static const char long_path[] = "/fat/project 长名字 🧪 report.TXT";
static const char alias[] = "/fat/PROJEC~1.TXT";

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "fat32vfat: FAIL: %s\n", what);
    exit(1);
  }
}

int
main(void)
{
  struct vfs_dirent de;
  struct stat by_long, by_short;
  int fd, r, names = 0, fallback = 0, aliases = 0;
  char buf[4];

  fd = open("/fat", O_RDONLY);
  check(fd >= 0, "open root");
  while((r = readdir(fd, &de)) == 1){
    if(strcmp(de.name, long_name) == 0)
      names++;
    if(strcmp(de.name, "BROKEN~1.TXT") == 0)
      fallback++;
    if(strcmp(de.name, "PROJEC~1.TXT") == 0)
      aliases++;
  }
  close(fd);
  check(r == 0 && names == 1 && fallback == 1 && aliases == 0,
        "list cross-cluster UTF-8 name once; fall back on bad checksum");
  check(stat(long_path, &by_long) == 0 && stat(alias, &by_short) == 0 &&
        by_long.ino == by_short.ino, "long and short paths identify the same file");
  fd = open(long_path, O_RDONLY);
  check(fd >= 0 && read(fd, buf, sizeof(buf)) == sizeof(buf) &&
        memcmp(buf, "vfat", sizeof(buf)) == 0, "read by long name");
  close(fd);
  check(open("/fat/Broken long name.txt", O_RDONLY) < 0,
        "reject corrupt long alias");
  check(unlink("/fat/BROKEN~1.TXT") < 0,
        "protect corrupt long slots from partial deletion");
  printf("fat32vfat: PASS\n");
  exit(0);
}
