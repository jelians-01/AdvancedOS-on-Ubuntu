#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  struct stat st;

  if(stat("/", &st) < 0 || st.type != T_DIR){
    fprintf(2, "fat32missing: FAIL: native root unavailable\n");
    exit(1);
  }
  if(stat("/fat", &st) == 0){
    fprintf(2, "fat32missing: FAIL: unsupported volume mounted\n");
    exit(1);
  }
  printf("fat32missing: PASS\n");
  exit(0);
}
