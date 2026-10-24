#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

static int __attribute__((noinline))
answer(void)
{
  return 42;
}

static void
isolate(int which)
{
  int child = fork(), status = 0;
  if(child < 0){
    fprintf(2, "archtest: fork failed\n");
    exit(1);
  }
  if(child == 0){
    if(which == 0){
      volatile uchar *text = (volatile uchar *)answer;
      *text = 0;
    } else if(which == 1){
      char *heap = malloc(128);
      if(heap == 0)
        exit(2);
      memmove(heap, (char *)answer, 128);
      int value = ((int (*)(void))heap)();
      exit(value == 42 ? 42 : 2);
    } else {
#ifdef ARCH_LOONGARCH
      volatile uint64 *kernel = (volatile uint64 *)0x9000000000200000UL;
#else
      volatile uint64 *kernel = (volatile uint64 *)0x80000000UL;
#endif
      volatile uint64 value = *kernel;
      (void)value;
    }
    exit(42);
  }
  if(wait(&status) != child || status != -1){
    fprintf(2, "archtest: isolation case %d failed status=%d\n", which, status);
    exit(1);
  }
}

int
main(void)
{
  if(answer() != 42)
    exit(1);
  for(int i = 0; i < 3; i++)
    isolate(i);
  printf("archtest: read-only text, NX heap, kernel isolation PASS\n");
  exit(0);
}
