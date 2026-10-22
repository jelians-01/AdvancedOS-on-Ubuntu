#ifndef PROBE_USER_COPY_H
#define PROBE_USER_COPY_H
#include "vm-table.h"

/* Single CPU; caller must hold mappings stable for the full check and copy.
 * Validate the entire range before modifying the destination.
 */
int user_copyin(const struct probe_vm *, void *, uintptr_t, size_t);
int user_copyout(const struct probe_vm *, uintptr_t, const void *, size_t);
#endif
