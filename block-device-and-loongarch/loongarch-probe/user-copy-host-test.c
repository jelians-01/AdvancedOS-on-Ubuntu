#include <stdio.h>
#include "user-copy.h"
static unsigned char pool[PROBE_PAGE_SIZE * PROBE_MAX_PAGES]
    __attribute__((aligned(PROBE_PAGE_SIZE)));
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"user-copy: FAIL line %d\n",__LINE__); return 1; } } while (0)
int main(void)
{
  struct probe_vm vm = {0};
  CHECK(probe_pages_init(pool,pool + sizeof pool) == 0 && vm_init(&vm) == 0);
  unsigned char *a = vm_frame_alloc(), *b = vm_frame_alloc();
  CHECK(a && b);
  CHECK(vm_map(&vm,0x10000,vm_physical(a),VM_VALID|VM_USER|VM_WRITE|VM_NX) == 0);
  CHECK(vm_map(&vm,0x14000,vm_physical(b),VM_VALID|VM_USER|VM_WRITE|VM_NX) == 0);
  unsigned char input[16], output[16];
  for (size_t i = 0; i < sizeof input; ++i) input[i] = (unsigned char)(i + 1);
  CHECK(user_copyout(&vm,0x13ff8,input,sizeof input) == 0);
  CHECK(user_copyin(&vm,output,0x13ff8,sizeof output) == 0);
  for (size_t i = 0; i < sizeof input; ++i) CHECK(output[i] == input[i]);
  CHECK(a[VM_PAGE_SIZE - 1] == 8 && b[0] == 9);
  puts("user-copy: cross-page input/output PASS");
  CHECK(vm_protect(&vm,0x14000,VM_VALID|VM_USER|VM_NX) == 0);
  input[0] = 0xff;
  CHECK(user_copyout(&vm,0x13ff8,input,sizeof input) == -1);
  CHECK(a[VM_PAGE_SIZE - 8] == 1 && b[0] == 9);
  CHECK(user_copyout(&vm,0x13ff8,input,SIZE_MAX) == -1);
  CHECK(user_copyin(&vm,output,UINTPTR_MAX,16) == -1);
  CHECK(user_copyin(&vm,NULL,0x10000,1) == -1);
  CHECK(user_copyin(&vm,NULL,0x10000,0) == 0);
  CHECK(vm_protect(&vm,0x14000,VM_VALID|VM_USER|VM_NR|VM_WRITE) == 0);
  for (size_t i = 0; i < sizeof output; ++i) output[i] = 0xaa;
  CHECK(user_copyin(&vm,output,0x13ff8,sizeof output) == -1);
  for (size_t i = 0; i < sizeof output; ++i) CHECK(output[i] == 0xaa);
  CHECK(vm_protect(&vm,0x14000,VM_VALID|VM_WRITE) == 0);
  CHECK(user_copyin(&vm,output,0x14000,1) == -1);
  CHECK(user_copyout(&vm,0x14000,input,1) == -1);
  CHECK(vm_unmap(&vm,0x14000) == 0);
  CHECK(user_copyin(&vm,output,0x14000,1) == -1);
  puts("user-copy: permissions overflow and no partial copies PASS");
  vm_destroy(&vm); vm_frame_free(a); vm_frame_free(b);
  CHECK(probe_pages_available() == PROBE_MAX_PAGES);
  puts("user-copy: ALL PASS");
  return 0;
}
