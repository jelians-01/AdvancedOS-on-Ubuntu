#include "user-copy.h"

static int range_ok(const struct probe_vm *vm, uintptr_t address,
                    size_t length, int writing)
{
  if (address >= VM_LIMIT || length > VM_LIMIT - address)
    return -1;
  while (length) {
    uint64_t pte = vm_lookup(vm,address);
    if (!(pte & VM_VALID) || (pte & VM_USER) != VM_USER ||
        (writing ? !(pte & VM_WRITE) : (pte & VM_NR)))
      return -1;
    size_t step = VM_PAGE_SIZE - address % VM_PAGE_SIZE;
    if (step > length)
      step = length;
    address += step;
    length -= step;
  }
  return 0;
}

int user_copyin(const struct probe_vm *vm, void *output,
                uintptr_t address, size_t length)
{
  if ((!output && length) || range_ok(vm,address,length,0))
    return -1;
  unsigned char *to = output;
  while (length) {
    size_t offset = address % VM_PAGE_SIZE;
    size_t step = VM_PAGE_SIZE - offset;
    if (step > length)
      step = length;
    const unsigned char *from = vm_direct(vm_lookup(vm,address) & VM_PHYS_MASK);
    for (size_t i = 0; i < step; ++i)
      to[i] = from[offset + i];
    to += step; address += step; length -= step;
  }
  return 0;
}

int user_copyout(const struct probe_vm *vm, uintptr_t address,
                 const void *input, size_t length)
{
  if ((!input && length) || range_ok(vm,address,length,1))
    return -1;
  const unsigned char *from = input;
  while (length) {
    size_t offset = address % VM_PAGE_SIZE;
    size_t step = VM_PAGE_SIZE - offset;
    if (step > length)
      step = length;
    unsigned char *to = vm_direct(vm_lookup(vm,address) & VM_PHYS_MASK);
    for (size_t i = 0; i < step; ++i)
      to[offset + i] = from[i];
    from += step; address += step; length -= step;
  }
  return 0;
}
