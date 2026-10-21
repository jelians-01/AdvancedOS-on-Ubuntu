#include "vm-table.h"

#define FLAGS (VM_VALID | VM_WRITE | VM_USER | VM_NR | VM_NX)

static int flags_valid(uint64_t flags)
{
  return !(flags & ~FLAGS) &&
         (!(flags & VM_USER) || (flags & VM_USER) == VM_USER);
}

uintptr_t
vm_physical(const void *pointer)
{
#ifdef __loongarch__
  return (uintptr_t)pointer & UINT64_C(0x0000ffffffffffff);
#else
  return (uintptr_t)pointer;
#endif
}

void *
vm_direct(uintptr_t physical)
{
#ifdef __loongarch__
  return (void *)(physical | UINT64_C(0x9000000000000000));
#else
  return (void *)physical;
#endif
}

int
vm_init(struct probe_vm *vm)
{
  if (!vm || vm->root)
    return -1;
  vm->root = probe_page_alloc();
  return vm->root ? 0 : -1;
}

static uint64_t *
leaf(const struct probe_vm *vm, uintptr_t va)
{
  if (!vm || !vm->root || va >= VM_LIMIT)
    return NULL;
  uint64_t *table = vm->root;
  for (int shift = 32; shift >= 23; shift -= 9) {
    uint64_t entry = table[(va >> shift) & 511];
    if (!(entry & VM_VALID))
      return NULL;
    table = vm_direct(entry & VM_PHYS_MASK);
  }
  return &table[(va >> VM_PAGE_SHIFT) & 511];
}

int
vm_map(struct probe_vm *vm, uintptr_t va, uintptr_t pa, uint64_t flags)
{
  if (!vm || !vm->root || va >= VM_LIMIT || va % VM_PAGE_SIZE ||
      pa % VM_PAGE_SIZE || (pa & ~VM_PHYS_MASK) || !flags_valid(flags))
    return -1;
  uint64_t *table = vm->root;
  uint64_t *created_slots[2];
  void *created_pages[2];
  size_t created = 0;
  for (int shift = 32; shift >= 23; shift -= 9) {
    uint64_t *slot = &table[(va >> shift) & 511];
    if (!*slot) {
      void *page = probe_page_alloc();
      if (!page) {
        while (created) {
          --created;
          *created_slots[created] = 0;
          probe_page_free(created_pages[created]);
        }
        return -1;
      }
      created_slots[created] = slot;
      created_pages[created++] = page;
      *slot = vm_physical(page) | VM_VALID;
    }
    table = vm_direct(*slot & VM_PHYS_MASK);
  }
  uint64_t *slot = &table[(va >> VM_PAGE_SHIFT) & 511];
  if (*slot)
    return -1;
  *slot = pa | flags | UINT64_C(0x10); /* coherent cached MAT, PLV0 */
  return 0;
}

uint64_t
vm_lookup(const struct probe_vm *vm, uintptr_t va)
{
  uint64_t *slot = leaf(vm, va);
  return slot ? *slot : 0;
}

int
vm_protect(struct probe_vm *vm, uintptr_t va, uint64_t flags)
{
  if (va % VM_PAGE_SIZE || !flags_valid(flags))
    return -1;
  uint64_t *slot = leaf(vm, va);
  if (!slot || !*slot)
    return -1;
  *slot = (*slot & VM_PHYS_MASK) | flags | UINT64_C(0x10);
  return 0;
}

int
vm_unmap(struct probe_vm *vm, uintptr_t va)
{
  if (va % VM_PAGE_SIZE)
    return -1;
  uint64_t *slot = leaf(vm, va);
  if (!slot || !*slot)
    return -1;
  *slot = 0;
  return 0;
}

void
vm_destroy(struct probe_vm *vm)
{
  if (!vm || !vm->root)
    return;
  for (size_t i = 0; i < 512; ++i) {
    if (!vm->root[i])
      continue;
    uint64_t *middle = vm_direct(vm->root[i] & VM_PHYS_MASK);
    for (size_t j = 0; j < 512; ++j)
      if (middle[j])
        probe_page_free(vm_direct(middle[j] & VM_PHYS_MASK));
    probe_page_free(middle);
  }
  probe_page_free(vm->root);
  vm->root = NULL;
}

void *vm_frame_alloc(void)
{
  return probe_pages_alloc_aligned(VM_PAGE_SIZE / PROBE_PAGE_SIZE,
                                   VM_PAGE_SIZE / PROBE_PAGE_SIZE);
}

void vm_frame_free(void *frame)
{
  if (!frame)
    return;
  for (size_t i = 0; i < VM_PAGE_SIZE / PROBE_PAGE_SIZE; ++i)
    probe_page_free((unsigned char *)frame + i * PROBE_PAGE_SIZE);
}
