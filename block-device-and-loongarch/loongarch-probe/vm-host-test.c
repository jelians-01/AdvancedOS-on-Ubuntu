#include <stdio.h>
#include "vm-table.h"

static unsigned char pool[PROBE_PAGE_SIZE * PROBE_MAX_PAGES]
    __attribute__((aligned(PROBE_PAGE_SIZE)));
static void *held[PROBE_MAX_PAGES];
#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr,"vm-host: FAIL line %d\n",__LINE__); return 1; } } while (0)

int main(void)
{
  struct probe_vm vm = {0};
  CHECK(probe_pages_init(pool,pool + sizeof pool) == 0);
  CHECK(!probe_pages_alloc_aligned(0,4) && !probe_pages_alloc_aligned(4,3) &&
        !probe_pages_alloc_aligned(PROBE_MAX_PAGES + 1,4) &&
        !probe_pages_alloc_aligned(4,PROBE_MAX_PAGES * 2));
  for (size_t i = 0; i < PROBE_MAX_PAGES; ++i)
    held[i] = probe_page_alloc();
  for (size_t i = 0; i < PROBE_MAX_PAGES; i += 2)
    CHECK(probe_page_free(held[i]) == 0);
  CHECK(probe_pages_available() == PROBE_MAX_PAGES / 2);
  CHECK(vm_frame_alloc() == NULL);
  CHECK(probe_pages_available() == PROBE_MAX_PAGES / 2);
  for (size_t i = 1; i < PROBE_MAX_PAGES; i += 2)
    CHECK(probe_page_free(held[i]) == 0);
  unsigned char *frame = vm_frame_alloc();
  CHECK(frame && !((uintptr_t)frame % VM_PAGE_SIZE));
  for (size_t i = 0; i < VM_PAGE_SIZE; ++i) {
    CHECK(frame[i] == 0);
    frame[i] = 0x5a;
  }
  vm_frame_free(frame);
  frame = vm_frame_alloc();
  CHECK(frame);
  for (size_t i = 0; i < VM_PAGE_SIZE; ++i)
    CHECK(frame[i] == 0);
  vm_frame_free(frame);
  CHECK(probe_pages_available() == PROBE_MAX_PAGES);
  puts("vm-host: aligned contiguous frames fragmentation clearing PASS");
  CHECK(vm_init(&vm) == 0 && vm_init(&vm) == -1);
  uintptr_t pa = vm_physical(vm_frame_alloc());
  CHECK(pa != 0);
  size_t before = probe_pages_available();
  CHECK(vm_map(&vm,1,pa,VM_VALID) == -1);
  CHECK(vm_map(&vm,VM_LIMIT,pa,VM_VALID) == -1);
  CHECK(vm_map(&vm,0,pa + 1,VM_VALID) == -1);
  CHECK(vm_map(&vm,0,UINTPTR_MAX & ~(PROBE_PAGE_SIZE - 1),VM_VALID) == -1);
  CHECK(vm_map(&vm,0,pa,UINT64_C(4)) == -1);
  CHECK(probe_pages_available() == before);
  uintptr_t addresses[] = {0,VM_PAGE_SIZE,0x7fc000,0x800000,UINT64_C(0x100000000),
                           UINT64_C(0x200000000),VM_LIMIT - VM_PAGE_SIZE};
  for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
    uintptr_t va = addresses[i];
    CHECK(vm_map(&vm,va,pa,VM_VALID|VM_WRITE|VM_NX) == 0);
    CHECK(vm_map(&vm,va,pa,VM_VALID) == -1);
    CHECK((vm_lookup(&vm,va + 31) & VM_PHYS_MASK) == pa);
    CHECK(vm_protect(&vm,va,VM_VALID|VM_NR) == 0);
    CHECK((vm_lookup(&vm,va) & (VM_VALID|VM_WRITE|VM_NR|VM_NX)) == (VM_VALID|VM_NR));
  }
  CHECK(vm_protect(&vm,1,VM_VALID) == -1);
  CHECK(vm_protect(&vm,0,4) == -1);
  CHECK(vm_lookup(&vm,VM_LIMIT) == 0);
  CHECK(vm_unmap(&vm,VM_PAGE_SIZE) == 0 && vm_unmap(&vm,VM_PAGE_SIZE) == -1);
  CHECK(vm_lookup(&vm,VM_PAGE_SIZE) == 0 && vm_unmap(&vm,1) == -1);
  puts("vm-host: mapping bounds aliases protection unmap PASS");
  /* Leave exactly one free page, but request a new two-page table branch. */
  size_t count = 0;
  while (probe_pages_available() > 1)
    held[count++] = probe_page_alloc();
  CHECK(vm_map(&vm,UINT64_C(0x300000000),pa,VM_VALID) == -1);
  CHECK(probe_pages_available() == 1);
  CHECK(vm_lookup(&vm,UINT64_C(0x300000000)) == 0);
  for (size_t i = 0; i < count; ++i)
    CHECK(probe_page_free(held[i]) == 0);
  puts("vm-host: allocation failure rollback PASS");
  vm_destroy(&vm);
  vm_destroy(&vm);
  CHECK(vm.root == NULL && vm_lookup(&vm,0) == 0);
  vm_frame_free(vm_direct(pa));
  CHECK(probe_pages_available() == PROBE_MAX_PAGES);
  puts("vm-host: table reclamation ALL PASS");
  return 0;
}
