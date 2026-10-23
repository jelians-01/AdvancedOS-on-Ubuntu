#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/vfs_dirent.h"
#include "user/user.h"

static char fill[101 * 1024];

static void
require(int ok, const char *what)
{
  if(!ok){
    fprintf(2, "ext2dirgrow: FAIL: %s\n", what);
    exit(1);
  }
}

static void
path_for(char *path, int i, int length)
{
  strcpy(path, "/mnt/");
  memset(path + 5, 'a', length);
  path[5] = 'g';
  path[6] = '0' + i / 100;
  path[7] = '0' + i / 10 % 10;
  path[8] = '0' + i % 10;
  path[5 + length] = 0;
}

static void
directory_size(int bytes)
{
  struct stat st;
  require(stat("/mnt", &st) == 0 && st.size == bytes, "root directory size");
}

static void
verify(int count, int length, int contents)
{
  int seen[108];
  char path[200];
  memset(seen, 0, sizeof(seen));
  struct vfs_dirent de;
  int fd = open("/mnt", O_RDONLY), r, total = 0;
  require(fd >= 0, "directory open");
  while((r = readdir(fd, &de)) == 1){
    if(de.name[0] != 'g') continue;
    int i = (de.name[1] - '0') * 100 + (de.name[2] - '0') * 10 + de.name[3] - '0';
    require(i >= 0 && i < count, "enumerated number");
    path_for(path, i, length);
    require(strcmp(de.name, path + 5) == 0 && seen[i] == 0, "name/duplicate");
    seen[i] = 1;
    total++;
  }
  require(r == 0 && total == count, "complete enumeration");
  close(fd);
  for(int i = 0; i < count; i++){
    path_for(path, i, length);
    fd = open(path, O_RDONLY);
    char byte = 0;
    struct stat st;
    require(fd >= 0 && fstat(fd, &st) == 0 && st.size == contents, "file size");
    if(contents)
      require(read(fd, &byte, 1) == 1 && byte == (char)i, "file byte");
    require(read(fd, &byte, 1) == 0, "EOF");
    close(fd);
  }
}

int
main(int argc, char **argv)
{
  int blocks = argc == 2 && strcmp(argv[1], "blocks") == 0;
  int inodes = argc == 2 && strcmp(argv[1], "inodes") == 0;
  int reboot = argc == 2 && strcmp(argv[1], "verify") == 0;
  int edge = blocks || inodes;
  int count = edge ? 7 : 108, length = edge ? 120 : 96;
  char path[200];
  if(reboot){
    verify(108, 96, 1);
    directory_size(12 * 1024);
    printf("ext2dirgrow: PERSISTENCE PASSED\n");
    exit(0);
  }
  for(int i = 0; i < count; i++){
    path_for(path, i, length);
    int fd = open(path, O_CREATE | O_RDWR);
    require(fd >= 0, "create through growth");
    if(!edge){
      char byte = i;
      require(write(fd, &byte, 1) == 1, "file write");
    }
    close(fd);
  }
  directory_size(edge ? 1024 : 12 * 1024);
  verify(count, length, !edge);
  if(blocks){
    // Prepared 128 KiB volume has 102 free blocks: 101 data + 1 index.
    path_for(path, 0, length);
    int fd = open(path, O_WRONLY);
    memset(fill, 'x', sizeof(fill));
    require(fd >= 0 && write(fd, fill, sizeof(fill)) == sizeof(fill), "fill disk");
    close(fd);
  }
  path_for(path, count, length);
  for(int i = 0; i < 3; i++)
    require(open(path, O_CREATE | O_RDWR) == -1, "growth limit retry");
  directory_size(edge ? 1024 : 12 * 1024);
  // Even with no directory room/inodes/blocks, opening an existing file works.
  path_for(path, count - 1, length);
  struct stat before, after;
  require(stat(path, &before) == 0, "duplicate before");
  int fd = open(path, O_CREATE | O_RDWR);
  require(fd >= 0 && fstat(fd, &after) == 0 && before.ino == after.ino &&
          before.size == after.size, "duplicate unchanged");
  close(fd);
  if(blocks){
    path_for(path, 0, length);
    fd = open(path, O_WRONLY | O_TRUNC);
    require(fd >= 0, "reclaim filler");
    close(fd);
    path_for(path, count, length);
    fd = open(path, O_CREATE | O_RDWR);
    require(fd >= 0, "growth after ENOSPC");
    close(fd);
    count++;
    directory_size(2048);
  }
  verify(count, length, !edge);
  printf("ext2dirgrow: %s PASSED\n", blocks ? "BLOCKS" : inodes ? "INODES" : "LIMIT");
  exit(0);
}
