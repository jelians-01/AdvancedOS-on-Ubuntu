#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static char payload[13000], buffer[13000];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "ext2unlink: FAIL: %s\n", what);
    exit(1);
  }
}

static void
put(const char *path, const char *bytes, int n)
{
  int fd = open(path, O_CREATE | O_RDWR);
  check(fd >= 0 && write(fd, bytes, n) == n, "file create/write");
  close(fd);
}

static void
filler_path(char *path, int i)
{
  strcpy(path, "/mnt/remove/");
  memset(path + 12, 'a', 80);
  path[12] = 'f';
  path[13] = '0' + i / 10;
  path[14] = '0' + i % 10;
  path[92] = 0;
}

int
main(void)
{
  struct stat root, old, fresh, st;
  struct vfs_dirent de;
  char path[128];
  int a, b, fd, r, dots = 0;

  check(stat("/mnt", &root) == 0, "initial root");
  check(stat("/mnt/seed/keep.txt", &st) == 0 && st.nlink == 2 &&
        unlink("/mnt/seed/other.txt") == 0 &&
        stat("/mnt/seed/keep.txt", &st) == 0 && st.nlink == 1,
        "remove one imported hard link without reclaiming its inode");
  check(mkdir("/mnt/remove") == 0 && mkdir("/mnt/remove/inner") == 0,
        "create nested directories");
  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = i % 251;
  put("/mnt/remove/inner/data", payload, sizeof(payload));
  check(unlink("/mnt/remove") < 0 && unlink("/mnt/remove/inner") < 0,
        "reject nonempty directories");
  a = open("/mnt/remove/inner/data", O_RDWR);
  b = open("/mnt/remove/inner/data", O_RDONLY);
  check(a >= 0 && b >= 0 && fstat(a, &old) == 0, "pin old file");
  check(unlink("/mnt/remove/inner/data") == 0 &&
        stat("/mnt/remove/inner/data", &st) < 0 &&
        fstat(a, &st) == 0 && st.nlink == 0 && st.size == sizeof(payload),
        "unlink removes name but preserves open file");
  check(write(a, "x", 1) < 0, "deleted file refuses mutation");
  put("/mnt/remove/inner/data", "new", 3);
  check(stat("/mnt/remove/inner/data", &fresh) == 0 && fresh.ino != old.ino,
        "same-name recreation uses a different pinned inode");
  close(a);
  check(read(b, buffer, sizeof(buffer)) == sizeof(buffer) &&
        memcmp(payload, buffer, sizeof(buffer)) == 0,
        "old data survives other handle close");
  close(b);
  fd = open("/mnt/remove/inner/data", O_RDONLY);
  check(fd >= 0 && read(fd, buffer, 3) == 3 && memcmp(buffer, "new", 3) == 0,
        "recreated file data intact");
  close(fd);
  check(unlink("/mnt/remove/inner/data") == 0 &&
        unlink("/mnt/remove/inner/data") < 0 &&
        unlink("/mnt/remove/inner") == 0, "remove file and now-empty directory");
  check(stat("/mnt/remove", &st) == 0 && st.nlink == 2,
        "parent directory link decrement");

  for(int i = 0; i < 14; i++){
    filler_path(path, i);
    fd = open(path, O_CREATE | O_RDWR);
    check(fd >= 0, "create expansion filler");
    close(fd);
  }
  check(stat("/mnt/remove", &st) == 0 && st.size >= 2048,
        "expanded directory");
  for(int i = 0; i < 14; i++){
    filler_path(path, i);
    check(unlink(path) == 0, "reclaim empty file inode");
  }
  fd = open("/mnt/remove", O_RDONLY);
  check(fd >= 0 && fstat(fd, &old) == 0 && chdir("/mnt/remove") == 0,
        "pin directory with fd and cwd");
  check(unlink("/mnt/remove") == 0 && fstat(fd, &st) == 0 && st.nlink == 0,
        "unlink open empty directory");
  check(open("ghost", O_CREATE | O_RDWR) < 0 && mkdir("ghost") < 0,
        "deleted cwd refuses creation");
  check(mkdir("/mnt/remove") == 0 && stat("/mnt/remove", &fresh) == 0 &&
        fresh.ino != old.ino, "directory name reuse isolated");
  check(chdir("/") == 0, "leave deleted cwd");
  while((r = readdir(fd, &de)) == 1){
    check(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0,
          "old directory retains only dot entries");
    dots++;
  }
  check(r == 0 && dots == 2, "enumerate old directory after name reuse");
  close(fd);
  check(unlink("/mnt/remove") == 0 && unlink("/mnt") < 0 &&
        unlink("/mnt/.") < 0 && unlink("/mnt/..") < 0,
        "directory cleanup and mount/dot protection");
  check(stat("/mnt", &st) == 0 && st.nlink == root.nlink &&
        stat("/mnt/remove", &st) < 0, "root links restored");
  printf("ext2unlink: PASS\n");
  exit(0);
}
