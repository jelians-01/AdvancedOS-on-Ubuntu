#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void check(int ok)
{
  if(!ok){
    fprintf(2, "fat32mbr: FAIL\n");
    exit(1);
  }
}
static void contents(const char *path, const char *expected)
{
  char buffer[16];
  struct stat st;
  int n = strlen(expected), fd = open(path, O_RDONLY);
  check(fd >= 0);
  check(fstat(fd, &st) == 0 && st.dev == 3 && st.size == n);
  check(read(fd, buffer, sizeof(buffer)) == n && memcmp(buffer, expected, n) == 0);
  close(fd);
}
int main(void)
{
  struct stat st;
  contents("/fat/Report 初稿 🧪.txt", "kept");
  contents("/fat/Exactly13.txt", "exact");
  contents("/fat/Shared file.txt", "new");
  check(stat("/fat/Report 终稿 🧪.txt", &st) < 0);
  check(stat("/fat/目录 Long 🧪", &st) < 0);
  check(stat("/fat/F0000000", &st) == 0 && st.size == 0);
  printf("fat32mbr: REBOOT PASS\n");
  exit(0);
}
