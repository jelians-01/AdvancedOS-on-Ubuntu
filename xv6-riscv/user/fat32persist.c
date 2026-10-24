#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(void)
{
  char buf[32] = {0};
  struct stat st;
  int fd = open("/fat/new.txt", O_RDONLY);
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != 3 ||
     read(fd, buf, 3) != 3 || memcmp(buf, "ok\n", 3) != 0){
    fprintf(2, "fat32persist: FAIL\n");
    if(fd >= 0)
      close(fd);
    exit(1);
  }
  close(fd);
  memset(buf, 0, sizeof(buf));
  fd = open("/fat/newdir/child.txt", O_RDONLY);
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != 22 ||
     read(fd, buf, 22) != 22 ||
     memcmp(buf, "directory write works\n", 22) != 0){
    fprintf(2, "fat32persist: FAIL nested\n");
    if(fd >= 0)
      close(fd);
    exit(1);
  }
  close(fd);
  memset(buf, 0, sizeof(buf));
  fd = open("/fat/newdir/inner/deep.txt", O_RDONLY);
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != 7 ||
     read(fd, buf, 7) != 7 || memcmp(buf, "deeper\n", 7) != 0){
    fprintf(2, "fat32persist: FAIL deeper\n");
    if(fd >= 0)
      close(fd);
    exit(1);
  }
  close(fd);
  memset(buf, 0, sizeof(buf));
  fd = open("/fat/open.txt", O_RDONLY);
  if(fd < 0 || fstat(fd, &st) < 0 || st.size != 3 ||
     read(fd, buf, 3) != 3 || memcmp(buf, "new", 3) != 0){
    fprintf(2, "fat32persist: FAIL recreated file\n");
    if(fd >= 0)
      close(fd);
    exit(1);
  }
  close(fd);
  if(stat("/fat/delroot", &st) == 0){
    fprintf(2, "fat32persist: FAIL deleted paths\n");
    exit(1);
  }
  printf("fat32persist: PASS\n");
  exit(0);
}
