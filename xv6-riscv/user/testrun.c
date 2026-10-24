#include "kernel/types.h"
#include "user/user.h"

// xv6 sh does not print child exit codes. Give the host an unambiguous result.
int
main(int argc, char **argv)
{
  int pid, status = -1;
  if(argc < 2){
    fprintf(2, "usage: testrun program [arguments ...]\n");
    exit(2);
  }
  pid = fork();
  if(pid < 0){
    fprintf(2, "TESTRUN %s status=-1\n", argv[1]);
    exit(1);
  }
  if(pid == 0){
    exec(argv[1], argv + 1);
    fprintf(2, "testrun: exec %s failed\n", argv[1]);
    exit(127);
  }
  if(wait(&status) != pid)
    status = -1;
  printf("TESTRUN %s status=%d\n", argv[1], status);
  exit(status == 0 ? 0 : 1);
}
