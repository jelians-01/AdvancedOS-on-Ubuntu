#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

#define DIR "vfsdir.tmp"
#define LONG DIR "/abcdefghijklmn"
#define SHARED "vfsdir.share"
#define NFILES 32

static void
check(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "vfsdir: FAILED %s\n", what);
    exit(1);
  }
}

static void
create(const char *path)
{
  int fd = open(path, O_CREATE | O_WRONLY);
  check(fd >= 0, "create");
  check(write(fd, "x", 1) == 1, "write");
  close(fd);
}

static void
valid(struct vfs_dirent *de)
{
  check(de->namelen > 0 && de->namelen <= VFS_NAME_MAX, "name length");
  check(strlen(de->name) == de->namelen, "terminated full name");
  check(de->reserved == 0 && de->type == VFS_DT_UNKNOWN, "native metadata");
  for(uint i = de->namelen; i < sizeof(de->name); i++)
    check(de->name[i] == 0, "zero name tail");
}

static void
basic(void)
{
  struct vfs_dirent de, saved;
  struct dirent raw;
  struct stat st;
  int fd, other, copy, pipes[2], r, mask = 0;

  check(mkdir(DIR) == 0, "mkdir");
  create(DIR "/gone");
  create(LONG);
  check(mkdir(DIR "/child") == 0, "child directory");
  create(DIR "/tail");
  check(unlink(DIR "/gone") == 0 && unlink(DIR "/tail") == 0, "holes");

  fd = open(DIR, O_RDONLY);
  check(fd >= 0, "open directory");
  check(readdir(-1, &de) == -1 && readdir(9999, &de) == -1, "invalid fd");
  check(readdir(fd, (struct vfs_dirent *)~(uint64)0) == -1, "invalid output");
  check(readdir(fd, &de) == 1 && strcmp(de.name, ".") == 0,
        "failed copy preserves cursor");
  valid(&de);
  check(fstat(fd, &st) == 0 && de.ino == st.ino, "directory identity");
  copy = dup(fd);
  check(copy >= 0 && readdir(copy, &de) == 1 &&
        strcmp(de.name, "..") == 0, "dup shares cursor");
  valid(&de);
  close(copy);
  other = open(DIR, O_RDONLY);
  check(other >= 0 && readdir(other, &de) == 1 &&
        strcmp(de.name, ".") == 0, "independent open");
  close(other);
  check(read(fd, &raw, sizeof(raw)) == sizeof(raw) &&
        raw.name[0] == '.' && raw.name[1] == 0, "legacy read independent");
  while((r = readdir(fd, &de)) == 1){
    valid(&de);
    if(strcmp(de.name, "abcdefghijklmn") == 0){
      check(!(mask & 1) && de.namelen == DIRSIZ, "full native name");
      check(stat(LONG, &st) == 0 && st.ino == de.ino, "file identity");
      mask |= 1;
    } else if(strcmp(de.name, "child") == 0){
      check(!(mask & 2), "duplicate child");
      mask |= 2;
    } else {
      check(0, "unexpected name or hole");
    }
  }
  check(r == 0 && mask == 3, "complete enumeration");
  memset(&de, 0xa5, sizeof(de));
  saved = de;
  check(readdir(fd, &de) == 0 && memcmp(&de, &saved, sizeof(de)) == 0,
        "EOF leaves output");
  check(readdir(fd, &de) == 0, "repeat EOF");
  close(fd);
  check(readdir(fd, &de) == -1, "closed fd");

  fd = open(LONG, O_RDONLY);
  check(fd >= 0 && readdir(fd, &de) == -1, "regular file");
  close(fd);
  fd = open(LONG, O_WRONLY);
  check(fd >= 0 && readdir(fd, &de) == -1, "write-only file");
  close(fd);
  check(pipe(pipes) == 0, "pipe");
  check(readdir(pipes[0], &de) == -1 && readdir(pipes[1], &de) == -1,
        "pipe rejection");
  close(pipes[0]);
  close(pipes[1]);
  fd = open("console", O_RDONLY);
  check(fd >= 0 && readdir(fd, &de) == -1, "device rejection");
  close(fd);

  // An open directory retains its inode after removal.
  fd = open(DIR "/child", O_RDONLY);
  check(fd >= 0 && unlink(DIR "/child") == 0, "remove open directory");
  check(readdir(fd, &de) == 1 && strcmp(de.name, ".") == 0,
        "unlinked directory remains readable");
  close(fd);
  check(unlink(LONG) == 0 && unlink(DIR) == 0, "basic cleanup");
}

static int
entry_id(struct vfs_dirent *de)
{
  valid(de);
  if(strcmp(de->name, ".") == 0)
    return 0;
  if(strcmp(de->name, "..") == 0)
    return 1;
  check(de->namelen == 3 && de->name[0] == 'n' &&
        de->name[1] >= '0' && de->name[1] <= '9' &&
        de->name[2] >= '0' && de->name[2] <= '9', "shared name");
  int id = (de->name[1] - '0') * 10 + de->name[2] - '0';
  check(id < NFILES, "shared name range");
  return id + 2;
}

static void
remember(uint64 *seen, int id)
{
  check(id >= 0 && id < NFILES + 2, "shared entry range");
  uint64 bit = (uint64)1 << id;
  check(!(*seen & bit), "duplicate shared entry");
  *seen |= bit;
}

static void
shared(void)
{
  char path[] = SHARED "/n00";
  struct vfs_dirent de;
  int fd, pipes[2], pid, status, r, id;
  uint64 seen = 0;

  check(mkdir(SHARED) == 0, "shared mkdir");
  for(int i = 0; i < NFILES; i++){
    path[sizeof(path)-3] = '0' + i / 10;
    path[sizeof(path)-2] = '0' + i % 10;
    create(path);
  }
  fd = open(SHARED, O_RDONLY);
  check(fd >= 0 && pipe(pipes) == 0, "shared setup");
  pid = fork();
  check(pid >= 0, "fork");
  if(pid == 0){
    close(pipes[0]);
    while((r = readdir(fd, &de)) == 1){
      id = entry_id(&de);
      check(write(pipes[1], &id, sizeof(id)) == sizeof(id), "child report");
    }
    check(r == 0, "child EOF");
    close(fd);
    close(pipes[1]);
    exit(0);
  }
  close(pipes[1]);
  while((r = readdir(fd, &de)) == 1)
    remember(&seen, entry_id(&de));
  check(r == 0, "parent EOF");
  close(fd);
  while((r = read(pipes[0], &id, sizeof(id))) == sizeof(id))
    remember(&seen, id);
  check(r == 0, "reports EOF");
  close(pipes[0]);
  check(wait(&status) == pid && status == 0, "child success");
  check(seen == (((uint64)1 << (NFILES+2)) - 1), "all shared entries once");

  // New open starts at zero, including after a file-table slot is reused.
  fd = open(SHARED, O_RDONLY);
  check(fd >= 0 && readdir(fd, &de) == 1 &&
        strcmp(de.name, ".") == 0, "reopen starts at zero");
  close(fd);
  for(int i = 0; i < NFILES; i++){
    path[sizeof(path)-3] = '0' + i / 10;
    path[sizeof(path)-2] = '0' + i % 10;
    check(unlink(path) == 0, "shared cleanup file");
  }
  check(unlink(SHARED) == 0, "shared cleanup directory");
}

int
main(void)
{
  check(sizeof(struct vfs_dirent) == 272, "ABI size");
  basic();
  for(int i = 0; i < 4; i++)
    shared();
  printf("vfsdir: ALL TESTS PASSED\n");
  exit(0);
}
