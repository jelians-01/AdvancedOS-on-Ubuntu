#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static const char base[] = "/mnt/links/left/base";
static const char alias[] = "/mnt/links/right/alias";
static const char same[] = "/mnt/links/left/same";
static char payload[13000], buffer[13000];

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "ext2link: FAIL: %s\n", what);
    exit(1);
  }
}

static void
put(const char *path, const char *bytes, int len)
{
  int fd = open(path, O_CREATE | O_RDWR | O_TRUNC);
  check(fd >= 0 && write(fd, bytes, len) == len, "file create/write");
  close(fd);
}

static void
same_inode(const char *a, const char *b, int links)
{
  struct stat x, y;
  check(stat(a, &x) == 0 && stat(b, &y) == 0 &&
        x.dev == y.dev && x.ino == y.ino && x.nlink == links &&
        y.nlink == links && x.size == y.size, "shared inode and link count");
}

static void
path_for(char *path, int i)
{
  strcpy(path, "/mnt/links/right/");
  memset(path + 17, 'a', 80);
  path[17] = 'h';
  path[18] = '0' + i / 10;
  path[19] = '0' + i % 10;
  path[97] = 0;
}

int
main(void)
{
  struct stat old, st, fresh;
  char path[128];
  int a, b, c, fd;

  for(int i = 0; i < sizeof(payload); i++)
    payload[i] = i % 251;
  check(mkdir("/mnt/links") == 0 && mkdir("/mnt/links/left") == 0 &&
        mkdir("/mnt/links/right") == 0, "test directories");
  put(base, payload, sizeof(payload));
  check(link(base, alias) == 0 && link(base, same) == 0, "cross-directory hard links");
  same_inode(base, alias, 3);
  same_inode(base, same, 3);
  for(int i = 0; i < 14; i++){
    path_for(path, i);
    check(link(base, path) == 0, "hard link through directory growth");
  }
  check(stat("/mnt/links/right", &st) == 0 && st.size >= 2048 && st.nlink == 2,
        "directory growth without directory link-count change");
  same_inode(base, alias, 17);
  for(int i = 0; i < 14; i++){
    path_for(path, i);
    check(unlink(path) == 0, "remove expansion aliases");
  }
  same_inode(base, alias, 3);
  a = open(base, O_RDWR);
  b = open(alias, O_RDONLY);
  check(a >= 0 && b >= 0 && write(a, "XYZ", 3) == 3, "write via one name");
  memmove(payload, "XYZ", 3);
  check(read(b, buffer, sizeof(buffer)) == sizeof(buffer) &&
        memcmp(payload, buffer, sizeof(buffer)) == 0, "read via other name");
  fd = open(same, O_WRONLY | O_TRUNC);
  check(fd >= 0, "truncate via third name");
  close(fd);
  check(fstat(a, &st) == 0 && st.size == 0 &&
        fstat(b, &st) == 0 && st.size == 0, "shared truncation");
  put(same, "fresh", 5);
  c = open(alias, O_RDONLY);
  check(c >= 0 && fstat(c, &old) == 0, "hold final reader");

  check(link(base, alias) < 0 && link(base, base) < 0 &&
        link("/mnt/links/left", "/mnt/links/right/dirbad") < 0,
        "duplicate and directory rejection");
  put("ext2link.tmp", "native", 6);
  check(link("ext2link.tmp", "/mnt/links/foreign") < 0 &&
        link(base, "xlinkforeign") < 0 && unlink("ext2link.tmp") == 0,
        "cross-filesystem rejection");
  check(mkdir("/mnt/links/gone") == 0 && chdir("/mnt/links/gone") == 0 &&
        unlink("/mnt/links/gone") == 0 &&
        link(base, "bad") < 0 && chdir("/") == 0, "deleted parent rejection");
  same_inode(base, alias, 3);

  check(unlink(base) == 0, "remove original name");
  same_inode(alias, same, 2);
  check(unlink(same) == 0 && stat(alias, &st) == 0 && st.nlink == 1,
        "last name retains inode");
  check(unlink(alias) == 0 && fstat(a, &st) == 0 && st.nlink == 0 &&
        write(a, "x", 1) < 0, "last name removed with handles pinned");
  put(base, "new", 3);
  check(stat(base, &fresh) == 0 && fresh.ino != old.ino,
        "new incarnation isolated from old handles");
  close(a); close(b);
  check(read(c, buffer, 5) == 5 && memcmp(buffer, "fresh", 5) == 0,
        "orphan survives until final close");
  close(c);
  check(unlink(base) == 0 && unlink("/mnt/links/left") == 0 &&
        unlink("/mnt/links/right") == 0 && unlink("/mnt/links") == 0,
        "temporary links and directories reclaimed");
  put("/mnt/keep-a", payload, sizeof(payload));
  check(link("/mnt/keep-a", "/mnt/seed/keep-b") == 0, "persistent linked pair");
  same_inode("/mnt/keep-a", "/mnt/seed/keep-b", 2);
  printf("ext2link: PASS\n");
  exit(0);
}
