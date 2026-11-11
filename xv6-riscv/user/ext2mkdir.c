#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static uchar payload[13000], readback[13000];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "ext2mkdir: FAIL: %s\n", what);
    exit(1);
  }
}

static void
put(const char *path, const void *bytes, int length)
{
  int fd = open(path, O_CREATE | O_RDWR | O_TRUNC);
  check(fd >= 0 && write(fd, bytes, length) == length, "subdirectory file write");
  close(fd);
}

int
main(void)
{
  struct stat root, parent, child, st;
  struct vfs_dirent de;
  char path[128];
  int fd, r, fillers = 0, inner = 0, short_file = 0;

  check(stat("/mnt", &root) == 0, "initial root attributes");
  check(mkdir("/mnt/tree") == 0 && mkdir("/mnt/tree/inner") == 0,
        "nested mkdir");
  check(stat("/mnt", &st) == 0 && st.nlink == root.nlink + 1,
        "root link count");
  check(stat("/mnt/tree", &parent) == 0 && parent.type == T_DIR &&
        parent.nlink == 3, "parent link count");
  check(stat("/mnt/tree/inner", &child) == 0 && child.nlink == 2 &&
        child.type == T_DIR && child.size == 1024, "new directory attributes");
  check(stat("/mnt/tree/inner/..", &st) == 0 && st.ino == parent.ino &&
        stat("/mnt/tree/inner/../..", &st) == 0 && st.ino == root.ino,
        "dotdot entries");

  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = i % 251;
  put("/mnt/tree/inner/data.bin", payload, sizeof(payload));
  fd = open("/mnt/tree/inner/data.bin", O_WRONLY);
  check(fd >= 0 && write(fd, "XYZ", 3) == 3, "nested overwrite");
  close(fd);
  memmove(payload, "XYZ", 3);
  fd = open("/mnt/tree/inner/data.bin", O_RDONLY);
  check(fd >= 0 && read(fd, readback, sizeof(readback)) == sizeof(readback) &&
        memcmp(payload, readback, sizeof(payload)) == 0,
        "nested single-indirect readback");
  close(fd);
  put("/mnt/tree/short", "old content", 11);
  put("/mnt/tree/short", "ok\n", 3);
  put("/mnt/seed/new.txt", "new in imported directory\n", 26);

  for(int i = 0; i < 14; i++){
    strcpy(path, "/mnt/tree/");
    memset(path + 10, 'a', 80);
    path[10] = 'g';
    path[11] = '0' + i / 10;
    path[12] = '0' + i % 10;
    path[90] = 0;
    fd = open(path, O_CREATE | O_RDWR);
    check(fd >= 0, "create through subdirectory expansion");
    close(fd);
  }
  check(stat("/mnt/tree", &st) == 0 && st.size >= 2048 && st.nlink == 3,
        "subdirectory expands without changing link count");
  fd = open("/mnt/tree", O_RDONLY);
  check(fd >= 0, "open parent");
  while((r = readdir(fd, &de)) == 1){
    if(de.name[0] == 'g' && de.namelen == 80)
      fillers++;
    if(strcmp(de.name, "inner") == 0) inner++;
    if(strcmp(de.name, "short") == 0) short_file++;
  }
  close(fd);
  check(r == 0 && fillers == 14 && inner == 1 && short_file == 1,
        "expanded directory enumeration");
  check(mkdir("/mnt/tree") < 0 &&
        mkdir("/mnt/tree/inner/data.bin") < 0 &&
        open("/mnt/tree/inner", O_CREATE | O_RDWR) < 0,
        "duplicate and wrong-type rejection");
  check(stat("/mnt", &st) == 0 && st.nlink == root.nlink + 1,
        "failed mkdir preserves parent links");
  check(chdir("/mnt/tree/inner") == 0 && stat("../short", &st) == 0 &&
        st.size == 3 && mkdir(".") < 0 && mkdir("..") < 0 && chdir("/") == 0,
        "relative parent path");
  printf("ext2mkdir: PASS\n");
  exit(0);
}
