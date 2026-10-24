#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(void)
{
  char buf[4] = {0};
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
  printf("fat32persist: PASS\n");
  exit(0);
}
