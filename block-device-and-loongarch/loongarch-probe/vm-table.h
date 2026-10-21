#ifndef PROBE_VM_TABLE_H
#define PROBE_VM_TABLE_H

#include "page-allocator.h"

#define VM_VALID UINT64_C(1)
#define VM_WRITE UINT64_C(2)
#define VM_NR (UINT64_C(1) << 61)
#define VM_NX (UINT64_C(1) << 62)
#define VM_PHYS_MASK UINT64_C(0x0000fffffffff000)
#define VM_PAGE_SHIFT 14
#define VM_PAGE_SIZE (UINT64_C(1) << VM_PAGE_SHIFT)
#define VM_LIMIT (UINT64_C(1) << 41)

/* Experiment-private three-level software walk: 9/9/9 bits, 16 KiB leaves.
 * Directory tables themselves use one 4 KiB allocator page, 512 entries.
 * Directory entries hold physical child-table addresses plus bit 0.
 * Leaves use LoongArch TLBELO encoding, MAT=1, PLV=0.
 * These are not RISC-V PTEs and do not configure hardware page walking.
 * The caller invalidates the TLB after any change and serializes all calls.
 */
struct probe_vm { uint64_t *root; };
uintptr_t vm_physical(const void *pointer);
void *vm_direct(uintptr_t physical);
int vm_init(struct probe_vm *vm);
int vm_map(struct probe_vm *vm, uintptr_t va, uintptr_t pa, uint64_t flags);
uint64_t vm_lookup(const struct probe_vm *vm, uintptr_t va);
int vm_protect(struct probe_vm *vm, uintptr_t va, uint64_t flags);
int vm_unmap(struct probe_vm *vm, uintptr_t va);
void vm_destroy(struct probe_vm *vm);
void *vm_frame_alloc(void);
void vm_frame_free(void *frame);

#endif
