#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  if(sync() < 0){
    fprintf(2, "sync: failed\n");
    exit(1);
  }
  printf("sync: OK\n");
  exit(0);
}
